/*
 * xcpuinfo.cc
 *
 * Raspberry Pi 5 CPU / cooling panel: SoC temperature bar plus fan
 * control, everything through the dev.cmd interface (dev_cmd()):
 *
 *   /dev/cpu  cpud - SoC temperature (JSON snapshot)
 *   /dev/fan  fand - cooling fan: level 0-10, raw duty %, rev, rpm
 *
 * All rows only fix their height, so every widget follows the window
 * width; the temperature bar is the flexible child and also absorbs
 * height changes. Range controls are CmdSlider: the command is sent on
 * mouse release, not on every drag step, so dragging does not flood the
 * driver with IPC roundtrips.
 */

#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <Widget/Label.h>
#include <Widget/LabelButton.h>
#include <Widget/Slider.h>

#include <x++/X.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/sys.h>
#include <sysinfo.h>
#include <tinyjson/tinyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace Ewok;
using namespace std;

static const char* FAN_DEV = "/dev/fan";
static const char* CPU_DEV = "/dev/cpu";

static void sendCmd(const char* dev, const string& cmd) {
	char* ret = dev_cmd(dev, cmd.c_str());
	if(ret != NULL)
		free(ret);
}

struct FanState {
	bool     manual;  /* manual duty mode vs level mode */
	int      value;   /* level (0-10) or duty pct, depending on mode */
	int      duty;    /* percent */
	unsigned rpm;
};

/* "fan level 3 duty 30% rpm 1200" / "fan manual 55 duty 55% rpm 1200" */
static bool parseFanStatus(const char* s, FanState& st) {
	char mode[16];
	int value = 0, duty = 0;
	unsigned rpm = 0;
	if(sscanf(s, "fan %15s %d duty %d%% rpm %u", mode, &value, &duty, &rpm) != 4)
		return false;
	st.manual = strcmp(mode, "manual") == 0;
	st.value = value;
	st.duty = duty;
	st.rpm = rpm;
	return true;
}

/* find the array entry whose "name" member matches */
static json_var_t* findNamedVar(json_var_t* arr, const char* name) {
	if(arr == NULL)
		return NULL;
	uint32_t n = json_var_array_size(arr);
	for(uint32_t i = 0; i < n; i++) {
		json_var_t* item = json_var_array_get_var(arr, i);
		if(item != NULL &&
				strcmp(json_get_str_def(item, "name", ""), name) == 0)
			return item;
	}
	return NULL;
}

/*
 * Slider bound to one "<prefix> <value>" dev.cmd. While dragging only
 * the value label follows the knob; the command goes out on mouse
 * release. syncValue() moves the knob from device state without echoing
 * a command back to the driver.
 */
class CmdSlider: public Slider {
	string dev;
	string prefix;
	int base;            /* value = slider position + base */
	const char* suffix;
	Label* valueLabel;
	bool pending;        /* dragged since last send */
	bool guard;          /* set while syncing from device state */

	int value() { return (int)getValue() + base; }

	void showValue(int v) {
		if(valueLabel == NULL)
			return;
		char s[16];
		snprintf(s, sizeof(s), "%d%s", v, suffix);
		valueLabel->setLabel(s);
	}

	void send() {
		char cmd[48];
		snprintf(cmd, sizeof(cmd), "%s %d", prefix.c_str(), value());
		sendCmd(dev.c_str(), cmd);
	}
protected:
	void onPosChange() {
		if(guard)
			return;
		showValue(value());
		if(isDragging) {
			pending = true;
			return;
		}
		send();
	}

	bool onMouse(xevent_t* ev) {
		bool ret = Slider::onMouse(ev);
		if(ev->state == MOUSE_STATE_UP && pending) {
			pending = false;
			send();
		}
		return ret;
	}
public:
	CmdSlider(const char* dev, const char* prefix, int base,
			uint32_t range, const char* suffix = "") {
		this->dev = dev;
		this->prefix = prefix;
		this->base = base;
		this->suffix = suffix;
		valueLabel = NULL;
		pending = false;
		guard = false;
		setRange(range);
	}

	void setValueLabel(Label* label) { valueLabel = label; }

	/* false when the widget has no area yet (layout not done) */
	bool syncValue(int v) {
		if(isDragging)
			return true;
		v -= base;
		if(v < 0)
			v = 0;
		if(v > (int)range - 1)
			v = (int)range - 1;
		uint32_t max = horizontal ?
			(area.w > area.h ? area.w - area.h : 0) :
			(area.h > area.w ? area.h - area.w : 0);
		if(max == 0)
			return false;
		/* round to the nearest position so value survives the
		   pos->value round trip (Slider::setValue truncates) */
		uint32_t p = ((uint32_t)v * max + range / 2) / range;
		if(p >= max)
			p = max - 1;
		guard = true;
		setPos(p);
		guard = false;
		showValue(v + base);
		return true;
	}
};

