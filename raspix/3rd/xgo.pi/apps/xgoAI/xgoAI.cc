#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <x++/X.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <graph/graph.h>
#include <font/font.h>
#include <ewoksys/keydef.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

using namespace Ewok;

/* ---------------- camera (merged from cam app) ---------------- */
#define CAM_DEV "/dev/cam0"
#define CAM_W 640
#define CAM_H 480
#define CAM_BPP 1 /* RAW8 Bayer (SBGGR8) */
#define CAM_FRAME_SIZE (CAM_W * CAM_H * CAM_BPP)
/* Hide the unstable raw border columns/rows from preview. */
#define CAM_CROP_LEFT 0
#define CAM_CROP_TOP 0
#define CAM_CROP_RIGHT 2
#define CAM_CROP_BOTTOM 0
#define CAM_VIEW_W (CAM_W - CAM_CROP_LEFT - CAM_CROP_RIGHT)
#define CAM_VIEW_H (CAM_H - CAM_CROP_TOP - CAM_CROP_BOTTOM)

/* ---------------- mic (merged from mic app) ---------------- */
#define MIC_DEV "/dev/mic0"
#define MIC_SAMPLE_HISTORY 208
#define MIC_READ_BYTES 2048
#define MIC_DC_TRACK_DIV 64
#define MIC_NOISE_FLOOR 320
#define MIC_DRAW_SMOOTH_DIV 4
#define MIC_DRAW_GAIN 8
#define MIC_DBG_TICKS 50

/* ---------------- layout ----------------
 * +------------------+-------+
 * |                  |   R   |
 * |        A         +-------+
 * |    (camera)      |   L   |
 * +------------------+-------+
 * |            D (debug)     |
 * +--------------------------+
 */
#define XAI_MARGIN 4
#define XAI_GAP 4
#define XAI_DBG_H 46
#define XAI_SIDE_DIV 3 /* right panel width = main width / XAI_SIDE_DIV */

class XgoAIWidget: public Widget {
	/* -------- camera state -------- */
	int _camFd;
	uint8_t* _frame;
	graph_t* _camGraph;
	int _camRetryTick;
	int _wb_r_q8;
	int _wb_b_q8;
	uint32_t _camFrames;
	int _camFps;
	int _fpsTick;
	int _fpsBase;
	static bool _gammaReady;
	static uint8_t _gammaLut[256];

	/* -------- mic state -------- */
	int _micFd;
	int _micRetryTick;
	bool _micOpened;
	int16_t _leftSamples[MIC_SAMPLE_HISTORY];
	int16_t _rightSamples[MIC_SAMPLE_HISTORY];
	int _writePos;
	bool _filled;
	int _lastReadBytes;
	int _peakLeft;
	int _peakRight;
	int32_t _dcLeft;
	int32_t _dcRight;
	int16_t _drawLeft;
	int16_t _drawRight;
	char _dbgLine[128];
	int _dbgTick;

