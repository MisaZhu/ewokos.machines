/**
 * sherpa.cc - minimal sherpa-onnx style offline recognizer for EwokOS.
 * See sherpa.h for the pipeline description.
 *
 * SenseVoice recipe (from sherpa-onnx offline-recognizer-sense-voice-impl.h):
 *   - fbank: 16 kHz, 80 mels, hamming window, snip_edges=true, dither=0,
 *     preemph 0.97, remove_dc_offset, low_freq 20, high_freq 0 (= nyquist),
 *     samples at raw int16 scale (metadata normalize_samples=0)
 *   - LFR: stack lfr_window_size(7) frames, shift lfr_window_shift(6)
 *   - CMVN: (x + neg_mean) * inv_stddev, vectors from model metadata
 *   - inputs: x [1,T,560] f32, x_length/language/text_norm [1] i32
 *   - decode: greedy CTC over T+4 output frames; first 4 tokens are meta
 *     (lang/emotion/event/itn) and are skipped
 *
 * Paraformer recipe (offline-recognizer-paraformer-impl.h):
 *   - same fbank/LFR/CMVN frontend as SenseVoice
 *   - inputs: speech [1,T,560] f32 + speech_lengths [1] i32
 *   - decode: per-position argmax until </s>; "@@"-suffixed tokens are
 *     BPE continuations joined with the next token
 *
 * NeMo CTC recipe (offline-recognizer-ctc-impl.h, EncDecCTCModelBPE):
 *   - samples scaled to [-1,1]; hann window, snip_edges=false,
 *     remove_dc_offset=false, low_freq 0, librosa (Slaney) mel banks
 *   - per-feature normalization (per-dim mean/stddev over frames)
 *   - inputs: audio_signal [1,80,T] f32 + length [1] i64
 *   - decode: greedy CTC, blank is the last vocab entry
 *
 * The pipeline is picked automatically from the model_type metadata.
 *
 * The linear resampler is ported from sherpa-onnx's linear-resample.cc
 * (Apache License 2.0, Xiaomi Corporation).
 */
#include "sherpa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "fbank.h"
#include "onnx.h"
#include "vecutil.h"

namespace {

/* linear resampler with carried-over phase (from sherpa-onnx) */
class LinearResampler {
 public:
  LinearResampler(int32_t in_rate, int32_t out_rate)
      : in_rate_(in_rate), out_rate_(out_rate), phase_(0.0f), last_(0.0f) {}

  void Resample(const float *in, int32_t n, std::vector<float> *out) {
    if (in_rate_ == out_rate_) {
      out->clear();
      for (int32_t i = 0; i < n; ++i) out->push_back(in[i]);
      return;
    }
    float step = static_cast<float>(in_rate_) / out_rate_;
    float pos = phase_;
    for (int32_t i = 0; i < n; ++i) {
      float cur = in[i];
      while (pos < 1.0f) {
        out->push_back(last_ + (cur - last_) * pos);
        pos += step;
      }
      pos -= 1.0f;
      last_ = cur;
    }
    phase_ = pos;
  }

  void Reset() {
    phase_ = 0.0f;
    last_ = 0.0f;
  }

 private:
  int32_t in_rate_;
  int32_t out_rate_;
  float phase_;
  float last_;
};

knf::FbankOptions MakeSenseVoiceFbankOpts() {
  knf::FbankOptions opts;
  opts.frame_opts.samp_freq = 16000.0f;
  opts.frame_opts.dither = 0.0f;
  opts.frame_opts.preemph_coeff = 0.97f;
  opts.frame_opts.remove_dc_offset = true;
  opts.frame_opts.window_type = "hamming";
  opts.frame_opts.snip_edges = true;
  opts.mel_opts.num_bins = 80;
  opts.mel_opts.low_freq = 20.0f;
  opts.mel_opts.high_freq = 0.0f;
  return opts;
}

knf::FbankOptions MakeNemoFbankOpts() {
  knf::FbankOptions opts;
  opts.frame_opts.samp_freq = 16000.0f;
  opts.frame_opts.dither = 0.0f;
  opts.frame_opts.preemph_coeff = 0.97f;
  opts.frame_opts.remove_dc_offset = false;
  opts.frame_opts.window_type = "hann";
  opts.frame_opts.snip_edges = false;
  opts.mel_opts.num_bins = 80;
  opts.mel_opts.low_freq = 0.0f;
  opts.mel_opts.high_freq = 0.0f;
  opts.mel_opts.is_librosa = true;
  return opts;
}

knf::FbankOptions MakeOnlineCtcFbankOpts() {
  knf::FbankOptions opts;
  opts.frame_opts.samp_freq = 16000.0f;
  opts.frame_opts.dither = 0.0f;
  opts.frame_opts.preemph_coeff = 0.97f;
  opts.frame_opts.remove_dc_offset = true;
  opts.frame_opts.window_type = "povey";
  /* kaldi/sherpa-onnx default for online models: snip_edges=true.
     With snip_edges=false the frame grid shifts half a window and extra
     tail frames appear, breaking the chunk alignment every streaming
     encoder relies on (decode_chunk_len frames per step) */
  opts.frame_opts.snip_edges = true;
  opts.mel_opts.num_bins = 80;
  opts.mel_opts.low_freq = 20.0f;
  opts.mel_opts.high_freq = -400.0f;
  return opts;
}

int32_t MetaInt(const sonnx::Model &m, const char *key, int32_t dflt) {
  std::string v = m.Metadata(key);
  return v.empty() ? dflt : atoi(v.c_str());
}

bool HasSuffix(const std::string &s, const char *suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string DirName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? std::string(path, static_cast<size_t>(slash - path))
               : std::string(".");
}

std::string JoinPath(const std::string &dir, const char *name) {
  if (dir.empty() || dir == ".") return std::string(name);
  return dir + "/" + name;
}

bool FindSiblingModel(const char *path, const char *name, std::string *out) {
  std::string p = JoinPath(DirName(path), name);
  if (access(p.c_str(), R_OK) != 0) return false;
  *out = p;
  return true;
}

void ParseFloatList(const std::string &s, std::vector<float> *out) {
  out->clear();
  const char *p = s.c_str();
  while (*p) {
    char *end = NULL;
    double v = strtod(p, &end);  // EwokOS libc has no strtof
    if (end == p) break;
    out->push_back(static_cast<float>(v));
    p = end;
    while (*p == ' ' || *p == ',') ++p;
  }
}

/* --- sherpa-onnx text-utils.cc RemoveSpaceBetweenCjk port --- */
static uint32_t Utf8Next(const std::string &s, size_t *i) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(s.data());
  uint8_t c = p[*i];
  if (c < 0x80) {
    ++*i;
    return c;
  }
  int cont = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
  uint32_t cp = c & (0x7F >> cont);
  for (int k = 0; k < cont && *i + 1 < s.size(); ++k)
    cp = (cp << 6) | (p[++*i] & 0x3F);
  ++*i;
  return cp;
}

