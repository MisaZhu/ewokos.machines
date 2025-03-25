#include <Widget/WidgetWin.h>
#include <Widget/Label.h>
#include <x++/X.h>
#include <unistd.h>
#include <string>
#include <fcntl.h>
#include <font/font.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/basic_math.h>
#include <ewoksys/keydef.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <upng/upng.h>
#include <xgo/xgo.h>

using namespace Ewok;

class XgoWidget: public Widget {
	int32_t bt;
	bool demoing;
	uint8_t expressStep;
	uint8_t actionStep;

	static const uint8_t BATT_NUM = 10;
	graph_t* battIcons[BATT_NUM];

	static const uint8_t EXPR_NUM = 39;
	graph_t* expressIcons[EXPR_NUM];
	bool expressLoaded;

	void loadBattIcons() {
		for(int i=0; i<BATT_NUM; i++) {
			char name[32];
			snprintf(name, 31, "batt/batt%d.png", i);
			battIcons[i] = png_image_new(X::getResName(name));
		}
	}
	
	void loadExpressIcons() {
		for(int i=0; i<EXPR_NUM; i++) {
			char name[32];
			snprintf(name, 31, "express/%d.png", i+1);
			graph_t* img = png_image_new(X::getResName(name));
			expressIcons[i] = img;
		}
	}

	void bored() {
		static const uint8_t actNum = 2;
		static uint8_t acts[actNum] = {XGO_ACT_SHAKE, XGO_ACT_HEIGHT};
		uint8_t act = acts[random_to(actNum)];
		klog("bored... ACT: %d\n", act);
		xgo_cmd(XGO_TYPE_SEND, XGO_CMD_ACT, act, NULL);
	}
	
protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		if(bt < 0)
			return;

		graph_fill(g, r.x, r.y, r.w, r.h, 0xff000000);
		graph_t* img = NULL;

		if(expressLoaded) {
			img = expressIcons[expressStep];
			if(img != NULL) {
				graph_blt_alpha(img, 0, 0, img->w, img->h,
						g, r.x + (r.w-img->w)/2, r.y + (r.h-img->h)/2, img->w, img->h, 0xff);
			}
		}

		int8_t i = (bt / 10) - 1;
		if(i < 0)
			i = 0;
		if(i > 9)
			i = 9;

		img = battIcons[i];
		graph_blt_alpha(img, 0, 0, img->w, img->h,
				g, r.x+r.w-img->w-4, r.y+4, img->w, img->h, 0xff);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerStep) {
		if(timerStep == 0) {
			xgo_cmd(XGO_TYPE_SEND, XGO_CMD_SET_FORCE_RT, 0x0, NULL);
			//xgo_cmd(XGO_TYPE_SEND, XGO_CMD_SET_FORCE_ROLL, 0x0, NULL);
		}
		else if(timerStep == 1) {
			loadExpressIcons();
			expressLoaded = true;
		}
		else if((timerStep % (timerFPS*60)) == 0) {
			bored();
		}

		uint8_t res[XGO_DATA_MAX];
		if(xgo_cmd(XGO_TYPE_READ, XGO_CMD_GET_BATT, 1, res) == 0)
			bt = res[0] & 0xff;

		update();

		expressStep++;
		if(expressStep >= EXPR_NUM)
			expressStep = 0;
	}

	bool onIM(xevent_t* ev) {
		if(ev->state != XIM_STATE_RELEASE)
			return false;

		if(ev->value.im.value == KEY_BUTTON_A) {
			if(demoing)
				xgo_cmd(XGO_TYPE_SEND, XGO_CMD_DEMO, 0x0, NULL);
			else
				xgo_cmd(XGO_TYPE_SEND, XGO_CMD_DEMO, 0x1, NULL);
			demoing = !demoing;
			return true;
		}
		else if(ev->value.im.value == KEY_RIGHT) {
			xgo_cmd(XGO_TYPE_SEND, XGO_CMD_ACT, actionStep, NULL);
			actionStep++;
			if(actionStep >= XGO_ACT_MAX)
				actionStep = 0;
			return true;
		}
		else if(ev->value.im.value == KEY_LEFT) {
			xgo_cmd(XGO_TYPE_SEND, XGO_CMD_ACT, XGO_ACT_STOP, NULL);
			return true;
		}

		return false;
	}

public: 
	~XgoWidget() {
		xgo_quit();
		for(int i=0; i<BATT_NUM; i++) {
			if(battIcons[i] != NULL)
				graph_free(battIcons[i]);
		}
		
		for(int i=0; i<EXPR_NUM; i++) {
			if(expressIcons[i] != NULL)
				graph_free(expressIcons[i]);
		}
	}

	XgoWidget() {
		bt = 0;
		expressStep = 0;
		actionStep = 0;
		demoing = false;
		expressLoaded = false;

		xgo_init();
		loadBattIcons();

		for(int i=0; i<EXPR_NUM; i++)
			expressIcons[i] = NULL;
	}
};

int main(int argc, char** argv) {
	X x;
	WidgetWin win;

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::HORIZONTAL);

	XgoWidget* xgoW = new XgoWidget();
	root->add(xgoW);
	root->focus(xgoW);

	win.open(&x, 0, -1, -1, 240, 240, "xgo", XWIN_STYLE_NORMAL);
	win.setTimer(8);
	win.max();
	x.run(NULL, &win);
	return 0;
}