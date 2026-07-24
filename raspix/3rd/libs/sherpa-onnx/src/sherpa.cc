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

int32_t MetaInt(const sonnx::Model &m, const char *key, int32_t dflt) {
  std::string v = m.Metadata(key);
  return v.empty() ? dflt : atoi(v.c_str());
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
  enum Kind { kSenseVoice = 0, kParaformer = 1, kNemoCtc = 2 };

  sonnx::Model model;
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
  std::string result;
};

struct SherpaStream {
  knf::OnlineFbank *fbank = NULL;
  LinearResampler *resampler = NULL;  // NULL when input is already 16 kHz
  int32_t in_rate = 16000;
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
  return !tokens->empty();
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

  // pipeline is picked from the model_type metadata
  std::string mt = r->model.Metadata("model_type");
  if (mt == "paraformer")
    r->kind = SherpaRecognizer::kParaformer;
  else if (mt == "EncDecCTCModelBPE")
    r->kind = SherpaRecognizer::kNemoCtc;
  else
    r->kind = SherpaRecognizer::kSenseVoice;

  if (r->kind == SherpaRecognizer::kNemoCtc) {
    r->fbank_opts = MakeNemoFbankOpts();
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
  return r;
}

void SherpaDestroyRecognizer(SherpaRecognizer *r) { delete r; }

int32_t SherpaLoadProgress(void) { return sonnx::g_load_progress; }

void SherpaSetLanguage(SherpaRecognizer *r, int32_t lang_id) {
  if (r) r->language = lang_id;
}

void SherpaSetUseItn(SherpaRecognizer *r, int32_t use_itn) {
  if (r) r->use_itn = use_itn ? 1 : 0;
}

SherpaStream *SherpaCreateStream(SherpaRecognizer *r, int32_t input_rate) {
  if (!r) return NULL;
  SherpaStream *s = new SherpaStream();
  s->fbank = new knf::OnlineFbank(r->fbank_opts);
  if (input_rate > 0 && input_rate != 16000) {
    s->resampler = new LinearResampler(input_rate, 16000);
    s->in_rate = input_rate;
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
}

void SherpaAcceptWaveform(SherpaRecognizer *r, SherpaStream *s,
                          const int16_t *samples, int32_t n) {
  if (!r || !s || !samples || n <= 0) return;

  // SenseVoice/paraformer keep the raw int16 scale (normalize_samples=0);
  // NeMo expects samples in [-1, 1]
  float scale =
      r->kind == SherpaRecognizer::kNemoCtc ? 1.0f / 32768.0f : 1.0f;
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

const char *SherpaDecode(SherpaRecognizer *r, SherpaStream *s) {
  if (!r || !s) return "";
  r->result.clear();

  s->fbank->InputFinished();
  int32_t T = s->fbank->NumFramesReady();
  const int32_t dim = 80;
  if (T <= 0) {
    SherpaReset(r, s);
    return r->result.c_str();
  }

  // gather frames (T x 80)
  std::vector<float> feats(static_cast<size_t>(T) * dim);
  for (int32_t t = 0; t < T; ++t) {
    memcpy(&feats[static_cast<size_t>(t) * dim], s->fbank->GetFrame(t),
           static_cast<size_t>(dim) * sizeof(float));
  }
  SherpaReset(r, s);

  if (r->kind == SherpaRecognizer::kNemoCtc)
    DecodeNemo(r, &feats, T);
  else if (r->kind == SherpaRecognizer::kParaformer)
    DecodeParaformer(r, feats, T);
  else
    DecodeSenseVoice(r, feats, T);
  return r->result.c_str();
}