/*
 * CPU temperature bar: fill width is current/max milli-Celsius, fill
 * color lerps dark->red over the useful window from the cool baseline
 * to the throttle temperature (dark when cool, red when hot).
 */
class TempBar: public Widget {
	int mc;     /* current temperature, milli-Celsius; <0 = unknown */
	int maxMc;  /* throttle temperature, milli-Celsius; <=0 = unknown */
protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		graph_fill_3d(g, r.x, r.y, r.w, r.h, theme->basic.bgColor, true);

		char s[48];
		if(mc >= 0) {
			uint32_t full = maxMc > 0 ? (uint32_t)maxMc : 100000;
			/* color ratio over the cool->hot window (40C .. throttle),
			   so idle temperatures show green instead of yellow */
			const uint32_t coolMc = 40000;
			uint32_t ratio = 0;
			if(full > coolMc) {
				if((uint32_t)mc > coolMc)
					ratio = ((uint32_t)mc - coolMc) * 255 / (full - coolMc);
				if(ratio > 255)
					ratio = 255;
			}
			/* lerp dark blue-grey -> red */
			uint32_t cr = 128 + (255 - 128) * ratio / 255;
			uint32_t cg = 40 * (255 - ratio) / 255;
			uint32_t cb = 64 * (255 - ratio) / 255;
			uint32_t color = 0xff000000 | (cr << 16) | (cg << 8) | cb;

			int32_t fw = (int32_t)((uint32_t)(r.w - 4) * (uint32_t)mc / full);
			if(fw < 2)
				fw = 2;
			if(fw > r.w - 4)
				fw = r.w - 4;
			graph_fill_rect(g, r.x+2, r.y+2, fw, r.h-4, color);

			if(maxMc > 0)
				snprintf(s, sizeof(s), "%d.%d / %d.%dC",
						mc/1000, (mc%1000)/10, maxMc/1000, (maxMc%1000)/10);
			else
				snprintf(s, sizeof(s), "%d.%dC", mc/1000, (mc%1000)/10);
		}
		else
			snprintf(s, sizeof(s), "cpu: n/a");

		int32_t ty = r.y + (r.h - (int32_t)theme->basic.fontSize) / 2;
		graph_draw_text_font(g, r.x+7, ty+1, s,
				theme->getFont(), theme->basic.fontSize, theme->basic.bgColor);
		graph_draw_text_font(g, r.x+6, ty, s,
				theme->getFont(), theme->basic.fontSize, theme->basic.fgColor);
	}
public:
	TempBar() {
		mc = -1;
		maxMc = 0;
	}

	void setTemp(int mc, int maxMc) {
		if(this->mc == mc && this->maxMc == maxMc)
			return;
		this->mc = mc;
		this->maxMc = maxMc;
		update();
	}
};

class XCpuInfoWin: public WidgetWin {
	TempBar* tempBar;
	Label* cpuTitle;
	Label* fanLabel;
	CmdSlider* levelSlider;
	CmdSlider* dutySlider;
	uint32_t cores;      /* cpu core count from sysinfo, 0 = unknown */
protected:
	void onTimer(uint32_t timerFPS, uint32_t timerSteps) {
		if(timerSteps % timerFPS == 0)
			pollFan();
		if(timerSteps % (timerFPS * 2) == 0)
			pollCpu();
	}
public:
	XCpuInfoWin() {
		tempBar = NULL;
		cpuTitle = NULL;
		fanLabel = NULL;
		levelSlider = dutySlider = NULL;
		cores = 0;
	}

	void setCores(uint32_t n) { cores = n; }

	void setWidgets(TempBar* temp, Label* cpuInfo, Label* fanInfo,
			CmdSlider* level, CmdSlider* duty) {
		tempBar = temp;
		cpuTitle = cpuInfo;
		fanLabel = fanInfo;
		levelSlider = level;
		dutySlider = duty;
	}

	static void revClick(Widget* wd, xevent_t* evt, void* arg) {
		(void)wd;
		if(evt->type != XEVT_MOUSE || evt->state != MOUSE_STATE_CLICK)
			return;
		XCpuInfoWin* win = (XCpuInfoWin*)arg;
		sendCmd(FAN_DEV, "rev");
		win->pollFan();
	}

	void applyFan(const FanState& st) {
		char s[48];
		if(st.manual)
			snprintf(s, sizeof(s), "manual %d%% rpm %u", st.duty, st.rpm);
		else
			snprintf(s, sizeof(s), "level %d rpm %u", st.value, st.rpm);
		fanLabel->setLabel(s);
		dutySlider->syncValue(st.duty);
		if(!st.manual)
			levelSlider->syncValue(st.value);
	}