static void Utf8Append(uint32_t cp, std::string *out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static bool IsCjkCp(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
         (cp >= 0xA840 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF65 && cp <= 0xFFDC) ||
         (cp >= 0x20000 && cp <= 0x2FFFF);
}

static bool IsPunctCp(uint32_t cp) {
  return (cp >= 0x21 && cp <= 0x2F) || (cp >= 0x3A && cp <= 0x40) ||
         (cp >= 0x5B && cp <= 0x60) || (cp >= 0x7B && cp <= 0x7E) ||
         (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
         (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
         (cp >= 0xFF5B && cp <= 0xFF65);
}

/* drop a space when it sits between two CJK chars or before punctuation
   (sherpa-onnx Convert() applies this to every transducer result) */
static std::string RemoveSpaceBetweenCjk(const std::string &text) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < text.size();) cps.push_back(Utf8Next(text, &i));
  if (cps.size() < 2) return text;
  std::string out;
  Utf8Append(cps[0], &out);
  for (size_t i = 1; i < cps.size(); ++i) {
    if (cps[i] == ' ' && i + 1 < cps.size() &&
        ((IsCjkCp(cps[i - 1]) && IsCjkCp(cps[i + 1])) ||
         IsPunctCp(cps[i + 1])))
      continue;
    Utf8Append(cps[i], &out);
  }
  return out;
}

/* replace UTF-8 "▁" (U+2581, sentencepiece word marker) with a space */
void ReplaceWordMarkers(std::string *text) {
  std::string out;
  out.reserve(text->size());
  for (size_t i = 0; i < text->size();) {
    if (i + 2 < text->size() && (*text)[i] == '\xe2' &&
        (*text)[i + 1] == '\x96' && (*text)[i + 2] == '\x81') {
      out += ' ';
      i += 3;
    } else {
      out += (*text)[i];
      ++i;
    }
  }
  // trim leading space produced by a leading marker
  size_t b = out.find_first_not_of(' ');
  *text = b == std::string::npos ? std::string() : out.substr(b);
}

}  // namespace

struct SherpaRecognizer {
  enum Kind { kSenseVoice = 0, kParaformer = 1, kNemoCtc = 2,
              kOnlineZipformer2Ctc = 3, kOnlineWenetCtc = 4,
              kOnlineZipformerTransducer = 5 };

  sonnx::Model model;
  sonnx::Model decoder;
  sonnx::Model joiner;
  std::vector<std::string> tokens;
  knf::FbankOptions fbank_opts;
  int32_t kind = kSenseVoice;
  std::vector<float> neg_mean;   // 560
  std::vector<float> inv_stddev; // 560
  int32_t lfr_window = 7;
  int32_t lfr_shift = 6;
  int32_t blank_id = 0;
  int32_t eos_id = 2;      // paraformer </s>
  int32_t language = 0;    // 0 = auto
  int32_t with_itn = 14;
  int32_t without_itn = 15;
  int32_t use_itn = 1;
  int32_t normalize_samples = 0;
  int32_t transducer_decode_chunk_len = 0;
  int32_t transducer_T = 0;
  int32_t transducer_context_size = 0;
  int32_t decoder_input_dtype = sonnx::kInt64;
  std::string result;
};

/* decode phase telemetry: written by the decode thread, polled by the
   UI thread through SherpaStreamGetInfo/SherpaStreamProgress */
enum {
  kPhaseIdle = 0,
  kPhaseEnc,    /* encoder graph running (g_run_progress advances) */
  kPhaseSearch, /* greedy joiner search over encoder output frames */
  kPhaseModel   /* offline: whole-utterance model run */
};

struct SherpaStream {
  knf::OnlineFbank *fbank = NULL;
  LinearResampler *resampler = NULL;  // NULL when input is already 16 kHz
  int32_t in_rate = 16000;
  int32_t last_decode_frames = 0;
  int32_t chunk_cursor = 0;
  bool input_finished = false;
  bool decode_error = false;  // sticky: a failed chunk must not be retried
  volatile int32_t phase_id = kPhaseIdle;
  volatile int32_t sub_num = 0;  // search frames done in current chunk
  volatile int32_t sub_den = 0;  // encoder output frames in current chunk
  std::vector<int64_t> hyp;
  sonnx::Tensor decoder_out;
  std::vector<sonnx::Tensor> encoder_states;
};

