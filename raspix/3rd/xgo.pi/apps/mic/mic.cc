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
#include <ewoksys/thread.h>
#include <ewoksys/kernel_tic.h>
#include <sherpa-onnx/sherpa.h>

using namespace Ewok;

#define MIC_DEV "/dev/mic0"
#define MIC_WIN_W 240
#define MIC_WIN_H 240
#define MIC_SAMPLE_HISTORY 208
/*
 * Per-read chunk; 4 turns x 4096B = 16KB per timer tick, above the
 * ~9.6KB a 50ms tick accumulates at 48kHz stereo (192KB/s), so the
 * driver ring never fills up and no audio is lost for the recognizer.
 */
#define MIC_READ_BYTES 4096
#define MIC_HEADER_H 28
#define MIC_MARGIN 10
#define MIC_DC_TRACK_DIV 64
#define MIC_NOISE_FLOOR 320
#define MIC_DRAW_SMOOTH_DIV 4
#define MIC_DRAW_GAIN 8
#define MIC_DBG_H 34

#define ASR_MODEL_PATH "/data/model/encn/model.int8.onnx"
#define ASR_TOKENS_PATH "/data/model/encn/tokens.txt"
//#define ASR_MODEL_PATH "/data/model/en/model.int8.onnx"
//#define ASR_TOKENS_PATH "/data/model/en/tokens.txt"

#define ASR_INPUT_RATE 48000
/* energy VAD over DC-removed mono, tuned against MIC_NOISE_FLOOR */
#define ASR_VAD_START 900
#define ASR_VAD_KEEP 500
#define ASR_SILENCE_TICKS 15   /* ~0.75s of trailing silence ends a segment */
#define ASR_MIN_SPEECH_TICKS 5 /* segments shorter than this are dropped */
#define ASR_MAX_SAMPLES (ASR_INPUT_RATE * 12)
#define ASR_PREROLL 9600       /* 0.2s kept before speech onset */

enum {
	ASR_LOADING = 0,
	ASR_FAILED,
	ASR_LISTENING,
	ASR_RECORDING,
	ASR_DECODING
};

class MicWidget: public Widget {
	int _fd;
	int _retryTick;
	bool _opened;
	int16_t _leftSamples[MIC_SAMPLE_HISTORY];
	int16_t _rightSamples[MIC_SAMPLE_HISTORY];
	int _writePos;
	bool _filled;
	int _lastReadBytes;
	int _rateAccum;   /* bytes since last rate stamp */
	int _rateKBs;     /* measured input data rate, KB/s */
	uint64_t _rateStamp;
	int _peakLeft;
	int _peakRight;
	int32_t _dcLeft;
	int32_t _dcRight;
	int16_t _drawLeft;
	int16_t _drawRight;

	SherpaRecognizer* _asr;
	SherpaStream* _asrStream;
	volatile int _asrState;
	char _asrText[256];
	int32_t _asrDc;
	int _silenceTicks;
	int _speechTicks;
	int _fedSamples;
	int16_t _preroll[ASR_PREROLL];
	int _prePos;
	bool _preFull;

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
		int16_t mono[MIC_READ_BYTES]; /* up to 4 turns x 1024 frames */
		int monoCnt = 0;
		int total = 0;
		int peakLeft = 0;
		int peakRight = 0;
		int64_t vadSum = 0;
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
				int16_t rawL = pcm[i * 2];
				int16_t rawR = pcm[i * 2 + 1];
				int left = prepareSample(rawL, &_dcLeft, &_drawLeft);
				int right = prepareSample(rawR, &_dcRight, &_drawRight);
				int leftAmp = abs_i32(left);
				int rightAmp = abs_i32(right);
				if (leftAmp > peakLeft)
					peakLeft = leftAmp;
				if (rightAmp > peakRight)
					peakRight = rightAmp;
				pushSample(clamp16(left), clamp16(right));