	void pollFan() {
		char* ret = dev_cmd(FAN_DEV, "status");
		if(ret == NULL)
			return;
		FanState st;
		if(parseFanStatus(ret, st))
			applyFan(st);
		free(ret);
	}

	/* "CPU: x4, 1.5 GHz, 850.8 mV"; each part shows only when known */
	void updateCpuTitle(int armMhz, int coreUv) {
		char s[48];
		size_t n = snprintf(s, sizeof(s), "CPU");
		if(cores > 0)
			n += snprintf(s+n, sizeof(s)-n, ": x%u", (unsigned)cores);
		if(armMhz > 0) {
			if(armMhz >= 1000)
				n += snprintf(s+n, sizeof(s)-n, ", %d.%d GHz",
						armMhz / 1000, (armMhz / 100) % 10);
			else
				n += snprintf(s+n, sizeof(s)-n, ", %d MHz", armMhz);
		}
		if(coreUv > 0)
			snprintf(s+n, sizeof(s)-n, ", %d.%d mV",
					coreUv / 1000, (coreUv / 100) % 10);
		cpuTitle->setLabel(s);
	}

	void pollCpu() {
		char* ret = dev_cmd(CPU_DEV, "info");
		if(ret == NULL) {
			tempBar->setTemp(-1, 0);
			return;
		}
		json_var_t* obj = json_parse_str(ret);
		free(ret);
		if(obj == NULL)
			return;

		json_var_t* t = json_get_obj(obj, "temperature");
		if(t != NULL && json_get_bool_def(t, "current_available", false))
			tempBar->setTemp(json_get_int_def(t, "current_millic", 0),
					json_get_int_def(t, "max_millic", 0));
		else
			tempBar->setTemp(-1, 0);

		/* current_hz can exceed int range (arm up to 2.4GHz); tinyjson
		   keeps such values as float, so read through the float getter */
		json_var_t* arm = findNamedVar(json_get_obj(obj, "clocks"), "arm");
		float armHzF = (arm != NULL && json_get_bool_def(arm, "current_available", false)) ?
				json_get_float_def(arm, "current_hz", 0.0f) : 0.0f;
		int armMhz = armHzF > 0.0f ? (int)(armHzF / 1000000.0f + 0.5f) : 0;

		json_var_t* core = findNamedVar(json_get_obj(obj, "voltages"), "core");
		int coreUv = (core != NULL && json_get_bool_def(core, "current_available", false)) ?
				json_get_int_def(core, "current_uv", 0) : 0;
		if(coreUv < 0)
			coreUv = 0;
		updateCpuTitle(armMhz, coreUv);

		json_var_unref(obj);
	}
};

static CmdSlider* addSliderRow(Container* parent, const char* name, int nameW,
		const char* dev, const char* prefix, int base, uint32_t range,
		const char* suffix) {
	Container* row = new Container();
	row->setType(Container::HORIZONTAL);
	row->fix(0, 28);
	parent->add(row);

	Label* nameLabel = new Label(name);
	nameLabel->fix(nameW, 0);
	row->add(nameLabel);

	CmdSlider* slider = new CmdSlider(dev, prefix, base, range, suffix);
	row->add(slider);

	Label* valueLabel = new Label("-");
	valueLabel->fix(44, 0);
	row->add(valueLabel);
	slider->setValueLabel(valueLabel);
	return slider;
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	X x;
	XCpuInfoWin win;

	sys_info_t sysinfo;
	if(sys_get_sys_info(&sysinfo) == 0)
		win.setCores(sysinfo.cores);

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::VERTICAL);
	root->setAlpha(false);

	Label* cpuTitle = new Label("CPU");
	cpuTitle->fix(0, 20);
	root->add(cpuTitle);

	/* flexible child: absorbs the window's width and height changes */
	TempBar* temp = new TempBar();
	root->add(temp);

	Label* fanTitle = new Label("FAN");
	fanTitle->fix(0, 20);
	root->add(fanTitle);

	Label* fanInfo = new Label("fan: -");
	fanInfo->fix(0, 24);
	root->add(fanInfo);

	CmdSlider* level = addSliderRow(root, "level", 40, FAN_DEV, "run", 0, 11, "");
	CmdSlider* duty  = addSliderRow(root, "duty", 40, FAN_DEV, "duty", 0, 101, "%");

	LabelButton* rev = new LabelButton("rev");
	rev->fix(0, 30);
	rev->setEventFunc(XCpuInfoWin::revClick, &win);
	root->add(rev);

	win.setWidgets(temp, cpuTitle, fanInfo, level, duty);

	win.open(&x, 0, -1, -1, 320, 180, "xcpuinfo",
			XWIN_STYLE_NORMAL | XWIN_STYLE_NO_BG_EFFECT);
	win.setTimer(4);

	widgetXRun(&x, &win);
	return 0;
}