static bool LoadTokens(const char *path, std::vector<std::string> *tokens) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "sherpa: cannot open tokens file %s\n", path);
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<char> buf(static_cast<size_t>(sz) + 1, 0);
  size_t n = fread(&buf[0], 1, static_cast<size_t>(sz), fp);
  fclose(fp);
  (void)n;

  // each line: <token> <id>; token itself may contain spaces, id is last
  const char *p = vdata(buf);
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ')) --len;
    if (len > 0) {
      size_t sp = len;
      while (sp > 0 && p[sp - 1] != ' ') --sp;
      if (sp > 0) {
        int32_t id = atoi(p + sp);
        if (id >= 0) {
          if (static_cast<size_t>(id) >= tokens->size())
            tokens->resize(static_cast<size_t>(id) + 1);
          (*tokens)[id].assign(p, sp - 1);
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  /* sanity check: real vocabs have thousands of non-empty entries. A
     git-lfs pointer or truncated copy parses into a handful of mostly
     empty strings; decoding would still run but every token would map to
     "" and the result text would silently stay empty. */
  size_t usable = 0;
  for (size_t i = 0; i < tokens->size(); ++i)
    if (!(*tokens)[i].empty()) ++usable;
  if (usable < 100) {
    fprintf(stderr, "sherpa: %s: only %d usable tokens (lfs pointer?)\n",
            path, static_cast<int>(usable));
    return false;
  }
  return true;
}

/* encoder chunks processed per streaming decode call while input is
   still open; bounding per-call work lets the caller interleave audio
   feeding and partial-result updates. The final flush (input_finished)
   always runs to completion. */
#define SHERPA_STREAM_CHUNK_BUDGET 1

static bool TryLoadTransducerModels(SherpaRecognizer *r, const char *model_path) {
  if (!r || (!HasSuffix(model_path, "encoder.int8.onnx") &&
             !HasSuffix(model_path, "encoder.onnx")))
    return false;

  std::string decoder_path, joiner_path;
  if (!FindSiblingModel(model_path, "decoder.int8.onnx", &decoder_path) &&
      !FindSiblingModel(model_path, "decoder.onnx", &decoder_path))
    return false;
  if (!FindSiblingModel(model_path, "joiner.int8.onnx", &joiner_path) &&
      !FindSiblingModel(model_path, "joiner.onnx", &joiner_path))
    return false;

  if (!r->decoder.Load(decoder_path.c_str()) ||
      !r->joiner.Load(joiner_path.c_str()))
    return false;

  r->kind = SherpaRecognizer::kOnlineZipformerTransducer;
  r->fbank_opts = MakeOnlineCtcFbankOpts();
  r->blank_id = 0;
  r->normalize_samples = 1;
  r->transducer_decode_chunk_len =
      MetaInt(r->model, "decode_chunk_len", 0);
  r->transducer_T = MetaInt(r->model, "T", 0);
  r->transducer_context_size =
      MetaInt(r->decoder, "context_size", 2);
  r->decoder_input_dtype = r->decoder.InputType(0);

  if (r->transducer_decode_chunk_len <= 0 || r->transducer_T <= 0 ||
      r->transducer_context_size <= 0) {
    fprintf(stderr, "sherpa: bad transducer metadata\n");
    return false;
  }

  return true;
}

SherpaRecognizer *SherpaCreateRecognizer(const char *model_path,
                                         const char *tokens_path) {
  sonnx::g_load_progress = 0;
  SherpaRecognizer *r = new SherpaRecognizer();
  if (!r->model.Load(model_path)) {
    delete r;
    return NULL;
  }
  if (!LoadTokens(tokens_path, &r->tokens)) {
    delete r;
    return NULL;
  }

  if (HasSuffix(model_path, "encoder.int8.onnx") ||
      HasSuffix(model_path, "encoder.onnx")) {
    /* named like a transducer encoder: either the full triple loads or
       the recognizer fails -- never fall through to a wrong pipeline
       (e.g. treating the encoder as a CTC model produces garbage). */
    if (TryLoadTransducerModels(r, model_path)) return r;
    fprintf(stderr, "sherpa: incomplete transducer model set for %s\n",
            model_path);
    delete r;
    return NULL;
  }

  // pipeline is picked from the model_type metadata
  std::string mt = r->model.Metadata("model_type");
  if (mt == "paraformer")
    r->kind = SherpaRecognizer::kParaformer;
  else if (mt == "zipformer2")
    r->kind = SherpaRecognizer::kOnlineZipformer2Ctc;
  else if (mt == "wenet_ctc")
    r->kind = SherpaRecognizer::kOnlineWenetCtc;
  else if (mt == "EncDecCTCModelBPE")
    r->kind = SherpaRecognizer::kNemoCtc;
  else
    r->kind = SherpaRecognizer::kSenseVoice;

  if (r->kind == SherpaRecognizer::kNemoCtc) {
    r->fbank_opts = MakeNemoFbankOpts();
    r->normalize_samples = 1;
    return r;
  }

  if (r->kind == SherpaRecognizer::kOnlineZipformer2Ctc ||
      r->kind == SherpaRecognizer::kOnlineWenetCtc) {
    r->fbank_opts = MakeOnlineCtcFbankOpts();
    r->blank_id = MetaInt(r->model, "blank_id", 0);
    r->normalize_samples = 1;
    return r;
  }

  // SenseVoice / paraformer share the LFR + CMVN frontend
  r->lfr_window = MetaInt(r->model, "lfr_window_size", 7);
  r->lfr_shift = MetaInt(r->model, "lfr_window_shift", 6);
  r->blank_id = MetaInt(r->model, "blank_id", 0);
  r->with_itn = MetaInt(r->model, "with_itn", 14);
  r->without_itn = MetaInt(r->model, "without_itn", 15);

  ParseFloatList(r->model.Metadata("neg_mean"), &r->neg_mean);
  ParseFloatList(r->model.Metadata("inv_stddev"), &r->inv_stddev);
  int32_t lfr_dim = 80 * r->lfr_window;
  if (static_cast<int32_t>(r->neg_mean.size()) != lfr_dim ||
      static_cast<int32_t>(r->inv_stddev.size()) != lfr_dim) {
    fprintf(stderr, "sherpa: bad CMVN metadata (%d/%d, want %d)\n",
            static_cast<int>(r->neg_mean.size()),
            static_cast<int>(r->inv_stddev.size()), static_cast<int>(lfr_dim));
    delete r;
    return NULL;
  }

  if (r->kind == SherpaRecognizer::kParaformer) {
    for (size_t i = 0; i < r->tokens.size(); ++i) {
      if (r->tokens[i] == "</s>") {
        r->eos_id = static_cast<int32_t>(i);
        break;
      }
    }
  }

  r->fbank_opts = MakeSenseVoiceFbankOpts();
  r->normalize_samples = 0;
  return r;
}

void SherpaDestroyRecognizer(SherpaRecognizer *r) { delete r; }

int32_t SherpaIsOnline(SherpaRecognizer *r) {
  return r && r->kind == SherpaRecognizer::kOnlineZipformerTransducer ? 1 : 0;
}

int32_t SherpaLoadProgress(void) { return sonnx::g_load_progress; }

void SherpaSetLanguage(SherpaRecognizer *r, int32_t lang_id) {
  if (r) r->language = lang_id;
}

void SherpaSetUseItn(SherpaRecognizer *r, int32_t use_itn) {
  if (r) r->use_itn = use_itn ? 1 : 0;
}

static sonnx::Tensor MakeTypedTensor(const std::vector<int64_t> &shape,
                                     int dtype) {
  if (dtype == sonnx::kFloat) return sonnx::Tensor::Float(shape);
  if (dtype == sonnx::kUint8 || dtype == sonnx::kInt8 || dtype == sonnx::kBool)
    return sonnx::Tensor::Byte(shape, dtype);
  return sonnx::Tensor::Int(shape, dtype);
}

static int TransducerDbg() {
  static int on = -1;
  if (on < 0) on = getenv("SHERPA_DEBUG") ? 1 : 0;
  return on;
}

static void DumpBin(const char *name, const float *p, int64_t n) {
  FILE *fp = fopen(name, "wb");
  if (!fp) return;
  fwrite(p, sizeof(float), static_cast<size_t>(n), fp);
  fclose(fp);
}

static bool RunTransducerDecoder(SherpaRecognizer *r,
                                 const std::vector<int64_t> &hyp,
                                 sonnx::Tensor *decoder_out) {
  if (!r || !decoder_out) return false;

  std::vector<int64_t> shape;
  shape.push_back(1);
  shape.push_back(r->transducer_context_size);
  sonnx::Tensor y = MakeTypedTensor(shape, r->decoder_input_dtype);
  for (int32_t i = 0; i < r->transducer_context_size; ++i) {
    int64_t v = hyp[hyp.size() - r->transducer_context_size + i];
    if (y.IsByte())
      y.b[static_cast<size_t>(i)] = static_cast<uint8_t>(v);
    else
      y.i[static_cast<size_t>(i)] = v;
  }

  std::vector<const sonnx::Tensor *> inputs;
  inputs.push_back(&y);
  std::vector<sonnx::Tensor> outputs;
  if (!r->decoder.Run(inputs, &outputs) || outputs.empty()) {
    fprintf(stderr, "sherpa: transducer decoder run failed\n");
    return false;
  }
  *decoder_out = std::move(outputs[0]);
  return true;
}

static void BuildTransducerText(SherpaRecognizer *r, SherpaStream *s) {
  std::string text;
  if (!r || !s) return;
  for (size_t i = static_cast<size_t>(r->transducer_context_size);
       i < s->hyp.size(); ++i) {
    int64_t id = s->hyp[i];
    if (id >= 0 && id < static_cast<int64_t>(r->tokens.size())) {
      const std::string &tok = r->tokens[static_cast<size_t>(id)];
      if (tok == "<unk>") continue;
      text += tok;
    }
  }
  ReplaceWordMarkers(&text);
  r->result = RemoveSpaceBetweenCjk(text);
}

static bool InitTransducerStream(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s) return false;

  s->hyp.clear();
  for (int32_t i = 0; i < r->transducer_context_size; ++i)
    s->hyp.push_back(r->blank_id);

  s->encoder_states.clear();
  for (int i = 1; i < r->model.NumInputs(); ++i) {
    std::vector<int64_t> shape = r->model.InputShape(i);
    for (size_t j = 0; j < shape.size(); ++j)
      if (shape[j] <= 0) shape[j] = 1;
    s->encoder_states.push_back(
        MakeTypedTensor(shape, r->model.InputType(i)));
  }

  if (!RunTransducerDecoder(r, s->hyp, &s->decoder_out)) return false;
  if (TransducerDbg())
    DumpBin("sonnx_dec0.bin", s->decoder_out.pf(), s->decoder_out.Numel());
  return true;
}

SherpaStream *SherpaCreateStream(SherpaRecognizer *r, int32_t input_rate) {
  if (!r) return NULL;
  SherpaStream *s = new SherpaStream();
  s->fbank = new knf::OnlineFbank(r->fbank_opts);
  if (input_rate > 0 && input_rate != 16000) {
    s->resampler = new LinearResampler(input_rate, 16000);
    s->in_rate = input_rate;
  }
  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer &&
      !InitTransducerStream(r, s)) {
    SherpaDestroyStream(r, s);
    return NULL;
  }
  return s;
}

