#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <x++/X.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <graph/graph.h>
#include <ewoksys/keydef.h>
#include <ewoksys/vfs.h>

using namespace Ewok;

#define CAM_DEV "/dev/cam0"
#define CAM_W 640
#define CAM_H 480
#define CAM_BPP 1 /* RAW8 Bayer (SBGGR8) */
#define CAM_FRAME_SIZE (CAM_W * CAM_H * CAM_BPP)

class CamWidget: public Widget {
	int _fd;
	uint8_t* _frame;
	graph_t* _camGraph;
	bool _running;
	int _retryTick;

	/* RAW8 Bayer -> ARGB, simple 2x2 demosaic. Phase measured on-device
	 * via camd's bayer-mean diagnostic: the two similar means are the G
	 * sites (s01/s10) and the brighter site under warm light is s11,
	 * so the buffer layout is BGGR: each block [B G / G R] yields one
	 * color for its 4 pixels */
	void bayer_to_argb(const uint8_t* raw, uint32_t* argb, int w, int h) {
		for (int y = 0; y < h; y += 2) {
			const uint8_t* row0 = raw + y * w;
			const uint8_t* row1 = row0 + w;
			uint32_t* out0 = argb + y * w;
			uint32_t* out1 = out0 + w;
			for (int x = 0; x < w; x += 2) {
				int b = row0[x];
				int g = (row0[x + 1] + row1[x]) >> 1;
				int r = row1[x + 1];
				uint32_t c = 0xff000000 | (r << 16) | (g << 8) | b;
				out0[x] = c;
				out0[x + 1] = c;
				out1[x] = c;
				out1[x + 1] = c;
			}
		}
	}

	bool grabFrame() {
		if (_fd < 0) {
			/* lazy open with retry: camd may mount later than this app */
			if (_retryTick > 0) {
				_retryTick--;
				return false;
			}
			_retryTick = 15; /* ~1s at 15fps timer */
			_fd = open(CAM_DEV, O_RDONLY);
			if (_fd < 0)
				return false;
		}
		/* driver delivers the frame in <=64KB chunks; accumulate a full one */
		int total = 0;
		while (total < CAM_FRAME_SIZE) {
			int ret = read(_fd, _frame + total, CAM_FRAME_SIZE - total);
			if (ret <= 0) {
				if (total > 0) {
					/* mid-frame read return lost: driver's chunk position
					 * has advanced past ours; resync both ends to 0 or
					 * every later frame is a constant-offset wrap */
					vfs_fcntl_wait(_fd, 2, NULL);
				}
				return false; /* no frame ready this tick */
			}
			total += ret;
		}
		bayer_to_argb(_frame, _camGraph->buffer, CAM_W, CAM_H);
		return true;
	}

protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		//graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff000000);
		if (_camGraph == NULL)
			return;

		/* scale to fit widget area */
		int dw = r.w;
		int dh = r.h;
		int dx = r.x;
		int dy = r.y;

		/* maintain aspect ratio */
		/*if (dw * CAM_H > dh * CAM_W) {
			dw = dh * CAM_W / CAM_H;
			dx = r.x + (r.w - dw) / 2;
		} else {
			dh = dw * CAM_H / CAM_W;
			dy = r.y + (r.h - dh) / 2;
		}
        */

		/* scale (bilinear) the camera frame to the dst rect */
		graph_blt_fit(_camGraph, 0, 0, CAM_W, CAM_H, g, dx, dy, dw, dh);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerStep) {
		if (grabFrame())
			update();
	}

	bool onIM(xevent_t* ev) {
		if (ev->state != XIM_STATE_RELEASE)
			return false;
		if (ev->value.im.value == KEY_ESC) {
			_running = false;
			return true;
		}
		return false;
	}

public:
	~CamWidget() {
		if (_fd >= 0)
			close(_fd);
		if (_frame != NULL)
			delete[] _frame;
		if (_camGraph != NULL) {
			delete[] _camGraph->buffer;
			graph_free(_camGraph);
		}
	}

	CamWidget() {
		_fd = -1;
		_frame = NULL;
		_camGraph = NULL;
		_running = true;
		_retryTick = 0;

		/* open is retried lazily in grabFrame; never block app startup */
		_fd = open(CAM_DEV, O_RDONLY);

		_frame = new uint8_t[CAM_FRAME_SIZE];
		uint32_t* buf = new uint32_t[CAM_W * CAM_H];
		memset(buf, 0, CAM_W * CAM_H * 4);
		_camGraph = graph_new(buf, CAM_W, CAM_H);
	}

	bool ready() { return _camGraph != NULL; }
};

int main(int argc, char** argv) {
	X x;
	WidgetWin win;

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::HORIZONTAL);

	CamWidget* camW = new CamWidget();
	if (!camW->ready()) {
		delete camW;
		return -1;
	}

	root->add(camW);
	root->focus(camW);

	win.open(&x, -1, -1, -1, CAM_W, CAM_H, "cam", XWIN_STYLE_NORMAL);
	win.setTimer(10); /* ~30fps */
	//win.max();
	widgetXRun(&x, &win);
	return 0;
}
