#include <Widget/WidgetWin.h>
#include <Widget/WidgetX.h>
#include <x++/X.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
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
/* debug panel: status line + wrapped text lines + progress bar */
#define MIC_DBG_LINE_H 17
#define MIC_DBG_LINES 4
#define MIC_DBG_H (4 + (1 + MIC_DBG_LINES) * MIC_DBG_LINE_H + 6)

#define ASR_ONLINE_TRANSDUCER_ENCODER_PATH "/data/model/encn-online/encoder.int8.onnx"
#define ASR_ONLINE_TOKENS_PATH "/data/model/encn-online/tokens.txt"
#define ASR_OFFLINE_MODEL_PATH "/data/model/encn/model.int8.onnx"
#define ASR_OFFLINE_TOKENS_PATH "/data/model/encn/tokens.txt"
//#define ASR_OFFLINE_MODEL_PATH "/data/model/en/model.int8.onnx"
//#define ASR_OFFLINE_TOKENS_PATH "/data/model/en/tokens.txt"

#define ASR_INPUT_RATE 48000
/* energy VAD over DC-removed mono, tuned against MIC_NOISE_FLOOR */
#define ASR_VAD_START 900
#define ASR_VAD_KEEP 500
#define ASR_SILENCE_TICKS 15   /* ~0.75s of trailing silence ends a segment */
#define ASR_MIN_SPEECH_TICKS 5 /* segments shorter than this are dropped */
#define ASR_PARTIAL_TICKS 4    /* ~0.2s between partial refreshes */
#define ASR_MAX_SAMPLES (ASR_INPUT_RATE * 12)
#define ASR_PREROLL 9600       /* 0.2s kept before speech onset */
#define ASR_QUEUE_SAMPLES (ASR_INPUT_RATE * 8) /* keep UI smooth under decode load */
#define ASR_QUEUE_BATCH 4096

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
        const char* _asrModelPath;
        const char* _asrTokensPath;
        bool _asrFellBack;
        pthread_mutex_t _asrLock;
        pthread_t _asrDecodeTid;
        volatile bool _asrDecodeThreadStarted;
        volatile bool _asrDecodeStop;
        volatile bool _asrResetPending;
        volatile bool _asrPartialPending;
        volatile bool _asrFinalPending;
	volatile int _asrState;
	volatile int _asrProgress; /* decode progress %, drawn while recognizing */
	volatile int _vadLevel;    /* last VAD energy, drawn while listening */
	char _asrText[256];
	int32_t _asrDc;
	int _silenceTicks;
	int _speechTicks;
        int _partialTicks;
	int _fedSamples;
	int16_t _preroll[ASR_PREROLL];
	int _prePos;
	bool _preFull;
        int16_t _asrQueue[ASR_QUEUE_SAMPLES];
        int _asrQRead;
        int _asrQWrite;
        int _asrQCount;

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

        static bool modelFileUsable(const char* path) {
                /* reject tiny files: a checked-out git-lfs pointer is only a
                   few hundred bytes while real models are megabytes */
                FILE* fp = fopen(path, "rb");
                if (fp == NULL)
                        return false;
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fclose(fp);
                return sz >= (1 << 20);
        }

        static bool tokensFileUsable(const char* path) {
                /* real vocabs are tens of KB; a checked-out git-lfs pointer
                   is ~130 bytes and would silently decode to empty text */
                FILE* fp = fopen(path, "rb");
                if (fp == NULL)
                        return false;
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fclose(fp);
                return sz >= 4096;
        }

        static bool onlineModelUsable(void) {
                return modelFileUsable(ASR_ONLINE_TRANSDUCER_ENCODER_PATH) &&
                                tokensFileUsable(ASR_ONLINE_TOKENS_PATH);
        }

        static const char* chooseModelPath(void) {
                if (onlineModelUsable())
                        return ASR_ONLINE_TRANSDUCER_ENCODER_PATH;
                return ASR_OFFLINE_MODEL_PATH;
        }

        static const char* chooseTokensPath(void) {
                if (onlineModelUsable())
                        return ASR_ONLINE_TOKENS_PATH;
                return ASR_OFFLINE_TOKENS_PATH;
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
                w->_asr = SherpaCreateRecognizer(w->_asrModelPath, w->_asrTokensPath);
                if (w->_asr == NULL &&
                                strcmp(w->_asrModelPath,
                                        ASR_ONLINE_TRANSDUCER_ENCODER_PATH) == 0) {
                        /* incomplete online triple (lfs pointer, missing
                           decoder/joiner, bad metadata): fall back to the
                           offline model instead of giving up */
                        w->_asrModelPath = ASR_OFFLINE_MODEL_PATH;
                        w->_asrTokensPath = ASR_OFFLINE_TOKENS_PATH;
                        w->_asrFellBack = true;
                        w->_asr = SherpaCreateRecognizer(w->_asrModelPath,
                                        w->_asrTokensPath);
                }
		if (w->_asr == NULL) {
                        /* name the actual culprit: a model file may exist while
                           its tokens file is a git-lfs pointer or truncated
                           copy, which used to fail silently as "(no speech)" */
                        if (modelFileUsable(ASR_ONLINE_TRANSDUCER_ENCODER_PATH) &&
                                        !tokensFileUsable(ASR_ONLINE_TOKENS_PATH))
                                snprintf(w->_asrText, sizeof(w->_asrText),
                                                "bad tokens file:\n%s",
                                                ASR_ONLINE_TOKENS_PATH);
                        else if (modelFileUsable(ASR_OFFLINE_MODEL_PATH) &&
                                        !tokensFileUsable(ASR_OFFLINE_TOKENS_PATH))
                                snprintf(w->_asrText, sizeof(w->_asrText),
                                                "bad tokens file:\n%s",
                                                ASR_OFFLINE_TOKENS_PATH);
                        else
                                snprintf(w->_asrText, sizeof(w->_asrText),
                                                "model not found:\n%s",
                                                w->_asrModelPath);
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
                if (pthread_create(&w->_asrDecodeTid, NULL, asrDecodeThread, w) != 0) {
                        strcpy(w->_asrText, "decode thread failed");
                        w->_asrState = ASR_FAILED;
                        return NULL;
                }
                w->_asrDecodeThreadStarted = true;
		if (w->_asrFellBack)
                        strcpy(w->_asrText, "say something... (offline)");
                else
                        strcpy(w->_asrText, "say something...");
		w->_asrState = ASR_LISTENING;
		return NULL;
	}

        static void* asrDecodeThread(void* p) {
                MicWidget* w = (MicWidget*)p;
                int16_t batch[ASR_QUEUE_BATCH];
                const bool online = SherpaIsOnline(w->_asr) != 0;

                while (!w->_asrDecodeStop) {
                        bool didWork = false;
                        bool doReset = false;
                        bool doFinal = false;
                        bool doPartial = false;

                        /*
                         * Phase A: feed everything queued so far into the
                         * recognizer. Resampling + fbank is cheap compared to
                         * the neural decode, so the queue stays near empty and
                         * no audio is ever dropped, no matter how far decoding
                         * falls behind; any backlog lives in the fbank frames.
                         */
                        while (true) {
                                int n = 0;
                                pthread_mutex_lock(&w->_asrLock);
                                if (w->_asrResetPending) {
                                        w->_asrResetPending = false;
                                        w->_asrPartialPending = false;
                                        w->_asrFinalPending = false;
                                        w->asrQueueClearLocked();
                                        doReset = true;
                                }
                                else if (w->_asrQCount > 0) {
                                        n = w->_asrQCount;
                                        if (n > ASR_QUEUE_BATCH)
                                                n = ASR_QUEUE_BATCH;
                                        for (int i = 0; i < n; ++i) {
                                                batch[i] = w->_asrQueue[w->_asrQRead];
                                                w->_asrQRead++;
                                                if (w->_asrQRead >= ASR_QUEUE_SAMPLES)
                                                        w->_asrQRead = 0;
                                        }
                                        w->_asrQCount -= n;
                                }
                                pthread_mutex_unlock(&w->_asrLock);

                                if (doReset) {
                                        SherpaReset(w->_asr, w->_asrStream);
                                        didWork = true;
                                        doReset = false;
                                        continue;
                                }
                                if (n <= 0)
                                        break;
                                SherpaAcceptWaveform(w->_asr, w->_asrStream, batch, n);
                                didWork = true;
                        }

                        /*
                         * Phase B: a single decode action per iteration, so
                         * audio feeding (phase A) is never blocked for long.
                         */
                        pthread_mutex_lock(&w->_asrLock);
                        if (w->_asrFinalPending &&
                                        w->_asrState == ASR_DECODING) {
                                w->_asrFinalPending = false;
                                doFinal = true;
                        }
                        else if (!online && w->_asrPartialPending &&
                                        w->_asrState == ASR_RECORDING) {
                                w->_asrPartialPending = false;
                                doPartial = true;
                        }
                        pthread_mutex_unlock(&w->_asrLock);

                        if (doFinal) {
                                SherpaInputFinished(w->_asr, w->_asrStream);
                                /* drain the remaining chunks one bounded
                                   decode at a time, publishing the growing
                                   text and the drain progress after every
                                   step: on slow hardware the backlog can
                                   hold seconds of audio and a single
                                   unbounded flush would freeze the UI on
                                   "recognizing..." the whole time */
                                w->_asrProgress =
                                                SherpaStreamProgress(w->_asr, w->_asrStream);
                                while (!w->_asrDecodeStop &&
                                                SherpaIsStreamReady(w->_asr, w->_asrStream)) {
                                        w->setAsrText(SherpaDecodeStream(w->_asr, w->_asrStream),
                                                        NULL);
                                        w->_asrProgress =
                                                        SherpaStreamProgress(w->_asr, w->_asrStream);
                                }
                                if (SherpaStreamHasError(w->_asr, w->_asrStream))
                                        w->setAsrText(NULL, "decode failed, retry");
                                else
                                        w->setAsrText(SherpaGetResult(w->_asr),
                                                        "(no speech)");
                                SherpaReset(w->_asr, w->_asrStream);
                                pthread_mutex_lock(&w->_asrLock);
                                w->_asrState = ASR_LISTENING;
                                w->_silenceTicks = 0;
                                w->_speechTicks = 0;
                                w->_partialTicks = 0;
                                w->_fedSamples = 0;
                                pthread_mutex_unlock(&w->_asrLock);
                                didWork = true;
                        }
                        else if (online) {
                                /* streaming: decode each pending encoder chunk as
                                   soon as it is ready; every call is bounded to a
                                   single chunk so partial text refreshes
                                   continuously while speaking */
                                if (w->_asrState == ASR_RECORDING) {
                                        if (SherpaStreamHasError(w->_asr, w->_asrStream)) {
                                                /* sticky error: the stream will never
                                                   become ready again, so finish the
                                                   utterance now instead of recording
                                                   into a dead recognizer */
                                                pthread_mutex_lock(&w->_asrLock);
                                                w->_asrFinalPending = true;
                                                w->_asrPartialPending = false;
                                                w->_asrState = ASR_DECODING;
                                                pthread_mutex_unlock(&w->_asrLock);
                                                didWork = true;
                                        }
                                        else if (SherpaIsStreamReady(w->_asr, w->_asrStream)) {
                                                w->setAsrText(SherpaDecodeStream(w->_asr, w->_asrStream),
                                                                NULL);
                                                w->_asrProgress =
                                                                SherpaStreamProgress(w->_asr, w->_asrStream);
                                                didWork = true;
                                        }
                                }
                        }
                        else if (doPartial && SherpaIsStreamReady(w->_asr, w->_asrStream)) {
                                w->setAsrText(SherpaDecodeStream(w->_asr, w->_asrStream),
                                                "recognizing...");
                                didWork = true;
                        }

                        if (!didWork)
                                usleep(5000);
                }
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

        void asrQueueClearLocked(void) {
                _asrQRead = 0;
                _asrQWrite = 0;
                _asrQCount = 0;
        }

        void asrQueuePushLocked(const int16_t* s, int n) {
                if (s == NULL || n <= 0)
                        return;

                if (n >= ASR_QUEUE_SAMPLES) {
                        s += n - ASR_QUEUE_SAMPLES;
                        n = ASR_QUEUE_SAMPLES;
                        asrQueueClearLocked();
                }

                while ((_asrQCount + n) > ASR_QUEUE_SAMPLES && _asrQCount > 0) {
                        int drop = (_asrQCount + n) - ASR_QUEUE_SAMPLES;
                        if (drop > _asrQCount)
                                drop = _asrQCount;
                        _asrQRead = (_asrQRead + drop) % ASR_QUEUE_SAMPLES;
                        _asrQCount -= drop;
                        _asrPartialPending = false;
                }

                for (int i = 0; i < n; ++i) {
                        _asrQueue[_asrQWrite] = s[i];
                        _asrQWrite++;
                        if (_asrQWrite >= ASR_QUEUE_SAMPLES)
                                _asrQWrite = 0;
                }
                _asrQCount += n;
        }

        void asrQueuePush(const int16_t* s, int n) {
                pthread_mutex_lock(&_asrLock);
                asrQueuePushLocked(s, n);
                pthread_mutex_unlock(&_asrLock);
        }

	void prerollFeed(void) {
		if (_preFull) {
                        asrQueuePush(_preroll + _prePos, ASR_PREROLL - _prePos);
                        asrQueuePush(_preroll, _prePos);
			_fedSamples += ASR_PREROLL;
		}
		else if (_prePos > 0) {
                        asrQueuePush(_preroll, _prePos);
			_fedSamples += _prePos;
		}
		_prePos = 0;
		_preFull = false;
	}

        void setAsrText(const char* text, const char* fallback = NULL) {
                pthread_mutex_lock(&_asrLock);
                if (text != NULL && text[0] != '\0') {
                        strncpy(_asrText, text, sizeof(_asrText) - 1);
                        _asrText[sizeof(_asrText) - 1] = '\0';
                }
                else if (fallback != NULL) {
                        strncpy(_asrText, fallback, sizeof(_asrText) - 1);
                        _asrText[sizeof(_asrText) - 1] = '\0';
                }
                pthread_mutex_unlock(&_asrLock);
        }

	void asrProcess(const int16_t* mono, int n, int level) {
		_vadLevel = level;
		if (_asrState == ASR_LISTENING) {
			prerollPush(mono, n);
			if (level >= ASR_VAD_START) {
				_fedSamples = 0;
				_speechTicks = 1;
				_silenceTicks = 0;
                                _partialTicks = 0;
                                _asrProgress = 0;
                                setAsrText("recognizing...");
				prerollFeed(); /* mono already inside the ring */
				_asrState = ASR_RECORDING;
			}
		}
		else if (_asrState == ASR_RECORDING) {
                        asrQueuePush(mono, n);
			_fedSamples += n;
			if (level >= ASR_VAD_KEEP) {
				_speechTicks++;
				_silenceTicks = 0;
			}
			else {
				_silenceTicks++;
			}

                        if (_speechTicks >= ASR_MIN_SPEECH_TICKS) {
                                _partialTicks++;
                                if (_partialTicks >= ASR_PARTIAL_TICKS) {
                                        _asrPartialPending = true;
                                        _partialTicks = 0;
                                }
                        }

			if (_silenceTicks >= ASR_SILENCE_TICKS || _fedSamples >= ASR_MAX_SAMPLES) {
				if (_speechTicks >= ASR_MIN_SPEECH_TICKS) {
                                        _asrPartialPending = false;
                                        _asrFinalPending = true;
                                        _asrState = ASR_DECODING;
                                        setAsrText("recognizing...");
				}
				else {
                                        pthread_mutex_lock(&_asrLock);
                                        _asrResetPending = true;
                                        _asrPartialPending = false;
                                        _asrFinalPending = false;
                                        asrQueueClearLocked();
                                        pthread_mutex_unlock(&_asrLock);
					_asrState = ASR_LISTENING;
                                        _silenceTicks = 0;
                                        _speechTicks = 0;
                                        _partialTicks = 0;
                                        _fedSamples = 0;
				}
			}
		}
                else if (_asrState == ASR_DECODING) {
                        prerollPush(mono, n);
                }
	}

	const char* asrStatus(void) const {
		switch (_asrState) {
		case ASR_FAILED: return "[asr disabled]";
		default: return "[listening]";
		}
	}

	void drawDbg(graph_t* g, XTheme* theme, const grect_t& r) {
		char status[48];
		char detail[112];
		char text[256];
		char lbuf[128];
		SherpaStreamInfo si;
		bool hasInfo = false;

		/* live pipeline snapshot; the getters only read telemetry counters
		   so polling from the UI thread while the decode thread runs is ok */
		if ((_asrState == ASR_RECORDING || _asrState == ASR_DECODING) &&
				_asr != NULL && _asrStream != NULL) {
			SherpaStreamGetInfo(_asr, _asrStream, &si);
			hasInfo = true;
		}
		int pct = hasInfo ? (int)si.progress : (int)_asrProgress;

		if (_asrState == ASR_LOADING)
			snprintf(status, sizeof(status), "[loading model %d%%]",
					(int)SherpaLoadProgress());
		else if (_asrState == ASR_DECODING)
			/* drain progress: how much of the buffered audio the
			   decoder has consumed, so the tail flush is visible
			   instead of a frozen "recognizing..." */
			snprintf(status, sizeof(status), "[recognizing %d%%]", pct);
		else if (_asrState == ASR_RECORDING)
			/* how far the decoder keeps up while still speaking */
			snprintf(status, sizeof(status), "[recording %d%%]", pct);
		else
			strcpy(status, asrStatus());

		/* activity detail: what the pipeline is concretely doing now */
		detail[0] = '\0';
		if (hasInfo)
			snprintf(detail, sizeof(detail),
					"chunk %d/%d %d.%ds/%d.%ds tok%d %s %d%%",
					(int)si.chunks_done, (int)si.chunks_total,
					(int)(si.decoded_ms / 1000),
					(int)((si.decoded_ms % 1000) / 100),
					(int)(si.total_ms / 1000),
					(int)((si.total_ms % 1000) / 100),
					(int)si.tokens, si.phase, (int)si.phase_pct);
		else if (_asrState == ASR_LISTENING)
			snprintf(detail, sizeof(detail), "vad %d / start %d",
					(int)_vadLevel, ASR_VAD_START);

		graph_fill_rect(g, r.x, r.y, r.w, r.h, 0xff1c1408);
		graph_draw_text_font(g, r.x + 4, r.y + 2,
				status, theme->getFont(), 14, 0xff8fd6ff);
		if (detail[0] != '\0')
			graph_draw_text_font(g, r.x + 4, r.y + 2 + MIC_DBG_LINE_H,
					detail, theme->getFont(), 14, 0xff9aa8b8);

		/* wrap the recognized text over the remaining lines; width is
		   estimated per UTF-8 char (CJK ~ fontSize, ASCII ~ half) */
		pthread_mutex_lock(&_asrLock);
		strncpy(text, (const char*)_asrText, sizeof(text) - 1);
		text[sizeof(text) - 1] = '\0';
		pthread_mutex_unlock(&_asrLock);

		const char* p = text;
		int y = r.y + 2 + 2 * MIC_DBG_LINE_H;
		for (int ln = 0; ln < MIC_DBG_LINES - 1 && *p != '\0'; ln++) {
			int li = 0;
			int w = 0;
			while (*p != '\0') {
				if (*p == '\n') {
					p++;
					break;
				}
				uint8_t c = (uint8_t)*p;
				int bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
				int cw = bytes == 1 ? 7 : 14;
				if (w + cw > r.w - 8 || li + bytes >= (int)sizeof(lbuf))
					break;
				for (int k = 0; k < bytes && *p != '\0'; k++)
					lbuf[li++] = *p++;
				w += cw;
			}
			lbuf[li] = '\0';
			if (li > 0)
				graph_draw_text_font(g, r.x + 4, y,
						lbuf, theme->getFont(), 14, 0xffffd080);
			y += MIC_DBG_LINE_H;
		}

		/* thin progress bar while loading or draining the decode backlog */
		if (_asrState == ASR_LOADING || _asrState == ASR_DECODING ||
				_asrState == ASR_RECORDING) {
			if (_asrState == ASR_LOADING)
				pct = (int)SherpaLoadProgress();
			if (pct < 0)
				pct = 0;
			if (pct > 100)
				pct = 100;
			graph_fill_rect(g, r.x + 2, r.y + r.h - 4, r.w - 4, 3, 0xff3a3020);
			graph_fill_rect(g, r.x + 2, r.y + r.h - 4,
					((r.w - 4) * pct) / 100, 3, 0xffffd080);
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
                _asrModelPath = chooseModelPath();
                _asrTokensPath = chooseTokensPath();
                _asrFellBack = strcmp(_asrModelPath,
                                ASR_ONLINE_TRANSDUCER_ENCODER_PATH) != 0;
                pthread_mutex_init(&_asrLock, NULL);
                _asrDecodeThreadStarted = false;
                _asrDecodeStop = false;
                _asrResetPending = false;
                _asrPartialPending = false;
                _asrFinalPending = false;
		_asrState = ASR_LOADING;
		_asrProgress = 0;
		_vadLevel = 0;
                snprintf(_asrText, sizeof(_asrText), "loading %s", _asrModelPath);
		_asrDc = 0;
		_silenceTicks = 0;
		_speechTicks = 0;
                _partialTicks = 0;
		_fedSamples = 0;
		_prePos = 0;
		_preFull = false;
                _asrQRead = 0;
                _asrQWrite = 0;
                _asrQCount = 0;
		if (thread_create(asrLoadThread, this) < 0)
			asrLoadThread(this); /* fall back to blocking load */
	}

	~MicWidget() {
		if (_fd >= 0)
			close(_fd);
                _asrDecodeStop = true;
                if (_asrDecodeThreadStarted)
                        pthread_join(_asrDecodeTid, NULL);
		if (_asr != NULL) {
			if (_asrStream != NULL)
				SherpaDestroyStream(_asr, _asrStream);
			SherpaDestroyRecognizer(_asr);
		}
                pthread_mutex_destroy(&_asrLock);
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