void SherpaDestroyStream(SherpaRecognizer *r, SherpaStream *s) {
  (void)r;
  if (!s) return;
  delete s->fbank;
  delete s->resampler;
  delete s;
}

void SherpaReset(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s) return;
  delete s->fbank;
  s->fbank = new knf::OnlineFbank(r->fbank_opts);
  if (s->resampler) s->resampler->Reset();
  s->last_decode_frames = 0;
  s->chunk_cursor = 0;
  s->input_finished = false;
  s->decode_error = false;
  s->phase_id = kPhaseIdle;
  s->sub_num = 0;
  s->sub_den = 0;
  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer) {
    InitTransducerStream(r, s);
  } else {
    s->hyp.clear();
    s->encoder_states.clear();
    s->decoder_out = sonnx::Tensor();
  }
}

void SherpaAcceptWaveform(SherpaRecognizer *r, SherpaStream *s,
                          const int16_t *samples, int32_t n) {
  if (!r || !s || !samples || n <= 0 || s->input_finished) return;

  // SenseVoice/paraformer keep the raw int16 scale (normalize_samples=0);
  // NeMo and online CTC models expect samples in [-1, 1]
  float scale = r->normalize_samples ? 1.0f / 32768.0f : 1.0f;
  std::vector<float> f(static_cast<size_t>(n));
  for (int32_t i = 0; i < n; ++i) f[i] = samples[i] * scale;

  if (!s->resampler) {
    s->fbank->AcceptWaveform(16000.0f, vdata(f), n);
    return;
  }
  std::vector<float> out;
  s->resampler->Resample(vdata(f), n, &out);
  if (!out.empty()) {
    s->fbank->AcceptWaveform(16000.0f, vdata(out),
                             static_cast<int32_t>(out.size()));
  }
}

/* streaming zipformer2 readiness is chunk_cursor + T < frames_ready, so
   without help the audio tail (up to ~T frames) would never be decoded.
   sherpa-onnx leaves it to the caller to append trailing silence; do it
   here once so the end of the utterance always reaches the encoder. */
static void PadTransducerTail(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s) return;
  if (r->kind != SherpaRecognizer::kOnlineZipformerTransducer) return;
  if (r->transducer_T <= 0 || r->transducer_decode_chunk_len <= 0) return;
  int32_t frames = r->transducer_T + r->transducer_decode_chunk_len;
  std::vector<float> zeros(static_cast<size_t>(frames) * 160, 0.0f);
  s->fbank->AcceptWaveform(16000.0f, vdata(zeros),
                           static_cast<int32_t>(zeros.size()));
}

void SherpaInputFinished(SherpaRecognizer *r, SherpaStream *s) {
  if (!s || s->input_finished) return;
  PadTransducerTail(r, s);
  s->fbank->InputFinished();
  s->input_finished = true;
}

static int32_t MinDecodeFrames(const SherpaRecognizer *r) {
  if (!r) return 1;
  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer)
    return r->transducer_decode_chunk_len > 0 ? r->transducer_decode_chunk_len
                                              : 1;
  if (r->kind == SherpaRecognizer::kNemoCtc) return 4;
  if (r->kind == SherpaRecognizer::kOnlineZipformer2Ctc ||
      r->kind == SherpaRecognizer::kOnlineWenetCtc)
    return 8;
  return r->lfr_shift > 0 ? r->lfr_shift : 1;
}

