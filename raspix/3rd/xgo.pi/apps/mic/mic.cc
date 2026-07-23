#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <x++/X.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <graph/graph.h>
#include <font/font.h>
#include <ewoksys/keydef.h>

using namespace Ewok;

#define MIC_DEV "/dev/mic0"
#define MIC_WIN_W 240
#define MIC_WIN_H 240
#define MIC_SAMPLE_HISTORY 208
#define MIC_READ_BYTES 2048
#define MIC_HEADER_H 28
#define MIC_MARGIN 10
#define MIC_DC_TRACK_DIV 64
#define MIC_NOISE_FLOOR 320
#define MIC_DRAW_SMOOTH_DIV 4

class MicWidget: public Widget {
	int _fd;
	int _retryTick;
	bool _opened;
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
		if (_fd >= 0)
			return true;
		if (_retryTick > 0) {
			_retryTick--;
			return false;
		}

		_retryTick = 15;
		_fd = open(MIC_DEV, O_RDONLY);
		_opened = (_fd >= 0);
		return _opened;
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
			int ret = read(_fd, raw, sizeof(raw));
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

	void drawHeader(graph_t* g, XTheme* theme, const grect_t& r) {
		char line[96];
		const char* state = _opened ? "ready" : "waiting";
		int peakLPct = (_peakLeft * 100) / 32767;
		int peakRPct = (_peakRight * 100) / 32767;

		snprintf(line, sizeof(line), "L %d%%  R %d%%  %s  %dB",
				peakLPct, peakRPct, state, _lastReadBytes);
		graph_draw_text_font(g, r.x + MIC_MARGIN, r.y + 6,
				line, theme->getFont(), theme->basic.fontSize, theme->basic.fgColor);
	}

	void drawWave(graph_t* g, const grect_t& r, const int16_t* samples, uint32_t lineColor) {
		int count = sampleCount();
		if (count < 2)
			return;

		int prevX = r.x;
		int prevY = r.y + r.h / 2;
		int usable = count < r.w ? count : r.w;

		for (int i = 0; i < usable; i++) {
			int idx = count - usable + i;
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
		graph_draw_text_font(g, r.x + 6, r.y + 4,
				label, theme->getFont(), theme->basic.fontSize, 0xffb7bec8);
	}

protected:
	void onRepaint(graph_t* g, XTheme* theme, const grect_t& r) {
		grect_t waveRect;
		grect_t leftRect;
		grect_t rightRect;
		int gap = 6;
		int halfH;

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff101318);
		drawHeader(g, theme, r);

		waveRect.x = r.x + MIC_MARGIN;
		waveRect.y = r.y + MIC_HEADER_H;
		waveRect.w = r.w - MIC_MARGIN * 2;
		waveRect.h = r.h - MIC_HEADER_H - MIC_MARGIN;

		halfH = (waveRect.h - gap) / 2;
		leftRect = waveRect;
		leftRect.h = halfH;
		rightRect = waveRect;
		rightRect.y = waveRect.y + halfH + gap;
		rightRect.h = waveRect.h - halfH - gap;

		drawChannel(g, theme, leftRect, "left", _leftSamples, 0xff45e0a8);
		drawChannel(g, theme, rightRect, "right", _rightSamples, 0xfff6c560);
	}

	void onTimer(uint32_t timerFPS, uint32_t timerStep) {
		(void)timerFPS;
		(void)timerStep;
		readMic();
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
	MicWidget() {
		_fd = -1;
		_retryTick = 0;
		_opened = false;
		_writePos = 0;
		_filled = false;
		_lastReadBytes = 0;
		_peakLeft = 0;
		_peakRight = 0;
		_dcLeft = 0;
		_dcRight = 0;
		_drawLeft = 0;
		_drawRight = 0;
		memset(_leftSamples, 0, sizeof(_leftSamples));
		memset(_rightSamples, 0, sizeof(_rightSamples));
	}

	~MicWidget() {
		if (_fd >= 0)
			close(_fd);
	}
};

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	X x;
	WidgetWin win;

	RootWidget* root = new RootWidget();
	win.setRoot(root);
	root->setType(Container::HORIZONTAL);

	MicWidget* mic = new MicWidget();
	root->add(mic);
	root->focus(mic);

	win.open(&x, -1, -1, -1, MIC_WIN_W, MIC_WIN_H, "mic", XWIN_STYLE_NORMAL);
	win.setTimer(20);
	win.max();
	widgetXRun(&x, &win);
	return 0;
}
