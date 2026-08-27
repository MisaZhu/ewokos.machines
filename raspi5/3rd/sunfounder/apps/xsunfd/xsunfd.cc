/*
 * xsunfd.cc
 *
 * SunFounder Pironman 5 control panel. Drives everything through the
 * dev.cmd interface (dev_cmd()):
 *
 *   /dev/light  spilightd  - 4x WS2812 RGB: on/off, color, bright,
 *                            speed, led count, effect mode
 *   /dev/fan    fand       - cooling fan: level 0-10, raw duty %, rev
 *   /dev/cpu    cpud       - SoC temperature (JSON snapshot)
 *
 * Layout: left column is the light panel (on/off + color button on top,
 * bright/speed/leds sliders, mode list below), right column shows CPU
 * temperature and the fan panel (level/duty sliders + rev toggle).
 * All range controls are CmdSlider: the command is sent on mouse
 * release, not on every drag step, so dragging does not flood the
 * driver with IPC roundtrips.
 */

#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <Widget/Label.h>
#include <Widget/LabelButton.h>
#include <Widget/List.h>
#include <Widget/Slider.h>
#include <Widget/Scroller.h>
#include <Widget/Splitter.h>
#include <Widget/Blank.h>
#include <WidgetEx/ColorDialog.h>

#include <x++/X.h>
#include <ewoksys/vdevice.h>
#include <tinyjson/tinyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace Ewok;
using namespace std;

static const char* LIGHT_DEV = "/dev/light";
static const char* FAN_DEV   = "/dev/fan";
static const char* CPU_DEV   = "/dev/cpu";

/* same set spilightd exposes via "dev.cmd /dev/light mode <name>" */
static const char* LIGHT_MODES[] = {
	"solid", "breathing", "flow", "flow_reverse",
	"rainbow", "rainbow_reverse", "hue_cycle"
};
static const int LIGHT_MODE_NUM = 7;

static void sendCmd(const char* dev, const string& cmd) {
	char* ret = dev_cmd(dev, cmd.c_str());
	if(ret != NULL)
		free(ret);
}

struct LightState {
	bool     on;
	int      bright;
	uint32_t color;
	string   mode;
	int      speed;
	int      leds;

	LightState(): on(true), bright(50), color(0x0000ff),
		mode("solid"), speed(50), leds(4) {}
};