int32_t SherpaIsStreamReady(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s || s->decode_error) return 0;

  int32_t ready = s->fbank->NumFramesReady();
  if (ready <= 0) return 0;

  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer)
    /* sherpa-onnx online-recognizer-transducer-impl.h IsReady:
       processed + ChunkSize(T) < NumFramesReady, same after finish */
    return s->chunk_cursor + r->transducer_T < ready;

  if (s->input_finished && ready != s->last_decode_frames) return 1;
  return (ready - s->last_decode_frames) >= MinDecodeFrames(r);
}

/* LFR + CMVN -> x [1, outT, 80*lfr_window]; returns outT (<=0: too short) */
static int32_t LfrCmvn(SherpaRecognizer *r, const std::vector<float> &feats,
                       int32_t T, sonnx::Tensor *x) {
  const int32_t dim = 80;
  const int32_t w = r->lfr_window, sh = r->lfr_shift;
  const int32_t lfr_dim = dim * w;
  int32_t outT = (T - w) / sh + 1;
  if (T < w || outT <= 0) return 0;

  std::vector<int64_t> xshape;
  xshape.push_back(1);
  xshape.push_back(outT);
  xshape.push_back(lfr_dim);
  *x = sonnx::Tensor::Float(xshape);
  for (int32_t i = 0; i < outT; ++i) {
    const float *src = &feats[static_cast<size_t>(i * sh) * dim];
    float *dst = x->pf() + static_cast<size_t>(i) * lfr_dim;
    for (int32_t j = 0; j < lfr_dim; ++j)
      dst[j] = (src[j] + r->neg_mean[j]) * r->inv_stddev[j];
  }
  return outT;
}

static void DecodeSenseVoice(SherpaRecognizer *r,
                             const std::vector<float> &feats, int32_t T) {
  sonnx::Tensor x;
  int32_t outT = LfrCmvn(r, feats, T, &x);
  if (outT <= 0) return;

  std::vector<int64_t> one;
  one.push_back(1);
  sonnx::Tensor xlen = sonnx::Tensor::Int(one, sonnx::kInt32);
  xlen.i[0] = outT;
  sonnx::Tensor lang = sonnx::Tensor::Int(one, sonnx::kInt32);
  lang.i[0] = r->language;
  sonnx::Tensor tn = sonnx::Tensor::Int(one, sonnx::kInt32);
  tn.i[0] = r->use_itn ? r->with_itn : r->without_itn;

  // bind by input name (order-independent)
  std::vector<const sonnx::Tensor *> inputs;
  for (int i = 0; i < r->model.NumInputs(); ++i) {
    const char *name = r->model.InputName(i);
    if (strcmp(name, "x_length") == 0)
      inputs.push_back(&xlen);
    else if (strcmp(name, "language") == 0)
      inputs.push_back(&lang);
    else if (strcmp(name, "text_norm") == 0)
      inputs.push_back(&tn);
    else
      inputs.push_back(&x);
  }

  std::vector<sonnx::Tensor> outputs;
  if (!r->model.Run(inputs, &outputs) || outputs.empty()) {
    fprintf(stderr, "sherpa: model run failed\n");
    return;
  }

  const sonnx::Tensor &logits = outputs[0];
  if (logits.dtype != sonnx::kFloat || logits.shape.size() < 2) {
    fprintf(stderr, "sherpa: unexpected model output\n");
    return;
  }
  size_t rk = logits.shape.size();
  int64_t t_out = logits.shape[rk - 2];
  int64_t c_out = logits.shape[rk - 1];

  // greedy CTC: argmax per frame, collapse repeats, drop blank
  std::vector<int64_t> ids;
  int64_t prev = -1;
  for (int64_t t = 0; t < t_out; ++t) {
    const float *row = logits.pf() + t * c_out;
    int64_t best = 0;
    float bv = row[0];
    for (int64_t c = 1; c < c_out; ++c) {
      if (row[c] > bv) {
        bv = row[c];
        best = c;
      }
    }
    if (best != r->blank_id && best != prev) ids.push_back(best);
    prev = best;
  }

  // first 4 tokens are meta (language/emotion/event/itn); skip them
  std::string text;
  for (size_t i = 4; i < ids.size(); ++i) {
    if (ids[i] >= 0 && ids[i] < static_cast<int64_t>(r->tokens.size()))
      text += r->tokens[ids[i]];
  }
  ReplaceWordMarkers(&text);

  r->result = text;
}

static void DecodeParaformer(SherpaRecognizer *r,
                             const std::vector<float> &feats, int32_t T) {
  sonnx::Tensor x;
  int32_t outT = LfrCmvn(r, feats, T, &x);
  if (outT <= 0) return;

  std::vector<int64_t> one;
  one.push_back(1);
  sonnx::Tensor xlen = sonnx::Tensor::Int(one, sonnx::kInt32);
  xlen.i[0] = outT;

  // inputs: speech [1,T,560] f32 + speech_lengths [1] i32
  std::vector<const sonnx::Tensor *> inputs;
  for (int i = 0; i < r->model.NumInputs(); ++i) {
    const char *name = r->model.InputName(i);
    inputs.push_back(strcmp(name, "speech_lengths") == 0 ? &xlen : &x);
  }

  std::vector<sonnx::Tensor> outputs;
  if (!r->model.Run(inputs, &outputs) || outputs.empty()) {
    fprintf(stderr, "sherpa: model run failed\n");
    return;
  }

  const sonnx::Tensor &logits = outputs[0];
  if (logits.dtype != sonnx::kFloat || logits.shape.size() < 2) {
    fprintf(stderr, "sherpa: unexpected model output\n");
    return;
  }
  size_t rk = logits.shape.size();
  int64_t n_out = logits.shape[rk - 2];
  int64_t vocab = logits.shape[rk - 1];
  // second output (token_num) bounds the valid positions
  if (outputs.size() > 1 && outputs[1].dtype != sonnx::kFloat &&
      outputs[1].Numel() >= 1) {
    int64_t tn = outputs[1].AtInt(0);
    if (tn >= 0 && tn < n_out) n_out = tn;
  }

  // per-position argmax until </s>; join "@@" BPE continuations
  std::string text;
  bool glue = false;  // previous token ended with "@@"
  for (int64_t t = 0; t < n_out; ++t) {
    const float *row = logits.pf() + t * vocab;
    int64_t best = 0;
    float bv = row[0];
    for (int64_t c = 1; c < vocab; ++c) {
      if (row[c] > bv) {
        bv = row[c];
        best = c;
      }
    }
    if (best == r->eos_id) break;
    if (best == r->blank_id || best >= static_cast<int64_t>(r->tokens.size()))
      continue;

    std::string tok = r->tokens[best];
    bool cont = tok.size() > 2 && tok.compare(tok.size() - 2, 2, "@@") == 0;
    if (cont) tok.erase(tok.size() - 2);
    bool ascii = !tok.empty();
    for (size_t k = 0; k < tok.size(); ++k) {
      if (static_cast<unsigned char>(tok[k]) >= 0x80) {
        ascii = false;
        break;
      }
    }
    // space before a fresh English piece ("hello world", "\u4f60\u597d ok")
    if (ascii && !glue && !text.empty() && text[text.size() - 1] != ' ')
      text += ' ';
    text += tok;
    glue = cont;
  }

  r->result = text;
}