	/* ================= camera ================= */
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
		if (_camFd < 0) {
			/* lazy open with retry: camd may mount later than this app */
			if (_camRetryTick > 0) {
				_camRetryTick--;
				return false;
			}
			_camRetryTick = 15; /* ~1s at 24fps timer */
			_camFd = open(CAM_DEV, O_RDONLY);
			if (_camFd < 0)
				return false;
		}
		/* driver delivers the frame in <=64KB chunks; accumulate a full one */
		int total = 0;
		while (total < CAM_FRAME_SIZE) {
			int ret = read(_camFd, _frame + total, CAM_FRAME_SIZE - total);
			if (ret <= 0) {
				if (total > 0) {
					/* mid-frame read return lost: resync both ends to 0 */
					vfs_fcntl_wait(_camFd, 2, NULL);
				}
				return false; /* no frame ready this tick */
			}
			total += ret;
		}
		bayer_to_argb(_frame, _camGraph->buffer, CAM_W, CAM_H);
		_camFrames++;
		return true;
	}

	/* ================= mic ================= */
	static int16_t clamp16(int v) {
		if (v < -32768)
			return -32768;
		if (v > 32767)
			return 32767;
		return (int16_t)v;
	}

	static int abs_i32(int v) {
		return v < 0 ? -v : v;
	}

	static int applyNoiseGate(int sample) {
		int amp = abs_i32(sample);
		if (amp <= MIC_NOISE_FLOOR)
			return 0;
		amp -= MIC_NOISE_FLOOR;
		return sample < 0 ? -amp : amp;
	}

	int16_t prepareSample(int16_t raw, int32_t* dcState, int16_t* drawState) {
		int centered;
		int filtered;

		*dcState += ((int32_t)raw - *dcState) / MIC_DC_TRACK_DIV;
		centered = (int32_t)raw - *dcState;
		centered = applyNoiseGate(centered);
		centered *= MIC_DRAW_GAIN;

		filtered = (*drawState * (MIC_DRAW_SMOOTH_DIV - 1) + centered) / MIC_DRAW_SMOOTH_DIV;
		if (abs_i32(filtered) < 4)
			filtered = 0;

		*drawState = clamp16(filtered);
		return *drawState;
	}

	void pushSample(int16_t left, int16_t right) {
		_leftSamples[_writePos] = left;
		_rightSamples[_writePos] = right;
		_writePos++;
		if (_writePos >= MIC_SAMPLE_HISTORY) {
			_writePos = 0;
			_filled = true;
		}
	}

	int sampleCount(void) const {
		return _filled ? MIC_SAMPLE_HISTORY : _writePos;
	}

	int16_t getHistorySample(const int16_t* samples, int index) const {
		int count = sampleCount();
		if (count <= 0)
			return 0;
		if (!_filled)
			return samples[index];

		int start = _writePos;
		return samples[(start + index) % MIC_SAMPLE_HISTORY];
	}

	bool openMic(void) {
		if (_micFd >= 0)
			return true;
		if (_micRetryTick > 0) {
			_micRetryTick--;
			return false;
		}

		_micRetryTick = 15;
		_micFd = open(MIC_DEV, O_RDONLY);
		_micOpened = (_micFd >= 0);
		return _micOpened;
	}

	void readMic(void) {
		uint8_t raw[MIC_READ_BYTES];
		int total = 0;
		int peakLeft = 0;
		int peakRight = 0;
		bool gotData = false;

		if (!openMic())
			return;

		for (int turns = 0; turns < 4; turns++) {
			int ret = read(_micFd, raw, sizeof(raw));
			if (ret <= 0) {
				break;
			}

			gotData = true;
			total += ret;
			int frames = ret / 4;
			const int16_t* pcm = (const int16_t*)raw;
			for (int i = 0; i < frames; i++) {
				int left = prepareSample(pcm[i * 2], &_dcLeft, &_drawLeft);
				int right = prepareSample(pcm[i * 2 + 1], &_dcRight, &_drawRight);
				int leftAmp = abs_i32(left);
				int rightAmp = abs_i32(right);
				if (leftAmp > peakLeft)
					peakLeft = leftAmp;
				if (rightAmp > peakRight)
					peakRight = rightAmp;
				pushSample(clamp16(left), clamp16(right));
			}
		}

		if (gotData) {
			_lastReadBytes = total;
			_peakLeft = peakLeft;
			_peakRight = peakRight;
		}
	}

	void fetchDbg(void) {
		proto_t out;

		if (_dbgTick > 0) {
			_dbgTick--;
			return;
		}
		_dbgTick = MIC_DBG_TICKS;

		PF->init(&out);
		if (dev_cntl(MIC_DEV, 0, NULL, &out) == 0) {
			const char* s = proto_read_str(&out);
			if (s != NULL) {
				strncpy(_dbgLine, s, sizeof(_dbgLine) - 1);
				_dbgLine[sizeof(_dbgLine) - 1] = '\0';
			}
		}
		else {
			strncpy(_dbgLine, "dev_cntl failed", sizeof(_dbgLine) - 1);
		}
		PF->clear(&out);
	}

	/* ================= drawing ================= */
	void drawCam(graph_t* g, XTheme* theme, const grect_t& r) {
		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff000000);
		if (_camGraph == NULL || _camFrames == 0) {
			graph_rect(g, r.x, r.y, r.w, r.h, 0xff454b55);
			graph_draw_text_font(g, r.x + 6, r.y + 6,
					_camFd >= 0 ? "cam: no frame" : "cam: waiting...",
					theme->getFont(), theme->basic.fontSize, 0xffb7bec8);
			return;
		}

		/* scale to fit area A, keep source aspect ratio (black bars) */
		int dw = r.w;
		int dh = r.h;
		if (dw * CAM_VIEW_H > dh * CAM_VIEW_W) {
			dw = dh * CAM_VIEW_W / CAM_VIEW_H;
		} else {
			dh = dw * CAM_VIEW_H / CAM_VIEW_W;
		}
		int dx = r.x + (r.w - dw) / 2;
		int dy = r.y + (r.h - dh) / 2;

		graph_blt_fit(_camGraph, CAM_CROP_LEFT, CAM_CROP_TOP,
				CAM_VIEW_W, CAM_VIEW_H, g, dx, dy, dw, dh);
	}

	void drawGrid(graph_t* g, const grect_t& r) {
		uint32_t gridColor = 0xff2a2f36;
		int midY = r.y + r.h / 2;

		graph_rect(g, r.x, r.y, r.w, r.h, 0xff454b55);
		graph_line(g, r.x, midY, r.x + r.w - 1, midY, 0xff3d8bfd);

		for (int i = 1; i < 4; i++) {
			int y = r.y + (r.h * i) / 4;
			graph_line(g, r.x, y, r.x + r.w - 1, y, gridColor);
		}

		for (int i = 1; i < 4; i++) {
			int x = r.x + (r.w * i) / 4;
			graph_line(g, x, r.y, x, r.y + r.h - 1, gridColor);
		}
	}

	void drawWave(graph_t* g, const grect_t& r, const int16_t* samples, uint32_t lineColor) {
		int count = sampleCount();
		if (count < 2)
			return;

		int prevX = r.x;
		int prevY = r.y + r.h / 2;
		int usable = count < r.w ? count : r.w;
		if (usable < 2)
			usable = 2;

		for (int i = 0; i < usable; i++) {
			int idx = count - usable + i;
			if (idx < 0)
				idx = 0;
			int16_t sample = getHistorySample(samples, idx);
			int x = r.x + (i * (r.w - 1)) / (usable - 1);
			int y = r.y + r.h / 2 - ((int)sample * (r.h / 2 - 4)) / 32768;

			if (i > 0)
				graph_line(g, prevX, prevY, x, y, lineColor);
			prevX = x;
			prevY = y;
		}
	}

	void drawChannel(graph_t* g, XTheme* theme, const grect_t& r,
			const char* label, const int16_t* samples, uint32_t lineColor) {
		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff171b22);
		drawGrid(g, r);
		drawWave(g, r, samples, lineColor);
		graph_draw_text_font(g, r.x + 4, r.y + 2,
				label, theme->getFont(), theme->basic.fontSize, 0xffb7bec8);
	}

	void drawDbg(graph_t* g, XTheme* theme, const grect_t& r) {
		char line[128];
		char status[96];
		char* second;
		int peakLPct = (_peakLeft * 100) / 32767;
		int peakRPct = (_peakRight * 100) / 32767;
		int lineH = (r.h - 4) / 3;

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff1c1408);
		graph_rect(g, r.x, r.y, r.w, r.h, 0xff454b55);

		snprintf(status, sizeof(status), "cam %s %dfps #%u  mic %s L%d%% R%d%% %dB",
				_camFd >= 0 ? "ok" : "wait", _camFps, _camFrames,
				_micOpened ? "ok" : "wait", peakLPct, peakRPct, _lastReadBytes);
		graph_draw_text_font(g, r.x + 4, r.y + 2,
				status, theme->getFont(), 10, 0xffffd080);

		strncpy(line, _dbgLine, sizeof(line) - 1);
		line[sizeof(line) - 1] = '\0';
		second = strchr(line, '\n');
		if (second != NULL) {
			*second = '\0';
			second++;
		}
		graph_draw_text_font(g, r.x + 4, r.y + 2 + lineH,
				line, theme->getFont(), 10, 0xffffd080);
		if (second != NULL) {
			graph_draw_text_font(g, r.x + 4, r.y + 2 + lineH * 2,
					second, theme->getFont(), 10, 0xffffd080);
		}
	}

protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		grect_t mainRect;
		grect_t camRect;
		grect_t rightRect;
		grect_t leftRect;
		grect_t dbgRect;
		int sideW;

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff101318);

		/* main area on top, debug strip at bottom spanning full width */
		mainRect.x = r.x + XAI_MARGIN;
		mainRect.y = r.y + XAI_MARGIN;
		mainRect.w = r.w - XAI_MARGIN * 2;
		mainRect.h = r.h - XAI_MARGIN * 2 - XAI_DBG_H - XAI_GAP;
		if (mainRect.h < 16)
			mainRect.h = 16;

		/* right panel (R over L) takes 1/3 of the main width */
		sideW = mainRect.w / XAI_SIDE_DIV;
		if (sideW < 56)
			sideW = 56;
		if (sideW > mainRect.w - 64)
			sideW = mainRect.w - 64;

		camRect.x = mainRect.x;
		camRect.y = mainRect.y;
		camRect.w = mainRect.w - sideW - XAI_GAP;
		camRect.h = mainRect.h;

		rightRect.x = mainRect.x + mainRect.w - sideW;
		rightRect.y = mainRect.y;
		rightRect.w = sideW;
		rightRect.h = (mainRect.h - XAI_GAP) / 2;

		leftRect.x = rightRect.x;
		leftRect.y = rightRect.y + rightRect.h + XAI_GAP;
		leftRect.w = sideW;
		leftRect.h = mainRect.y + mainRect.h - leftRect.y;

		dbgRect.x = r.x + XAI_MARGIN;
		dbgRect.y = r.y + r.h - XAI_MARGIN - XAI_DBG_H;
		dbgRect.w = r.w - XAI_MARGIN * 2;
		dbgRect.h = XAI_DBG_H;

		drawCam(g, theme, camRect);
		drawChannel(g, theme, rightRect, "R", _rightSamples, 0xfff6c560);
		drawChannel(g, theme, leftRect, "L", _leftSamples, 0xff45e0a8);
		drawDbg(g, theme, dbgRect);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerStep) {
		(void)timerStep;

		grabFrame();
		readMic();
		fetchDbg();

		/* fps statistics once per second */
		_fpsTick++;
		if (_fpsTick >= (int)timerFPS) {
			_camFps = (int)_camFrames - _fpsBase;
			_fpsBase = (int)_camFrames;
			_fpsTick = 0;
		}

		update();
	}

	bool onIM(xevent_t* ev) {
		if (ev->state != XIM_STATE_RELEASE)
			return false;

		if (ev->value.im.value == KEY_ESC) {
			getWin()->close();
			return true;
		}
		return false;
	}