				/* raw mono (unfiltered) for the recognizer */
				int m = ((int)rawL + (int)rawR) / 2;
				_asrDc += ((int32_t)m - _asrDc) / MIC_DC_TRACK_DIV;
				int centered = m - _asrDc;
				vadSum += abs_i32(centered);
				if (monoCnt < (int)(sizeof(mono) / sizeof(mono[0])))
					mono[monoCnt++] = clamp16(centered);
			}
		}

		if (gotData) {
			_lastReadBytes = total;
			_peakLeft = peakLeft;
			_peakRight = peakRight;
		}

		/* measured incoming byte rate; 48kHz stereo s16 should read ~187KB/s */
		_rateAccum += total;
		uint64_t now = kernel_tic_ms(0);
		if (_rateStamp == 0)
			_rateStamp = now;
		else if (now - _rateStamp >= 1000) {
			_rateKBs = (int)(((uint64_t)_rateAccum * 1000) / (now - _rateStamp) / 1024);
			_rateAccum = 0;
			_rateStamp = now;
		}

		if (monoCnt > 0)
			asrProcess(mono, monoCnt, (int)(vadSum / monoCnt));
	}

	static void* asrLoadThread(void* p) {
		MicWidget* w = (MicWidget*)p;
		w->_asr = SherpaCreateRecognizer(ASR_MODEL_PATH, ASR_TOKENS_PATH);
		if (w->_asr == NULL) {
			snprintf(w->_asrText, sizeof(w->_asrText),
					"model not found:\n" ASR_MODEL_PATH);
			w->_asrState = ASR_FAILED;
			return NULL;
		}
		w->_asrStream = SherpaCreateStream(w->_asr, ASR_INPUT_RATE);
		if (w->_asrStream == NULL) {
			SherpaDestroyRecognizer(w->_asr);
			w->_asr = NULL;
			strcpy(w->_asrText, "stream create failed");
			w->_asrState = ASR_FAILED;
			return NULL;
		}
		strcpy(w->_asrText, "say something...");
		w->_asrState = ASR_LISTENING;
		return NULL;
	}

	static void* asrDecodeThread(void* p) {
		MicWidget* w = (MicWidget*)p;
		const char* text = SherpaDecode(w->_asr, w->_asrStream);
		if (text != NULL && text[0] != '\0') {
			strncpy(w->_asrText, text, sizeof(w->_asrText) - 1);
			w->_asrText[sizeof(w->_asrText) - 1] = '\0';
		}
		else {
			strcpy(w->_asrText, "(no speech)");
		}
		w->_asrState = ASR_LISTENING;
		return NULL;
	}

	void prerollPush(const int16_t* s, int n) {
		for (int i = 0; i < n; i++) {
			_preroll[_prePos++] = s[i];
			if (_prePos >= ASR_PREROLL) {
				_prePos = 0;
				_preFull = true;
			}
		}
	}

	void prerollFeed(void) {
		if (_preFull) {
			SherpaAcceptWaveform(_asr, _asrStream,
					_preroll + _prePos, ASR_PREROLL - _prePos);
			SherpaAcceptWaveform(_asr, _asrStream, _preroll, _prePos);
			_fedSamples += ASR_PREROLL;
		}
		else if (_prePos > 0) {
			SherpaAcceptWaveform(_asr, _asrStream, _preroll, _prePos);
			_fedSamples += _prePos;
		}
		_prePos = 0;
		_preFull = false;
	}

	void asrProcess(const int16_t* mono, int n, int level) {
		if (_asrState == ASR_LISTENING) {
			prerollPush(mono, n);
			if (level >= ASR_VAD_START) {
				_fedSamples = 0;
				_speechTicks = 1;
				_silenceTicks = 0;
				prerollFeed(); /* mono already inside the ring */
				_asrState = ASR_RECORDING;
			}
		}
		else if (_asrState == ASR_RECORDING) {
			SherpaAcceptWaveform(_asr, _asrStream, mono, n);
			_fedSamples += n;
			if (level >= ASR_VAD_KEEP) {
				_speechTicks++;
				_silenceTicks = 0;
			}
			else {
				_silenceTicks++;
			}

			if (_silenceTicks >= ASR_SILENCE_TICKS || _fedSamples >= ASR_MAX_SAMPLES) {
				if (_speechTicks >= ASR_MIN_SPEECH_TICKS) {
					strcpy(_asrText, "recognizing...");
					_asrState = ASR_DECODING;
					if (thread_create(asrDecodeThread, this) < 0)
						asrDecodeThread(this); /* fall back to blocking */
				}
				else {
					SherpaReset(_asr, _asrStream);
					_asrState = ASR_LISTENING;
				}
			}
		}
		/* ASR_DECODING: stream is owned by the worker; keep pre-roll warm */
		else if (_asrState == ASR_DECODING) {
			prerollPush(mono, n);
		}
	}

	const char* asrStatus(void) const {
		switch (_asrState) {
		case ASR_FAILED: return "[asr disabled]";
		case ASR_RECORDING: return "[recording]";
		case ASR_DECODING: return "[recognizing...]";
		default: return "[listening]";
		}
	}

	void drawDbg(graph_t* g, XTheme* theme, const grect_t& r) {
		char status[40];
		char line[300];
		char* second;

		if (_asrState == ASR_LOADING)
			snprintf(status, sizeof(status), "[loading model %d%%]",
					(int)SherpaLoadProgress());
		else
			strcpy(status, asrStatus());

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff1c1408);
		snprintf(line, sizeof(line), "%s\n%s", status, _asrText);

		second = strchr(line, '\n');
		if (second != NULL) {
			*second = '\0';
			second++;
			char* third = strchr(second, '\n');
			if (third != NULL)
				*third = '\0';
		}
		graph_draw_text_font(g, r.x + 4, r.y + 2,
				line, theme->getFont(), 14, 0xffffd080);
		if (second != NULL) {
			graph_draw_text_font(g, r.x + 4, r.y + 2 + MIC_DBG_H / 2,
					second, theme->getFont(), 14, 0xffffd080);
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

		snprintf(line, sizeof(line), "L %d%%  R %d%%  %s  %dKB/s",
				peakLPct, peakRPct, state, _rateKBs);
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
		grect_t dbgRect;
		int gap = 6;
		int halfH;

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff101318);
		drawHeader(g, theme, r);

		waveRect.x = r.x + MIC_MARGIN;
		waveRect.y = r.y + MIC_HEADER_H;
		waveRect.w = r.w - MIC_MARGIN * 2;
		waveRect.h = r.h - MIC_HEADER_H - MIC_MARGIN - MIC_DBG_H;

		halfH = (waveRect.h - gap) / 2;
		leftRect = waveRect;
		leftRect.h = halfH;
		rightRect = waveRect;
		rightRect.y = waveRect.y + halfH + gap;
		rightRect.h = waveRect.h - halfH - gap;

		drawChannel(g, theme, leftRect, "left", _leftSamples, 0xff45e0a8);
		drawChannel(g, theme, rightRect, "right", _rightSamples, 0xfff6c560);

		dbgRect.x = r.x + MIC_MARGIN;
		dbgRect.y = waveRect.y + waveRect.h + 2;
		dbgRect.w = r.w - MIC_MARGIN * 2;
		dbgRect.h = MIC_DBG_H - 4;
		drawDbg(g, theme, dbgRect);
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
		_rateAccum = 0;
		_rateKBs = 0;
		_rateStamp = 0;
		_peakLeft = 0;
		_peakRight = 0;
		_dcLeft = 0;
		_dcRight = 0;
		_drawLeft = 0;
		_drawRight = 0;
		memset(_leftSamples, 0, sizeof(_leftSamples));
		memset(_rightSamples, 0, sizeof(_rightSamples));

		_asr = NULL;
		_asrStream = NULL;
		_asrState = ASR_LOADING;
		strcpy(_asrText, "loading " ASR_MODEL_PATH);
		_asrDc = 0;
		_silenceTicks = 0;
		_speechTicks = 0;
		_fedSamples = 0;
		_prePos = 0;
		_preFull = false;
		if (thread_create(asrLoadThread, this) < 0)
			asrLoadThread(this); /* fall back to blocking load */
	}

	~MicWidget() {
		if (_fd >= 0)
			close(_fd);
		if (_asr != NULL) {
			if (_asrStream != NULL)
				SherpaDestroyStream(_asr, _asrStream);
			SherpaDestroyRecognizer(_asr);
		}
	}
};

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	X x;
	WidgetWin win;
	XTheme* theme = win.getTheme();
	theme->setFont("system-cn", 14);

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