static void DecodeNemo(SherpaRecognizer *r, std::vector<float> *feats,
                       int32_t T) {
  const int32_t dim = 80;

  // NeMo per_feature normalization: per-dim mean/stddev over frames
  for (int32_t j = 0; j < dim; ++j) {
    double sum = 0.0, sq = 0.0;
    for (int32_t t = 0; t < T; ++t) {
      double v = (*feats)[static_cast<size_t>(t) * dim + j];
      sum += v;
      sq += v * v;
    }
    double mean = sum / T;
    double var = sq / T - mean * mean;
    if (var < 0.0) var = 0.0;
    float inv = 1.0f / (sqrtf(static_cast<float>(var)) + 1e-5f);
    for (int32_t t = 0; t < T; ++t) {
      float *v = &(*feats)[static_cast<size_t>(t) * dim + j];
      *v = (*v - static_cast<float>(mean)) * inv;
    }
  }

  // transpose to audio_signal [1, 80, T]
  std::vector<int64_t> xshape;
  xshape.push_back(1);
  xshape.push_back(dim);
  xshape.push_back(T);
  sonnx::Tensor x = sonnx::Tensor::Float(xshape);
  for (int32_t t = 0; t < T; ++t) {
    for (int32_t j = 0; j < dim; ++j)
      x.pf()[static_cast<size_t>(j) * T + t] =
          (*feats)[static_cast<size_t>(t) * dim + j];
  }

  std::vector<int64_t> one;
  one.push_back(1);
  sonnx::Tensor len = sonnx::Tensor::Int(one, sonnx::kInt64);
  len.i[0] = T;

  std::vector<const sonnx::Tensor *> inputs;
  for (int i = 0; i < r->model.NumInputs(); ++i) {
    const char *name = r->model.InputName(i);
    inputs.push_back(strcmp(name, "length") == 0 ? &len : &x);
  }

  std::vector<sonnx::Tensor> outputs;
  if (!r->model.Run(inputs, &outputs) || outputs.empty()) {
    fprintf(stderr, "sherpa: model run failed\n");
    return;
  }

  const sonnx::Tensor &logits = outputs[0];
  if (logits.dtype != sonnx::kFloat || logits.shape.size() < 2) {
    fprintf(stderr, "sherpa: unexpected model output\n");
    return;
  }
  size_t rk = logits.shape.size();
  int64_t t_out = logits.shape[rk - 2];
  int64_t vocab = logits.shape[rk - 1];
  int64_t blank = vocab - 1;  // NeMo CTC: blank is the last entry

#ifdef SHERPA_DEBUG_NEMO
  fprintf(stderr, "nemo dbg: T=%d t_out=%lld vocab=%lld\nargmax:", (int)T,
          (long long)t_out, (long long)vocab);
  for (int64_t t = 0; t < t_out; ++t) {
    const float *row = logits.pf() + t * vocab;
    int64_t b0 = 0;
    float v0 = row[0];
    for (int64_t c = 1; c < vocab; ++c) {
      if (row[c] > v0) {
        v0 = row[c];
        b0 = c;
      }
    }
    fprintf(stderr, " %lld", (long long)b0);
  }
  fprintf(stderr, "\n");
#endif

  // greedy CTC: argmax per frame, collapse repeats, drop blank
  std::string text;
  int64_t prev = -1;
  for (int64_t t = 0; t < t_out; ++t) {
    const float *row = logits.pf() + t * vocab;
    int64_t best = 0;
    float bv = row[0];
    for (int64_t c = 1; c < vocab; ++c) {
      if (row[c] > bv) {
        bv = row[c];
        best = c;
      }
    }
    if (best != blank && best != prev &&
        best < static_cast<int64_t>(r->tokens.size()))
      text += r->tokens[best];
    prev = best;
  }
  ReplaceWordMarkers(&text);

  r->result = text;
}

static void DecodeOnlineCtc(SherpaRecognizer *r,
                            const std::vector<float> &feats, int32_t T) {
  std::vector<int64_t> xshape;
  xshape.push_back(1);
  xshape.push_back(T);
  xshape.push_back(80);
  sonnx::Tensor x = sonnx::Tensor::Float(xshape);
  memcpy(x.pf(), vdata(feats), static_cast<size_t>(T) * 80 * sizeof(float));

  std::vector<int64_t> one;
  one.push_back(1);
  sonnx::Tensor xlen = sonnx::Tensor::Int(one, sonnx::kInt32);
  xlen.i[0] = T;

  std::vector<const sonnx::Tensor *> inputs;
  for (int i = 0; i < r->model.NumInputs(); ++i) {
    const char *name = r->model.InputName(i);
    if (strcmp(name, "x_length") == 0 || strcmp(name, "speech_lengths") == 0 ||
        strcmp(name, "feats_length") == 0 || strcmp(name, "length") == 0)
      inputs.push_back(&xlen);
    else
      inputs.push_back(&x);
  }

  std::vector<sonnx::Tensor> outputs;
  if (!r->model.Run(inputs, &outputs) || outputs.empty()) {
    fprintf(stderr, "sherpa: online ctc model run failed\n");
    return;
  }

  const sonnx::Tensor &logits = outputs[0];
  if (logits.dtype != sonnx::kFloat || logits.shape.size() < 2) {
    fprintf(stderr, "sherpa: unexpected online ctc output\n");
    return;
  }

  size_t rk = logits.shape.size();
  int64_t t_out = logits.shape[rk - 2];
  int64_t vocab = logits.shape[rk - 1];

  std::string text;
  int64_t prev = -1;
  for (int64_t t = 0; t < t_out; ++t) {
    const float *row = logits.pf() + t * vocab;
    int64_t best = 0;
    float bv = row[0];
    for (int64_t c = 1; c < vocab; ++c) {
      if (row[c] > bv) {
        bv = row[c];
        best = c;
      }
    }
    if (best != r->blank_id && best != prev &&
        best < static_cast<int64_t>(r->tokens.size())) {
      text += r->tokens[best];
    }
    prev = best;
  }

  ReplaceWordMarkers(&text);
  r->result = text;
}