/* "light on bright 50 color 0000ff mode solid speed 50 leds 4" */
static bool parseLightStatus(const char* s, LightState& st) {
	char on[16], mode[16];
	int bright = 0, speed = 0, leds = 0;
	unsigned color = 0;
	if(sscanf(s, "light %15s bright %d color %x mode %15s speed %d leds %d",
			on, &bright, &color, mode, &speed, &leds) != 6)
		return false;
	st.on = strcmp(on, "on") == 0;
	st.bright = bright;
	st.color = color & 0xffffff;
	st.mode = mode;
	st.speed = speed;
	st.leds = leds;
	return true;
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

/*
 * Slider bound to one "<prefix> <value>" dev.cmd. While dragging only
 * the value label follows the knob; the command goes out on mouse
 * release. syncValue() moves the knob from device state without echoing
 * a command back to the driver.
 */
class CmdSlider: public Slider {
	string dev;
	string prefix;
	int base;            /* value = slider position + base (leds: base 1) */
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

class ModeList: public List {
	bool guard;
protected:
	void drawItem(graph_t* g, XTheme* theme, int32_t index, const grect_t& r) {
		if(index < 0 || index >= LIGHT_MODE_NUM)
			return;
		uint32_t fg = theme->basic.fgColor;
		if(index == itemSelected) {
			graph_fill_rect(g, r.x, r.y, r.w, r.h, theme->basic.selectBGColor);
			fg = theme->basic.selectColor;
		}
		graph_draw_text_font(g, r.x+8, r.y+4, LIGHT_MODES[index],
				theme->getFont(), theme->basic.fontSize, fg);
	}

	void onSelect(int sel) {
		if(guard || sel < 0 || sel >= LIGHT_MODE_NUM)
			return;
		char cmd[48];
		snprintf(cmd, sizeof(cmd), "mode %s", LIGHT_MODES[sel]);
		sendCmd(LIGHT_DEV, cmd);
	}
public:
	ModeList() {
		guard = false;
		setItemNum(LIGHT_MODE_NUM);
		setItemSize(24);
	}

	void syncMode(const char* name) {
		for(int i = 0; i < LIGHT_MODE_NUM; i++) {
			if(strcmp(LIGHT_MODES[i], name) == 0) {
				if(itemSelected != i) {
					guard = true;
					select(i);
					guard = false;
				}
				return;
			}
		}
	}
};

class XSunfWin;

/*
 * CPU temperature bar: fill width is current/max milli-Celsius, fill
 * color lerps green->yellow->red with the same ratio.
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
			uint32_t ratio = (uint32_t)mc * 255 / full;
			if(ratio > 255)
				ratio = 255;
			uint32_t cr = ratio < 128 ? ratio * 2 : 255;
			uint32_t cg = ratio < 128 ? 255 : (255 - ratio) * 2;
			uint32_t color = 0xff000000 | (cr << 16) | (cg << 8);

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

/* current color swatch; click opens the ColorDialog to pick a new one */
class ColorPreview: public Widget {
	XSunfWin* win;
	uint32_t color;
protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		graph_fill_3d(g, r.x, r.y, r.w, r.h, theme->basic.bgColor, true);
		graph_fill_rect(g, r.x+4, r.y+4, r.w-8, r.h-8, 0xff000000 | color);
	}

	bool onMouse(xevent_t* ev); /* needs the complete XSunfWin type */
public:
	ColorPreview(XSunfWin* win) {
		this->win = win;
		color = 0x0000ff;
	}

	void setColor(uint32_t c) {
		c &= 0xffffff;
		if(color != c) {
			color = c;
			update();
		}
	}
};

class XSunfWin: public WidgetWin {
	LightState light;
	FanState fan;
	bool lightSynced;

	LabelButton* onOffBtn;
	ColorPreview* colorPreview;
	CmdSlider* brightSlider;
	CmdSlider* speedSlider;
	CmdSlider* ledSlider;
	ModeList* modeList;
	TempBar* tempBar;
	Label* fanLabel;
	CmdSlider* levelSlider;
	CmdSlider* dutySlider;
	ColorDialog colorDlg;
protected:
	void onDialoged(XWin* from, int res, void* arg) {
		(void)from;
		(void)arg;
		if(res != Dialog::RES_OK)
			return;
		char cmd[48];
		snprintf(cmd, sizeof(cmd), "color %06x",
				(unsigned)(colorDlg.getColor() & 0xffffff));
		char* ret = dev_cmd(LIGHT_DEV, cmd);
		if(ret == NULL)
			return;
		if(parseLightStatus(ret, light))
			applyLight();
		free(ret);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerSteps) {
		/*keep retrying until the driver answered once with the
		  layout already in place (sliders need a sized area)*/
		if(!lightSynced)
			syncLight();
		if(timerSteps % timerFPS == 0)
			pollFan();
		if(timerSteps % (timerFPS * 2) == 0)
			pollCpu();
	}
public:
	XSunfWin() {
		lightSynced = false;
		onOffBtn = NULL;
		colorPreview = NULL;
		brightSlider = speedSlider = ledSlider = NULL;
		modeList = NULL;
		tempBar = NULL;
		fanLabel = NULL;
		levelSlider = dutySlider = NULL;
	}

	void setWidgets(LabelButton* onOff, ColorPreview* preview,
			CmdSlider* bright, CmdSlider* speed, CmdSlider* leds,
			ModeList* modes, TempBar* temp, Label* fanInfo,
			CmdSlider* level, CmdSlider* duty) {
		onOffBtn = onOff;
		colorPreview = preview;
		brightSlider = bright;
		speedSlider = speed;
		ledSlider = leds;
		modeList = modes;
		tempBar = temp;
		fanLabel = fanInfo;
		levelSlider = level;
		dutySlider = duty;
	}

	static void onOffClick(Widget* wd, xevent_t* evt, void* arg) {
		(void)wd;
		if(evt->type != XEVT_MOUSE || evt->state != MOUSE_STATE_CLICK)
			return;
		((XSunfWin*)arg)->toggleLight();
	}

	static void revClick(Widget* wd, xevent_t* evt, void* arg) {
		(void)wd;
		if(evt->type != XEVT_MOUSE || evt->state != MOUSE_STATE_CLICK)
			return;
		XSunfWin* win = (XSunfWin*)arg;
		sendCmd(FAN_DEV, "rev");
		win->pollFan();
	}

	void openColorDialog() {
		colorDlg.setColor(0xff000000 | light.color);
		colorDlg.popup(this, 320, 240, "light color", XWIN_STYLE_NO_TITLE);
	}

	void applyLight() {
		onOffBtn->setLabel(light.on ? "light: ON" : "light: OFF");
		colorPreview->setColor(light.color);
		bool ok = brightSlider->syncValue(light.bright);
		ok = speedSlider->syncValue(light.speed) && ok;
		ok = ledSlider->syncValue(light.leds) && ok;
		modeList->syncMode(light.mode.c_str());
		lightSynced = ok;
	}

	void syncLight() {
		char* ret = dev_cmd(LIGHT_DEV, "status");
		if(ret == NULL)
			return;
		if(parseLightStatus(ret, light))
			applyLight();
		free(ret);
	}

	void toggleLight() {
		char* ret = dev_cmd(LIGHT_DEV, light.on ? "off" : "on");
		if(ret == NULL)
			return;
		if(parseLightStatus(ret, light))
			applyLight();
		free(ret);
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
		json_var_unref(obj);
	}
};

bool ColorPreview::onMouse(xevent_t* ev) {
	if(ev->state == MOUSE_STATE_CLICK && win != NULL)
		win->openColorDialog();
	return true;
}

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
	XSunfWin win;

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::HORIZONTAL);
	root->setAlpha(false);

	/* left: light panel */
	Container* left = new Container();
	left->setType(Container::VERTICAL);
	root->add(left);

	Container* row = new Container();
	row->setType(Container::HORIZONTAL);
	row->fix(0, 36);
	left->add(row);

	LabelButton* onOff = new LabelButton("light: ?");
	onOff->fix(96, 0);
	onOff->setEventFunc(XSunfWin::onOffClick, &win);
	row->add(onOff);

	Blank* gap = new Blank();
	gap->fix(6, 0);
	row->add(gap);

	ColorPreview* preview = new ColorPreview(&win);
	row->add(preview);

	CmdSlider* bright = addSliderRow(left, "bright", 52, LIGHT_DEV, "bright", 0, 101, "%");
	CmdSlider* speed  = addSliderRow(left, "speed", 52, LIGHT_DEV, "speed", 0, 101, "%");
	/* Pironman 5 has 4 WS2812 on the IO board; the driver's num command
	   accepts up to 32 for daisy-chained strips, the panel caps at 4 */
	CmdSlider* leds   = addSliderRow(left, "leds", 52, LIGHT_DEV, "num", 1, 4, "");

	Label* modeTitle = new Label("mode");
	modeTitle->fix(0, 20);
	left->add(modeTitle);

	Container* listRow = new Container();
	listRow->setType(Container::HORIZONTAL);
	left->add(listRow);

	ModeList* modes = new ModeList();
	listRow->add(modes);

	Scroller* scroller = new Scroller();
	scroller->fix(8, 0);
	listRow->add(scroller);
	modes->setScrollerV(scroller);

	Splitter* splitter = new Splitter();
	splitter->attach(left);
	root->add(splitter);

	/* right: cpu temperature + fan panel; not fixed-width, shares the
	   window evenly with the left column and follows window resizes */
	Container* right = new Container();
	right->setType(Container::VERTICAL);
	root->add(right);

	Label* cpuTitle = new Label("CPU");
	cpuTitle->fix(0, 20);
	right->add(cpuTitle);

	TempBar* temp = new TempBar();
	temp->fix(0, 32);
	right->add(temp);

	Label* fanTitle = new Label("FAN");
	fanTitle->fix(0, 20);
	right->add(fanTitle);

	Label* fanInfo = new Label("fan: -");
	fanInfo->fix(0, 24);
	right->add(fanInfo);

	CmdSlider* level = addSliderRow(right, "level", 40, FAN_DEV, "run", 0, 11, "");
	CmdSlider* duty  = addSliderRow(right, "duty", 40, FAN_DEV, "duty", 0, 101, "%");

	LabelButton* rev = new LabelButton("rev");
	rev->fix(0, 30);
	rev->setEventFunc(XSunfWin::revClick, &win);
	right->add(rev);

	win.setWidgets(onOff, preview, bright, speed, leds, modes,
			temp, fanInfo, level, duty);

	win.open(&x, 0, -1, -1, 520, 360, "xsunfd",
			XWIN_STYLE_NORMAL | XWIN_STYLE_NO_BG_EFFECT);
	win.setTimer(4);

	widgetXRun(&x, &win);
	return 0;
}
