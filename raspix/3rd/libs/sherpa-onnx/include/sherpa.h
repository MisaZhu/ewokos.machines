/**
 * sherpa.h - minimal sherpa-onnx style offline recognizer for EwokOS.
 *
 * Targets several sherpa-onnx model families, auto-detected from metadata:
 *   - SenseVoice / offline paraformer / NeMo CTC
 *   - streaming Zipformer2 CTC / WeNet CTC single-model exports
 * (e.g. sherpa-onnx-sense-voice-zh-en-ja-ko-yue, float32 or int8):
 *
 *   PCM (any rate, resampled to 16 kHz) -> kaldi fbank (80 bins, hamming,
 *   snip_edges) -> LFR (window 7, shift 6 -> 560 dims) -> CMVN from model
 *   metadata -> ONNX model (x, x_length, language, text_norm)
 *   -> greedy CTC decode -> text.
 *
 * Typical usage (energy-VAD segmented offline decoding):
 *   SherpaRecognizer *r = SherpaCreateRecognizer(model, tokens);
 *   SherpaStream *s = SherpaCreateStream(r, 48000);
 *   ... SherpaAcceptWaveform(r, s, samples, n);  // one speech segment
 *   const char *text = SherpaDecode(r, s);       // also resets the stream
 *
 * Streaming-style usage with an offline model:
 *   SherpaRecognizer *r = SherpaCreateRecognizer(model, tokens);
 *   SherpaStream *s = SherpaCreateStream(r, 48000);
 *   ... SherpaAcceptWaveform(r, s, samples, n);
 *   if (SherpaIsStreamReady(r, s))
 *     printf("%s\n", SherpaDecodeStream(r, s));
 *   ...
 *   SherpaInputFinished(r, s);
 *   printf("%s\n", SherpaDecodeStream(r, s));
 *   SherpaReset(r, s);
 */
#ifndef SHERPA_ONNX_PORT_SHERPA_H_
#define SHERPA_ONNX_PORT_SHERPA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SherpaRecognizer SherpaRecognizer;
typedef struct SherpaStream SherpaStream;

/**
 * @param model_path  .onnx model (SenseVoice CTC, float32 or int8)
 * @param tokens_path tokens.txt shipped with the model
 * @return recognizer handle or NULL on failure
 */
SherpaRecognizer *SherpaCreateRecognizer(const char *model_path,
                                         const char *tokens_path);
void SherpaDestroyRecognizer(SherpaRecognizer *r);

/**
 * Model load progress in percent (0..100). Poll from the UI thread while
 * SherpaCreateRecognizer runs on a worker thread.
 */
int32_t SherpaLoadProgress(void);

/**
 * Language hint: 0 = auto (default), 3 = zh, 4 = en, 7 = yue, 11 = ja,
 * 12 = ko (ids come from the model metadata).
 */
void SherpaSetLanguage(SherpaRecognizer *r, int32_t lang_id);

/** Inverse text normalization (numbers/punctuation): 1 = on (default). */
void SherpaSetUseItn(SherpaRecognizer *r, int32_t use_itn);

/**
 * @param input_rate sample rate of the PCM passed to SherpaAcceptWaveform
 *                   (resampled to 16 kHz internally when != 16000)
 */
SherpaStream *SherpaCreateStream(SherpaRecognizer *r, int32_t input_rate);
void SherpaDestroyStream(SherpaRecognizer *r, SherpaStream *s);

/** Discard all buffered audio. */
void SherpaReset(SherpaRecognizer *r, SherpaStream *s);

/** Feed 16-bit mono PCM at the stream's input rate. */
void SherpaAcceptWaveform(SherpaRecognizer *r, SherpaStream *s,
                          const int16_t *samples, int32_t n);

/**
 * Mark the current utterance finished so the frontend can flush tail frames.
 * Call SherpaReset() before feeding the next utterance.
 */
void SherpaInputFinished(SherpaRecognizer *r, SherpaStream *s);

/**
 * Returns non-zero when enough new audio has arrived to make another
 * streaming-style decode worthwhile.
 */
int32_t SherpaIsStreamReady(SherpaRecognizer *r, SherpaStream *s);

/**
 * Re-decode the current stream contents without resetting it.
 * This provides incremental text for UI updates while keeping the stream
 * open for more audio until SherpaInputFinished() + SherpaReset().
 *
 * @return recognized text so far; the pointer is valid until the next decode
 *         or reset on this recognizer.
 */
const char *SherpaDecodeStream(SherpaRecognizer *r, SherpaStream *s);

/** Return the most recent streaming/offline result for this recognizer. */
const char *SherpaGetResult(SherpaRecognizer *r);

/**
 * Decode all buffered audio, then reset the stream for the next segment.
 * @return recognized text ("" when audio too short); the pointer is valid
 *         until the next SherpaDecode call on this recognizer.
 */
const char *SherpaDecode(SherpaRecognizer *r, SherpaStream *s);

#ifdef __cplusplus
}
#endif

#endif  // SHERPA_ONNX_PORT_SHERPA_H_