static bool RunTransducerChunk(SherpaRecognizer *r, SherpaStream *s,
                               const float *chunk, int32_t chunk_frames) {
  s->phase_id = kPhaseEnc;
  s->sub_num = 0;
  s->sub_den = 0;
  std::vector<int64_t> xshape;
  xshape.push_back(1);
  xshape.push_back(chunk_frames);
  xshape.push_back(80);
  sonnx::Tensor x = sonnx::Tensor::Float(xshape);
  memcpy(x.pf(), chunk,
         static_cast<size_t>(chunk_frames) * 80 * sizeof(float));
  if (TransducerDbg() && s->chunk_cursor == 0)
    DumpBin("sonnx_feat0.bin", x.pf(), x.Numel());

  std::vector<const sonnx::Tensor *> inputs;
  inputs.push_back(&x);
  for (size_t i = 0; i < s->encoder_states.size(); ++i)
    inputs.push_back(&s->encoder_states[i]);

  std::vector<sonnx::Tensor> outputs;
  if (!r->model.Run(inputs, &outputs) || outputs.size() < 1 + s->encoder_states.size()) {
    fprintf(stderr, "sherpa: transducer encoder run failed\n");
    return false;
  }

  const sonnx::Tensor &encoder_out = outputs[0];
  if (encoder_out.dtype != sonnx::kFloat || encoder_out.shape.size() < 2) {
    fprintf(stderr, "sherpa: unexpected transducer encoder output\n");
    return false;
  }

  int64_t num_frames = encoder_out.shape[encoder_out.shape.size() - 2];
  int64_t dim = encoder_out.shape[encoder_out.shape.size() - 1];
  s->sub_den = static_cast<int32_t>(num_frames);
  s->phase_id = kPhaseSearch;
  size_t hyp_before = s->hyp.size();
  if (TransducerDbg() && s->chunk_cursor == 0)
    DumpBin("sonnx_enc0.bin", encoder_out.pf(), encoder_out.Numel());
  if (TransducerDbg()) {
    double mn = 1e30, mx = -1e30, sum = 0;
    for (int64_t i = 0; i < encoder_out.Numel(); ++i) {
      double v = encoder_out.pf()[i];
      if (v < mn) mn = v;
      if (v > mx) mx = v;
      sum += v;
    }
    fprintf(stderr,
            "sherpa dbg: chunk in=%d out=%lld dim=%lld enc[min=%.3f max=%.3f "
            "mean=%.3f]\n",
            (int)chunk_frames, (long long)num_frames, (long long)dim, mn, mx,
            sum / (encoder_out.Numel() > 0 ? encoder_out.Numel() : 1));
  }
  for (int64_t k = 0; k < num_frames; ++k) {
    std::vector<int64_t> eshape;
    eshape.push_back(1);
    eshape.push_back(1);
    eshape.push_back(dim);
    sonnx::Tensor cur = sonnx::Tensor::Float(eshape);
    memcpy(cur.pf(), encoder_out.pf() + k * dim,
           static_cast<size_t>(dim) * sizeof(float));

    /* greedy search: keep emitting from this frame until the joiner
       predicts blank; a frame can hold several BPE pieces */
    for (int step = 0; step < 64; ++step) {
      std::vector<const sonnx::Tensor *> jin;
      jin.push_back(&cur);
      jin.push_back(&s->decoder_out);
      std::vector<sonnx::Tensor> jout;
      if (!r->joiner.Run(jin, &jout) || jout.empty()) {
        fprintf(stderr, "sherpa: transducer joiner run failed\n");
        return false;
      }

      const sonnx::Tensor &logit = jout[0];
      if (logit.dtype != sonnx::kFloat || logit.shape.empty()) {
        fprintf(stderr, "sherpa: unexpected transducer joiner output\n");
        return false;
      }

      int64_t vocab = logit.shape[logit.shape.size() - 1];
      if (TransducerDbg() && s->chunk_cursor == 0 && k == 0)
        DumpBin("sonnx_logit0.bin", logit.pf() + (logit.Numel() - vocab), vocab);
      const float *row = logit.pf() + (logit.Numel() - vocab);
      int64_t best = 0;
      float bv = row[0];
      for (int64_t i = 1; i < vocab; ++i) {
        if (row[i] > bv) {
          bv = row[i];
          best = i;
        }
      }

      if (best == r->blank_id)
        break;
      s->hyp.push_back(best);
      if (!RunTransducerDecoder(r, s->hyp, &s->decoder_out))
        return false;
    }
    s->sub_num = static_cast<int32_t>(k) + 1;
  }

  for (size_t i = 0; i < s->encoder_states.size(); ++i)
    s->encoder_states[i] = std::move(outputs[i + 1]);

  if (TransducerDbg() && s->hyp.size() > hyp_before) {
    fprintf(stderr, "sherpa dbg: emitted");
    for (size_t i = hyp_before; i < s->hyp.size(); ++i)
      fprintf(stderr, " %lld", (long long)s->hyp[i]);
    fprintf(stderr, "\n");
  }
  return true;
}

static void DecodeOnlineTransducer(SherpaRecognizer *r, SherpaStream *s,
                                   const std::vector<float> &feats,
                                   int32_t T, bool flush_all) {
  const int32_t dim = 80;
  /* streaming zipformer2 contract (sherpa-onnx
     online-zipformer2-transducer-model.h): ChunkSize() = T, so each
     encoder call consumes T feature frames starting at chunk_cursor and
     the window then advances by ChunkShift() = decode_chunk_len. The
     13-frame overlap between consecutive windows is intentional -- the
     model's internal mask arithmetic assumes exactly T inputs. */
  const int32_t shift = r->transducer_decode_chunk_len;
  const int32_t win = r->transducer_T;
  /* keep per-call work bounded even after input_finished: on slow
     hardware the tail backlog can hold many chunks, and an unbounded
     flush would block the caller for the whole time with no partial
     updates. The caller drains via SherpaIsStreamReady() instead. */
  int32_t budget = flush_all ? 0x7fffffff : SHERPA_STREAM_CHUNK_BUDGET;
  while (!s->decode_error && budget > 0 && win > 0 &&
         s->chunk_cursor + win < T) {
    if (!RunTransducerChunk(r, s, &feats[static_cast<size_t>(s->chunk_cursor) * dim],
                            win)) {
      /* without this the caller would retry the same chunk forever */
      s->decode_error = true;
      break;
    }
    s->chunk_cursor += shift;
    s->sub_num = 0;
    s->sub_den = 0;
    --budget;
  }
  s->phase_id = kPhaseIdle;

  BuildTransducerText(r, s);
}

