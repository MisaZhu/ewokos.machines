/**
 * fbank.h - kaldi-compatible fbank feature extraction (merged single header).
 *
 * Ported from kaldi-native-fbank (https://github.com/csukuangfj/kaldi-native-fbank)
 * which is copied/modified from kaldi (Apache License 2.0).
 * Original copyright: Xiaomi Corporation (authors: Fangjun Kuang) and
 * kaldi authors. See NOTICE in this directory.
 *
 * This is the feature-extraction frontend used by sherpa-onnx, condensed
 * into one header for EwokOS (aarch64 / arm).
 */
#ifndef SHERPA_ONNX_PORT_FBANK_H_
#define SHERPA_ONNX_PORT_FBANK_H_

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

#ifndef M_2PI
#define M_2PI 6.283185307179586476925286766559005
#endif

/* minimal replacements for knf log.h (no exceptions on EwokOS) */
#define KNF_CHECK(x)                                                        \
  do {                                                                      \
    if (!(x)) {                                                             \
      fprintf(stderr, "KNF_CHECK failed at %s:%d: %s\n", __FILE__,          \
              __LINE__, #x);                                                \
      exit(-1);                                                             \
    }                                                                       \
  } while (0)

#define KNF_CHECK_OP(x, y, op)                                              \
  do {                                                                      \
    if (!((x)op(y))) {                                                      \
      fprintf(stderr, "KNF_CHECK failed at %s:%d: %s %s %s (%lld vs %lld)\n", \
              __FILE__, __LINE__, #x, #op, #y, (long long)(x),              \
              (long long)(y));                                              \
      exit(-1);                                                             \
    }                                                                       \
  } while (0)

#define KNF_CHECK_EQ(x, y) KNF_CHECK_OP(x, y, ==)
#define KNF_CHECK_NE(x, y) KNF_CHECK_OP(x, y, !=)
#define KNF_CHECK_LT(x, y) KNF_CHECK_OP(x, y, <)
#define KNF_CHECK_LE(x, y) KNF_CHECK_OP(x, y, <=)
#define KNF_CHECK_GT(x, y) KNF_CHECK_OP(x, y, >)
#define KNF_CHECK_GE(x, y) KNF_CHECK_OP(x, y, >=)

#define KNF_FATAL(msg)                                                     \
  do {                                                                      \
    fprintf(stderr, "fatal at %s:%d: %s\n", __FILE__, __LINE__, msg);       \
    exit(-1);                                                               \
  } while (0)

namespace knf {

inline int32_t RoundUpToNearestPowerOfTwo(int32_t n) {
  KNF_CHECK_GT(n, 0);
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

struct FrameExtractionOptions {
  float samp_freq = 16000;
  float frame_shift_ms = 10.0f;
  float frame_length_ms = 25.0f;
  float dither = 0.0f;  // sherpa-onnx uses 0 here
  float preemph_coeff = 0.97f;
  bool remove_dc_offset = true;
  std::string window_type = "povey";
  bool round_to_power_of_two = true;
  float blackman_coeff = 0.42f;
  bool snip_edges = true;

  int32_t WindowShift() const {
    return static_cast<int32_t>(samp_freq * 0.001f * frame_shift_ms);
  }
  int32_t WindowSize() const {
    return static_cast<int32_t>(samp_freq * 0.001f * frame_length_ms);
  }
  int32_t PaddedWindowSize() const {
    return (round_to_power_of_two ? RoundUpToNearestPowerOfTwo(WindowSize())
                                  : WindowSize());
  }
};

std::vector<float> GetWindow(const std::string &window_type,
                             int32_t window_size, float blackman_coeff = 0.42);

class FeatureWindowFunction {
 public:
  FeatureWindowFunction() = default;
  explicit FeatureWindowFunction(const FrameExtractionOptions &opts);
  FeatureWindowFunction(const std::string &window_type, int32_t window_size,
                        float blackman_coeff = 0.42);
  explicit FeatureWindowFunction(const std::vector<float> &window);

  void Apply(float *wave) const;
  const std::vector<float> &GetWindow() const { return window_; }

 private:
  std::vector<float> window_;
};

int64_t FirstSampleOfFrame(int32_t frame, const FrameExtractionOptions &opts);

int32_t NumFrames(int64_t num_samples, const FrameExtractionOptions &opts,
                  bool flush = true);

void ExtractWindow(int64_t sample_offset, const std::vector<float> &wave,
                   int32_t f, const FrameExtractionOptions &opts,
                   const FeatureWindowFunction &window_function,
                   std::vector<float> *window,
                   float *log_energy_pre_window = nullptr);

void ProcessWindow(const FrameExtractionOptions &opts,
                   const FeatureWindowFunction &window_function, float *window,
                   float *log_energy_pre_window = nullptr);

float InnerProduct(const float *a, const float *b, int32_t n);

/* ----------------------------- mel banks ----------------------------- */

struct MelBanksOptions {
  int32_t num_bins = 25;
  float low_freq = 20;
  float high_freq = 0;  // 0 -> no cutoff, negative -> added to Nyquist
  float vtln_low = 100;
  float vtln_high = -500;
  bool debug_mel = false;
  bool htk_mode = false;
  // librosa-compatible mel banks (Slaney mel scale + Slaney area norm),
  // used by NeMo models
  bool is_librosa = false;
};

class MelBanks {
 public:
  static inline float InverseMelScale(float mel_freq) {
    return 700.0f * (expf(mel_freq / 1127.0f) - 1.0f);
  }

  static inline float MelScale(float freq) {
    return 1127.0f * logf(1.0f + freq / 700.0f);
  }

  static float VtlnWarpFreq(float vtln_low_cutoff, float vtln_high_cutoff,
                            float low_freq, float high_freq,
                            float vtln_warp_factor, float freq);

  static float VtlnWarpMelFreq(float vtln_low_cutoff, float vtln_high_cutoff,
                               float low_freq, float high_freq,
                               float vtln_warp_factor, float mel_freq);

  MelBanks(const MelBanksOptions &opts, const FrameExtractionOptions &frame_opts,
           float vtln_warp_factor);

  MelBanks(const float *weights, int32_t num_rows, int32_t num_cols);

  void Compute(const float *fft_energies, float *mel_energies_out) const;

  int32_t NumBins() const { return static_cast<int32_t>(bins_.size()); }

 private:
  std::vector<std::pair<int32_t, std::vector<float>>> bins_;
  bool debug_ = false;
  bool htk_mode_ = false;
  int32_t num_fft_bins_ = -1;
};

/* ----------------------------- fbank computer ----------------------------- */

// n-point real FFT, n is even (always a power of two here since
// FrameExtractionOptions::round_to_power_of_two is true).
// Output packing follows kaldi convention:
//   out[0] = R[0], out[1] = R[n/2], out[2k] = R[k], out[2k+1] = I[k]
class Rfft {
 public:
  explicit Rfft(int32_t n, bool inverse = false);
  ~Rfft();

  void Compute(float *in_out);

 private:
  int32_t n_;
  bool inverse_ = false;
  std::vector<float> table_;  // cos/sin twiddles
  std::vector<int32_t> rev_;  // bit-reversal permutation
  std::vector<float> buf_;    // scratch, 2*n interleaved re/im
};

void ComputePowerSpectrum(std::vector<float> *complex_fft);
void Sqrt(float *in_out, int32_t n);
void Dither(float *d, int32_t n, float dither_value);

struct FbankOptions {
  FrameExtractionOptions frame_opts;
  MelBanksOptions mel_opts;
  bool use_energy = false;
  float energy_floor = 0.0f;
  bool raw_energy = true;
  bool htk_compat = false;
  bool use_log_fbank = true;
  bool use_power = true;

  FbankOptions() { mel_opts.num_bins = 23; }
};

class FbankComputer {
 public:
  using Options = FbankOptions;

  explicit FbankComputer(const FbankOptions &opts);
  ~FbankComputer();

  int32_t Dim() const {
    return opts_.mel_opts.num_bins + (opts_.use_energy ? 1 : 0);
  }

  bool NeedRawLogEnergy() const { return opts_.use_energy && opts_.raw_energy; }

  const FrameExtractionOptions &GetFrameOptions() const {
    return opts_.frame_opts;
  }

  void Compute(float signal_raw_log_energy, float vtln_warp,
               std::vector<float> *signal_frame, float *feature);

 private:
  const MelBanks *GetMelBanks(float vtln_warp);

  FbankOptions opts_;
  float log_energy_floor_ = 0.0f;
  std::map<float, MelBanks *> mel_banks_;
  Rfft rfft_;
};

/* ----------------------------- online feature ----------------------------- */

class RecyclingVector {
 public:
  explicit RecyclingVector(int32_t items_to_hold = -1);

  const float *At(int32_t index) const;
  void PushBack(std::vector<float> item);
  int32_t Size() const;
  void Pop(int32_t n);

 private:
  std::deque<std::vector<float>> items_;
  int32_t items_to_hold_;
  int32_t first_available_index_;
};

template <class C>
class OnlineGenericBaseFeature {
 public:
  explicit OnlineGenericBaseFeature(const typename C::Options &opts);

  int32_t Dim() const { return computer_.Dim(); }

  float FrameShiftInSeconds() const {
    return computer_.GetFrameOptions().frame_shift_ms / 1000.0f;
  }

  int32_t NumFramesReady() const { return features_.Size(); }

  bool IsLastFrame(int32_t frame) const {
    return input_finished_ && frame == NumFramesReady() - 1;
  }

  const float *GetFrame(int32_t frame) const { return features_.At(frame); }

  void AcceptWaveform(float sampling_rate, const float *waveform, int32_t n);

  void InputFinished();

  void Pop(int32_t n) { features_.Pop(n); }

 private:
  void ComputeFeatures();

  C computer_;
  FeatureWindowFunction window_function_;
  RecyclingVector features_;
  bool input_finished_;
  int64_t waveform_offset_;
  std::vector<float> waveform_remainder_;
};

using OnlineFbank = OnlineGenericBaseFeature<FbankComputer>;

}  // namespace knf

#endif  // SHERPA_ONNX_PORT_FBANK_H_