public:
	XgoAIWidget() {
		_camFd = -1;
		_frame = NULL;
		_camGraph = NULL;
		_camRetryTick = 0;
		_wb_r_q8 = 256;
		_wb_b_q8 = 256;
		_camFrames = 0;
		_camFps = 0;
		_fpsTick = 0;
		_fpsBase = 0;
		init_gamma_lut();

		_micFd = -1;
		_micRetryTick = 0;
		_micOpened = false;
		_writePos = 0;
		_filled = false;
		_lastReadBytes = 0;
		_peakLeft = 0;
		_peakRight = 0;
		_dcLeft = 0;
		_dcRight = 0;
		_drawLeft = 0;
		_drawRight = 0;
		_dbgTick = 0;
		strcpy(_dbgLine, "fetching...");
		memset(_leftSamples, 0, sizeof(_leftSamples));
		memset(_rightSamples, 0, sizeof(_rightSamples));

		/* opens are retried lazily in onTimer; never block app startup */
		_camFd = open(CAM_DEV, O_RDONLY);

		_frame = new uint8_t[CAM_FRAME_SIZE];
		uint32_t* buf = new uint32_t[CAM_W * CAM_H];
		memset(buf, 0, CAM_W * CAM_H * 4);
		_camGraph = graph_new(buf, CAM_W, CAM_H);
	}

	~XgoAIWidget() {
		if (_camFd >= 0)
			close(_camFd);
		if (_micFd >= 0)
			close(_micFd);
		if (_frame != NULL)
			delete[] _frame;
		if (_camGraph != NULL) {
			delete[] _camGraph->buffer;
			graph_free(_camGraph);
		}
	}

	bool ready() { return _camGraph != NULL; }
};

bool XgoAIWidget::_gammaReady = false;
uint8_t XgoAIWidget::_gammaLut[256];

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	X x;
	WidgetWin win;

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::HORIZONTAL);

	XgoAIWidget* ai = new XgoAIWidget();
	if (!ai->ready()) {
		delete ai;
		return -1;
	}

	root->add(ai);
	root->focus(ai);

	win.open(&x, -1, -1, -1, 320, 240, "xgoAI", XWIN_STYLE_NORMAL);
	win.setTimer(24); /* ~24fps */
	win.max();
	widgetXRun(&x, &win);
	return 0;
}