static const char *DecodeCurrentStream(SherpaRecognizer *r, SherpaStream *s,
                                       bool finalize_input,
                                       bool reset_after) {
  if (!r || !s) return "";
  r->result.clear();

  if (finalize_input && !s->input_finished) {
    PadTransducerTail(r, s);
    s->fbank->InputFinished();
    s->input_finished = true;
  }
  int32_t T = s->fbank->NumFramesReady();
  const int32_t dim = 80;
  if (T <= 0) {
    s->last_decode_frames = T;
    if (reset_after) SherpaReset(r, s);
    return r->result.c_str();
  }

  // gather frames (T x 80)
  std::vector<float> feats(static_cast<size_t>(T) * dim);
  for (int32_t t = 0; t < T; ++t) {
    memcpy(&feats[static_cast<size_t>(t) * dim], s->fbank->GetFrame(t),
           static_cast<size_t>(dim) * sizeof(float));
  }
  s->last_decode_frames = T;

  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer) {
    DecodeOnlineTransducer(r, s, feats, T, finalize_input);
  } else {
    /* offline models decode the whole utterance in one graph run;
       g_run_progress tracks the node index inside that run */
    sonnx::g_run_progress = 0;
    s->phase_id = kPhaseModel;
    if (r->kind == SherpaRecognizer::kNemoCtc)
      DecodeNemo(r, &feats, T);
    else if (r->kind == SherpaRecognizer::kOnlineZipformer2Ctc ||
             r->kind == SherpaRecognizer::kOnlineWenetCtc)
      DecodeOnlineCtc(r, feats, T);
    else if (r->kind == SherpaRecognizer::kParaformer)
      DecodeParaformer(r, feats, T);
    else
      DecodeSenseVoice(r, feats, T);
    s->phase_id = kPhaseIdle;
  }

  if (reset_after) SherpaReset(r, s);
  return r->result.c_str();
}

const char *SherpaDecodeStream(SherpaRecognizer *r, SherpaStream *s) {
  return DecodeCurrentStream(r, s, false, false);
}

int32_t SherpaStreamHasError(SherpaRecognizer *r, SherpaStream *s) {
  return (r && s && s->decode_error) ? 1 : 0;
}

/* sub-progress of the chunk currently being decoded, 0..100: encoder
   graph run maps to 0..50 (top-level node index), greedy search to
   50..100 (encoder output frames consumed) */
static int32_t TransducerChunkSubPct(const SherpaStream *s) {
  int32_t phase = s->phase_id;
  if (phase == kPhaseEnc) {
    int32_t p = sonnx::g_run_progress;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p / 2;
  }
  if (phase == kPhaseSearch) {
    int32_t den = s->sub_den, num = s->sub_num;
    if (den <= 0) return 50;
    if (num < 0) num = 0;
    if (num > den) num = den;
    return 50 + (50 * num) / den;
  }
  return 0;
}

int32_t SherpaStreamProgress(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s) return 0;
  int32_t ready = s->fbank->NumFramesReady();
  if (ready <= 0) return 0;

  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer) {
    /* decodable extent: the cursor stops once cursor + T >= ready */
    int32_t total = ready - r->transducer_T;
    if (total <= 0) return 100;
    int32_t done = s->chunk_cursor;
    if (done < 0) done = 0;
    if (done > total) done = total;
    /* interpolate inside the in-flight chunk (it advances the cursor
       by `shift` frames once finished) for finer-grained progress */
    int64_t p = (100LL * done +
                 (int64_t)r->transducer_decode_chunk_len *
                     TransducerChunkSubPct(s)) / total;
    if (p > 100) p = 100;
    return static_cast<int32_t>(p);
  }

  /* offline: the whole utterance is one model run */
  if (s->phase_id == kPhaseModel) {
    int32_t p = sonnx::g_run_progress;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
  }
  int32_t done = s->last_decode_frames;
  if (done < 0) done = 0;
  if (done > ready) done = ready;
  return (done * 100) / ready;
}

void SherpaStreamGetInfo(SherpaRecognizer *r, SherpaStream *s,
                         SherpaStreamInfo *info) {
  if (!info) return;
  memset(info, 0, sizeof(*info));
  info->phase = "idle";
  if (!r || !s) return;

  int32_t phase = s->phase_id;
  info->phase = phase == kPhaseEnc ? "encoder"
                : phase == kPhaseSearch ? "search"
                : phase == kPhaseModel ? "model" : "idle";
  info->progress = SherpaStreamProgress(r, s);

  /* fbank frame shift is 10 ms */
  int32_t ready = s->fbank->NumFramesReady();
  if (ready < 0) ready = 0;
  if (r->kind == SherpaRecognizer::kOnlineZipformerTransducer) {
    int32_t shift = r->transducer_decode_chunk_len;
    int32_t total = ready - r->transducer_T;
    if (total < 0) total = 0;
    int32_t done = s->chunk_cursor;
    if (done < 0) done = 0;
    if (done > total) done = total;
    info->decoded_ms = done * 10;
    info->total_ms = total * 10;
    if (shift > 0) {
      info->chunks_total = (total + shift - 1) / shift;
      info->chunks_done = s->chunk_cursor / shift;
      if (info->chunks_done > info->chunks_total)
        info->chunks_done = info->chunks_total;
    }
    /* the hypothesis starts with context_size blanks, not real tokens */
    int32_t tok = static_cast<int32_t>(s->hyp.size()) -
                  r->transducer_context_size;
    info->tokens = tok > 0 ? tok : 0;
  } else {
    int32_t done = s->last_decode_frames;
    if (done < 0) done = 0;
    if (done > ready) done = ready;
    info->decoded_ms = done * 10;
    info->total_ms = ready * 10;
  }

  if (phase == kPhaseEnc || phase == kPhaseModel) {
    int32_t p = sonnx::g_run_progress;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    info->phase_pct = p;
  } else if (phase == kPhaseSearch) {
    int32_t den = s->sub_den, num = s->sub_num;
    if (num < 0) num = 0;
    if (den > 0 && num > den) num = den;
    info->phase_pct = den > 0 ? (100 * num) / den : 0;
  }
}

const char *SherpaGetResult(SherpaRecognizer *r) {
  return r ? r->result.c_str() : "";
}

const char *SherpaDecode(SherpaRecognizer *r, SherpaStream *s) {
  return DecodeCurrentStream(r, s, true, true);
}
