#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <x++/X.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <graph/graph.h>
#include <ewoksys/keydef.h>
#include <ewoksys/vfs.h>

using namespace Ewok;

#define CAM_DEV "/dev/cam0"
#define CAM_W 640
#define CAM_H 480
#define CAM_BPP 1 /* RAW8 Bayer (SBGGR8) */
#define CAM_FRAME_SIZE (CAM_W * CAM_H * CAM_BPP)
/* The OV5647 raw stream can expose a noisy Bayer edge column on the far
 * right. Crop one 2-pixel Bayer cell from the preview so it doesn't show
 * up as a persistent colored line after demosaic. */
#define CAM_CROP_LEFT 0
#define CAM_CROP_TOP 0
#define CAM_CROP_RIGHT 2
#define CAM_CROP_BOTTOM 0
#define CAM_VIEW_W (CAM_W - CAM_CROP_LEFT - CAM_CROP_RIGHT)
#define CAM_VIEW_H (CAM_H - CAM_CROP_TOP - CAM_CROP_BOTTOM)

class CamWidget: public Widget {
	int _fd;
	uint8_t* _frame;
	graph_t* _camGraph;
	bool _running;
	int _retryTick;
	int _wb_r_q8;
	int _wb_b_q8;
	static bool _gammaReady;
	static uint8_t _gammaLut[256];

	static inline int clamp_u8(int v) {
		if (v < 0)
			return 0;
		if (v > 255)
			return 255;
		return v;
	}

	static inline int clamp_coord(int v, int hi) {
		if (v < 0)
			return 0;
		if (v > hi)
			return hi;
		return v;
	}

	static void init_gamma_lut(void) {
		if (_gammaReady)
			return;
		for (int i = 0; i < 256; i++) {
			float f = (float)i / 255.0f;
			_gammaLut[i] = (uint8_t)(powf(f, 1.0f / 2.2f) * 255.0f + 0.5f);
		}
		_gammaReady = true;
	}

	inline int raw_at(const uint8_t* raw, int w, int h, int x, int y) const {
		x = clamp_coord(x, w - 1);
		y = clamp_coord(y, h - 1);
		return raw[y * w + x];
	}

	void update_white_balance(const uint8_t* raw, int w, int h) {
		uint64_t sum_r = 0;
		uint64_t sum_g = 0;
		uint64_t sum_b = 0;
		uint32_t count = 0;

		for (int y = 0; y + 1 < h; y += 8) {
			const uint8_t* row0 = raw + y * w;
			const uint8_t* row1 = row0 + w;
			for (int x = 0; x + 1 < w; x += 8) {
				sum_b += row0[x];
				sum_g += row0[x + 1];
				sum_g += row1[x];
				sum_r += row1[x + 1];
				count++;
			}
		}

		if (count == 0)
			return;

		int avg_r = (int)(sum_r / count);
		int avg_g = (int)(sum_g / (count * 2));
		int avg_b = (int)(sum_b / count);
		if (avg_r < 1)
			avg_r = 1;
		if (avg_b < 1)
			avg_b = 1;

		int target = avg_g;
		int next_r = (target << 8) / avg_r;
		int next_b = (target << 8) / avg_b;
		if (next_r < 128)
			next_r = 128;
		if (next_r > 768)
			next_r = 768;
		if (next_b < 128)
			next_b = 128;
		if (next_b > 768)
			next_b = 768;

		/* Smooth frame-to-frame changes to avoid WB pumping/flicker. */
		_wb_r_q8 = (_wb_r_q8 * 7 + next_r) >> 3;
		_wb_b_q8 = (_wb_b_q8 * 7 + next_b) >> 3;
	}

	inline uint32_t pack_rgb(int r, int g, int b) const {
		r = _gammaLut[clamp_u8((r * _wb_r_q8) >> 8)];
		g = _gammaLut[clamp_u8(g)];
		b = _gammaLut[clamp_u8((b * _wb_b_q8) >> 8)];
		return 0xff000000 | (r << 16) | (g << 8) | b;
	}

	/* RAW8 BGGR -> ARGB, bilinear demosaic + gray-world WB + display gamma. */
	void bayer_to_argb(const uint8_t* raw, uint32_t* argb, int w, int h) {
		update_white_balance(raw, w, h);

		for (int y = 0; y < h; y++) {
			uint32_t* out = argb + y * w;
			for (int x = 0; x < w; x++) {
				int r, g, b;
				int c = raw_at(raw, w, h, x, y);
				bool ye = ((y & 1) == 0);
				bool xe = ((x & 1) == 0);

				if (ye && xe) {
					/* B site */
					b = c;
					g = (raw_at(raw, w, h, x - 1, y) +
						raw_at(raw, w, h, x + 1, y) +
						raw_at(raw, w, h, x, y - 1) +
						raw_at(raw, w, h, x, y + 1)) >> 2;
					r = (raw_at(raw, w, h, x - 1, y - 1) +
						raw_at(raw, w, h, x + 1, y - 1) +
						raw_at(raw, w, h, x - 1, y + 1) +
						raw_at(raw, w, h, x + 1, y + 1)) >> 2;
				} else if (ye) {
					/* G site on blue row */
					g = c;
					b = (raw_at(raw, w, h, x - 1, y) +
						raw_at(raw, w, h, x + 1, y)) >> 1;
					r = (raw_at(raw, w, h, x, y - 1) +
						raw_at(raw, w, h, x, y + 1)) >> 1;
				} else if (xe) {
					/* G site on red row */
					g = c;
					r = (raw_at(raw, w, h, x - 1, y) +
						raw_at(raw, w, h, x + 1, y)) >> 1;
					b = (raw_at(raw, w, h, x, y - 1) +
						raw_at(raw, w, h, x, y + 1)) >> 1;
				} else {
					/* R site */
					r = c;
					g = (raw_at(raw, w, h, x - 1, y) +
						raw_at(raw, w, h, x + 1, y) +
						raw_at(raw, w, h, x, y - 1) +
						raw_at(raw, w, h, x, y + 1)) >> 2;
					b = (raw_at(raw, w, h, x - 1, y - 1) +
						raw_at(raw, w, h, x + 1, y - 1) +
						raw_at(raw, w, h, x - 1, y + 1) +
						raw_at(raw, w, h, x + 1, y + 1)) >> 2;
				}

				out[x] = pack_rgb(r, g, b);
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

		/* Hide the unstable raw border columns/rows from preview. */
		graph_blt_fit(_camGraph, CAM_CROP_LEFT, CAM_CROP_TOP,
				CAM_VIEW_W, CAM_VIEW_H, g, dx, dy, dw, dh);
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
		_wb_r_q8 = 256;
		_wb_b_q8 = 256;
		init_gamma_lut();

		/* open is retried lazily in grabFrame; never block app startup */
		_fd = open(CAM_DEV, O_RDONLY);

		_frame = new uint8_t[CAM_FRAME_SIZE];
		uint32_t* buf = new uint32_t[CAM_W * CAM_H];
		memset(buf, 0, CAM_W * CAM_H * 4);
		_camGraph = graph_new(buf, CAM_W, CAM_H);
	}

	bool ready() { return _camGraph != NULL; }
};

bool CamWidget::_gammaReady = false;
uint8_t CamWidget::_gammaLut[256];

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
	win.setTimer(24); /* ~30fps */
	//win.max();
	widgetXRun(&x, &win);
	return 0;
}
