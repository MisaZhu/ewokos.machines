/**
 * fbank.cc - kaldi-compatible fbank feature extraction (merged implementation).
 *
 * Ported from kaldi-native-fbank (https://github.com/csukuangfj/kaldi-native-fbank)
 * which is copied/modified from kaldi (Apache License 2.0).
 * Original copyright: Xiaomi Corporation (authors: Fangjun Kuang) and
 * kaldi authors. See NOTICE in this directory.
 *
 * The Rfft here is a compact radix-2 FFT written for this port (window sizes
 * are always powers of two), replacing the kissfft dependency.
 */
#include "fbank.h"

#include <algorithm>
#include <limits>
#include <string.h>

#include "vecutil.h"

namespace knf {

/* ------------------------- windowing ------------------------- */

std::vector<float> GetWindow(const std::string &window_type,
                             int32_t window_size,
                             float blackman_coeff /*= 0.42*/) {
  std::vector<float> window(window_size);
  int32_t frame_length = window_size;
  KNF_CHECK_GT(frame_length, 0);

  float *window_data = vdata(window);

  double a = M_2PI / (frame_length - 1);
  if (window_type == "hann") {
    // periodic hann, see https://pytorch.org/docs/stable/generated/torch.hann_window.html
    a = M_2PI / frame_length;
  }

  for (int32_t i = 0; i < frame_length; i++) {
    double i_fl = static_cast<double>(i);
    if (window_type == "hanning") {
      window_data[i] = 0.5 - 0.5 * cos(a * i_fl);
    } else if (window_type == "sine") {
      window_data[i] = sin(0.5 * a * i_fl);
    } else if (window_type == "hamming") {
      window_data[i] = 0.54 - 0.46 * cos(a * i_fl);
    } else if (window_type == "hann") {
      window_data[i] = 0.50 - 0.50 * cos(a * i_fl);
    } else if (window_type == "povey") {
      // like hamming but goes to zero at edges.
      window_data[i] = pow(0.5 - 0.5 * cos(a * i_fl), 0.85);
    } else if (window_type == "rectangular") {
      window_data[i] = 1.0;
    } else if (window_type == "blackman") {
      window_data[i] = blackman_coeff - 0.5 * cos(a * i_fl) +
                       (0.5 - blackman_coeff) * cos(2 * a * i_fl);
    } else {
      KNF_FATAL("invalid window type");
    }
  }

  return window;
}

FeatureWindowFunction::FeatureWindowFunction(const FrameExtractionOptions &opts)
    : FeatureWindowFunction(opts.window_type, opts.WindowSize(),
                            opts.blackman_coeff) {}

FeatureWindowFunction::FeatureWindowFunction(const std::string &window_type,
                                             int32_t window_size,
                                             float blackman_coeff /*= 0.42*/)
    : window_(knf::GetWindow(window_type, window_size, blackman_coeff)) {}

FeatureWindowFunction::FeatureWindowFunction(const std::vector<float> &window)
    : window_(window) {}

void FeatureWindowFunction::Apply(float *wave) const {
  int32_t window_size = static_cast<int32_t>(window_.size());
  const float *p = vdata(window_);
  for (int32_t k = 0; k != window_size; ++k) {
    wave[k] *= p[k];
  }
}

int64_t FirstSampleOfFrame(int32_t frame, const FrameExtractionOptions &opts) {
  int64_t frame_shift = opts.WindowShift();
  if (opts.snip_edges) {
    return frame * frame_shift;
  } else {
    int64_t midpoint_of_frame = frame_shift * frame + frame_shift / 2,
            beginning_of_frame = midpoint_of_frame - opts.WindowSize() / 2;
    return beginning_of_frame;
  }
}

int32_t NumFrames(int64_t num_samples, const FrameExtractionOptions &opts,
                  bool flush /*= true*/) {
  int64_t frame_shift = opts.WindowShift();
  int64_t frame_length = opts.WindowSize();
  if (opts.snip_edges) {
    if (num_samples < frame_length)
      return 0;
    else
      return (1 + ((num_samples - frame_length) / frame_shift));
  } else {
    int32_t num_frames =
        static_cast<int32_t>((num_samples + (frame_shift / 2)) / frame_shift);

    if (flush) return num_frames;

    int64_t end_sample_of_last_frame =
        FirstSampleOfFrame(num_frames - 1, opts) + frame_length;

    while (num_frames > 0 && end_sample_of_last_frame > num_samples) {
      num_frames--;
      end_sample_of_last_frame -= frame_shift;
    }
    return num_frames;
  }
}

void ExtractWindow(int64_t sample_offset, const std::vector<float> &wave,
                   int32_t f, const FrameExtractionOptions &opts,
                   const FeatureWindowFunction &window_function,
                   std::vector<float> *window,
                   float *log_energy_pre_window /*= nullptr*/) {
  KNF_CHECK(sample_offset >= 0 && wave.size() != 0);

  int32_t frame_length = opts.WindowSize();
  int32_t frame_length_padded = opts.PaddedWindowSize();

  int64_t num_samples = sample_offset + static_cast<int64_t>(wave.size());
  int64_t start_sample = FirstSampleOfFrame(f, opts);
  int64_t end_sample = start_sample + frame_length;

  if (opts.snip_edges) {
    KNF_CHECK(start_sample >= sample_offset && end_sample <= num_samples);
  } else {
    KNF_CHECK(sample_offset == 0 || start_sample >= sample_offset);
  }

  if (window->size() != static_cast<size_t>(frame_length_padded)) {
    window->resize(frame_length_padded);
  }

  int32_t wave_start = static_cast<int32_t>(start_sample - sample_offset);
  int32_t wave_end = wave_start + frame_length;

  if (wave_start >= 0 && wave_end <= static_cast<int32_t>(wave.size())) {
    // the normal case-- no edge effects to consider.
    std::copy(wave.begin() + wave_start,
              wave.begin() + wave_start + frame_length, vdata(*window));
  } else {
    // Deal with any end effects by reflection, if needed.
    int32_t wave_dim = static_cast<int32_t>(wave.size());
    for (int32_t s = 0; s < frame_length; ++s) {
      int32_t s_in_wave = s + wave_start;
      while (s_in_wave < 0 || s_in_wave >= wave_dim) {
        if (s_in_wave < 0)
          s_in_wave = -s_in_wave - 1;
        else
          s_in_wave = 2 * wave_dim - 1 - s_in_wave;
      }
      (*window)[s] = wave[s_in_wave];
    }
  }

  ProcessWindow(opts, window_function, vdata(*window), log_energy_pre_window);
}

static void RemoveDcOffset(float *d, int32_t n) {
  float sum = 0;
  for (int32_t i = 0; i != n; ++i) {
    sum += d[i];
  }

  float mean = sum / n;

  for (int32_t i = 0; i != n; ++i) {
    d[i] -= mean;
  }
}

float InnerProduct(const float *a, const float *b, int32_t n) {
  float sum = 0;
  for (int32_t i = 0; i != n; ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

void Dither(float *d, int32_t n, float dither_value) {
  if (dither_value == 0.0f) {
    return;
  }

  // Box-Muller gaussian dither (matches knf/kaldi semantics closely enough;
  // sherpa-onnx uses dither=0 by default anyway).
  for (int32_t i = 0; i < n; ++i) {
    float u1 = (static_cast<float>(rand()) + 1.0f) / (RAND_MAX + 2.0f);
    float u2 = (static_cast<float>(rand()) + 1.0f) / (RAND_MAX + 2.0f);
    d[i] += sqrtf(-2.0f * logf(u1)) * cosf(2.0f * static_cast<float>(M_PI) * u2) *
            dither_value;
  }
}

static void Preemphasize(float *d, int32_t n, float preemph_coeff) {
  if (preemph_coeff == 0.0f) {
    return;
  }

  KNF_CHECK(preemph_coeff >= 0.0f && preemph_coeff <= 1.0f);

  for (int32_t i = n - 1; i > 0; --i) {
    d[i] -= preemph_coeff * d[i - 1];
  }
  d[0] -= preemph_coeff * d[0];
}

void ProcessWindow(const FrameExtractionOptions &opts,
                   const FeatureWindowFunction &window_function, float *window,
                   float *log_energy_pre_window /*= nullptr*/) {
  int32_t frame_length = opts.WindowSize();

  if (opts.dither != 0.0f) {
    Dither(window, frame_length, opts.dither);
  }

  if (opts.remove_dc_offset) {
    RemoveDcOffset(window, frame_length);
  }

  if (log_energy_pre_window != NULL) {
    float energy = std::max<float>(InnerProduct(window, window, frame_length),
                                   std::numeric_limits<float>::epsilon());
    *log_energy_pre_window = logf(energy);
  }

  if (opts.preemph_coeff != 0.0f) {
    Preemphasize(window, frame_length, opts.preemph_coeff);
  }

  window_function.Apply(window);
}

/* ------------------------- mel banks ------------------------- */

float MelBanks::VtlnWarpFreq(float vtln_low_cutoff, float vtln_high_cutoff,
                             float low_freq, float high_freq,
                             float vtln_warp_factor, float freq) {
  if (freq < low_freq || freq > high_freq) return freq;

  KNF_CHECK_GT(vtln_low_cutoff, low_freq);
  KNF_CHECK_LT(vtln_high_cutoff, high_freq);

  float one = 1.0f;
  float l = vtln_low_cutoff * std::max(one, vtln_warp_factor);
  float h = vtln_high_cutoff * std::min(one, vtln_warp_factor);
  float scale = 1.0f / vtln_warp_factor;
  float Fl = scale * l;
  float Fh = scale * h;
  KNF_CHECK(l > low_freq && h < high_freq);
  float scale_left = (Fl - low_freq) / (l - low_freq);
  float scale_right = (high_freq - Fh) / (high_freq - h);

  if (freq < l) {
    return low_freq + scale_left * (freq - low_freq);
  } else if (freq < h) {
    return scale * freq;
  } else {  // freq >= h
    return high_freq + scale_right * (freq - high_freq);
  }
}

float MelBanks::VtlnWarpMelFreq(float vtln_low_cutoff, float vtln_high_cutoff,
                                float low_freq, float high_freq,
                                float vtln_warp_factor, float mel_freq) {
  return MelScale(VtlnWarpFreq(vtln_low_cutoff, vtln_high_cutoff, low_freq,
                               high_freq, vtln_warp_factor,
                               InverseMelScale(mel_freq)));
}

/* librosa (Slaney) mel scale, used when MelBanksOptions::is_librosa is set */
static inline float MelScaleSlaney(float freq) {
  if (freq <= 1000.0f) return freq * 3.0f / 200.0f;
  return 15.0f + 27.0f * logf(freq / 1000.0f) / logf(6.4f);
}

static inline float InverseMelScaleSlaney(float mel) {
  if (mel <= 15.0f) return mel * 200.0f / 3.0f;
  return 1000.0f * expf((mel - 15.0f) * logf(6.4f) / 27.0f);
}

MelBanks::MelBanks(const MelBanksOptions &opts,
                   const FrameExtractionOptions &frame_opts,
                   float vtln_warp_factor)
    : num_fft_bins_(frame_opts.PaddedWindowSize()) {
  htk_mode_ = opts.htk_mode;
  debug_ = opts.debug_mel;

  int32_t num_bins = opts.num_bins;
  if (num_bins < 3) {
    KNF_FATAL("must have at least 3 mel bins");
  }

  float sample_freq = frame_opts.samp_freq;
  int32_t window_length_padded = frame_opts.PaddedWindowSize();
  KNF_CHECK_EQ(window_length_padded % 2, 0);

  int32_t num_fft_bins = window_length_padded / 2;
  float nyquist = 0.5f * sample_freq;

  float low_freq = opts.low_freq, high_freq;
  if (opts.high_freq > 0.0f) {
    high_freq = opts.high_freq;
  } else {
    high_freq = nyquist + opts.high_freq;
  }

  if (low_freq < 0.0f || low_freq >= nyquist || high_freq <= 0.0f ||
      high_freq > nyquist || high_freq <= low_freq) {
    KNF_FATAL("bad low/high frequency in mel options");
  }

  float fft_bin_width = sample_freq / window_length_padded;

  if (opts.is_librosa) {
    // librosa.filters.mel: Slaney mel spacing, triangles computed in the
    // Hz domain, Slaney area normalization 2/(right_hz - left_hz)
    float mel_low = MelScaleSlaney(low_freq);
    float mel_high = MelScaleSlaney(high_freq);
    float mel_delta = (mel_high - mel_low) / (num_bins + 1);

    bins_.resize(num_bins);
    for (int32_t bin = 0; bin < num_bins; ++bin) {
      float left_hz = InverseMelScaleSlaney(mel_low + bin * mel_delta);
      float center_hz = InverseMelScaleSlaney(mel_low + (bin + 1) * mel_delta);
      float right_hz = InverseMelScaleSlaney(mel_low + (bin + 2) * mel_delta);

      std::vector<float> this_bin(num_fft_bins);
      int32_t first_index = -1, last_index = -1;
      for (int32_t i = 0; i < num_fft_bins; ++i) {
        float freq = fft_bin_width * i;
        if (freq > left_hz && freq < right_hz) {
          float weight;
          if (freq <= center_hz) {
            weight = (freq - left_hz) / (center_hz - left_hz);
          } else {
            weight = (right_hz - freq) / (right_hz - center_hz);
          }
          this_bin[i] = weight * 2.0f / (right_hz - left_hz);
          if (first_index == -1) first_index = i;
          last_index = i;
        }
      }
      KNF_CHECK(first_index != -1 && last_index >= first_index);

      bins_[bin].first = first_index;
      int32_t size = last_index + 1 - first_index;
      bins_[bin].second.insert(bins_[bin].second.end(),
                               this_bin.begin() + first_index,
                               this_bin.begin() + first_index + size);
    }
    return;
  }

  float mel_low_freq = MelScale(low_freq);
  float mel_high_freq = MelScale(high_freq);

  // divide by num_bins+1 in next line because of end-effects where the bins
  // spread out to the sides.
  float mel_freq_delta = (mel_high_freq - mel_low_freq) / (num_bins + 1);

  float vtln_low = opts.vtln_low, vtln_high = opts.vtln_high;
  if (vtln_high < 0.0f) {
    vtln_high += nyquist;
  }

  if (vtln_warp_factor != 1.0f &&
      (vtln_low < 0.0f || vtln_low <= low_freq || vtln_low >= high_freq ||
       vtln_high <= 0.0f || vtln_high >= high_freq || vtln_high <= vtln_low)) {
    KNF_FATAL("bad vtln values in mel options");
  }

  bins_.resize(num_bins);

  for (int32_t bin = 0; bin < num_bins; ++bin) {
    float left_mel = mel_low_freq + bin * mel_freq_delta,
          center_mel = mel_low_freq + (bin + 1) * mel_freq_delta,
          right_mel = mel_low_freq + (bin + 2) * mel_freq_delta;

    if (vtln_warp_factor != 1.0f) {
      left_mel = VtlnWarpMelFreq(vtln_low, vtln_high, low_freq, high_freq,
                                 vtln_warp_factor, left_mel);
      center_mel = VtlnWarpMelFreq(vtln_low, vtln_high, low_freq, high_freq,
                                   vtln_warp_factor, center_mel);
      right_mel = VtlnWarpMelFreq(vtln_low, vtln_high, low_freq, high_freq,
                                  vtln_warp_factor, right_mel);
    }

    std::vector<float> this_bin(num_fft_bins);

    int32_t first_index = -1, last_index = -1;
    for (int32_t i = 0; i < num_fft_bins; ++i) {
      float freq = (fft_bin_width * i);  // Center frequency of this fft bin.
      float mel = MelScale(freq);
      if (mel > left_mel && mel < right_mel) {
        float weight;
        if (mel <= center_mel) {
          weight = (mel - left_mel) / (center_mel - left_mel);
        } else {
          weight = (right_mel - mel) / (right_mel - center_mel);
        }
        this_bin[i] = weight;
        if (first_index == -1) {
          first_index = i;
        }
        last_index = i;
      }
    }
    KNF_CHECK(first_index != -1 && last_index >= first_index);

    bins_[bin].first = first_index;
    int32_t size = last_index + 1 - first_index;
    bins_[bin].second.insert(bins_[bin].second.end(),
                             this_bin.begin() + first_index,
                             this_bin.begin() + first_index + size);

    // Replicate a bug in HTK, for testing purposes.
    if (opts.htk_mode && bin == 0 && mel_low_freq != 0.0f) {
      bins_[bin].second[0] = 0.0f;
    }
  }
}

MelBanks::MelBanks(const float *weights, int32_t num_rows, int32_t num_cols)
    : debug_(false), htk_mode_(false), num_fft_bins_((num_cols - 1) * 2) {
  bins_.resize(num_rows);
  for (int32_t bin = 0; bin < num_rows; ++bin) {
    const float *this_bin = weights + bin * num_cols;

    int32_t first_index = -1, last_index = -1;

    for (int32_t i = 0; i < num_cols; ++i) {
      if (this_bin[i] == 0) {
        continue;
      }
      if (first_index == -1) first_index = i;
      last_index = i;
    }

    KNF_CHECK(first_index != -1 && last_index >= first_index);

    bins_[bin].first = first_index;
    int32_t size = last_index + 1 - first_index;

    for (int32_t i = 0; i < size; ++i) {
      bins_[bin].second.push_back(this_bin[first_index + i]);
    }
  }
}

// "power_spectrum" contains fft energies.
void MelBanks::Compute(const float *power_spectrum,
                       float *mel_energies_out) const {
  int32_t num_bins = static_cast<int32_t>(bins_.size());

  for (int32_t i = 0; i < num_bins; i++) {
    int32_t offset = bins_[i].first;
    const auto &v = bins_[i].second;
    float energy = 0;
    for (int32_t k = 0; k != static_cast<int32_t>(v.size()); ++k) {
      energy += v[k] * power_spectrum[k + offset];
    }

    // HTK-like flooring- for testing purposes (we prefer dither)
    if (htk_mode_ && energy < 1.0f) {
      energy = 1.0f;
    }

    mel_energies_out[i] = energy;

    KNF_CHECK_EQ(energy, energy);  // check that energy is not nan
  }
}

/* ------------------------- rfft (radix-2) ------------------------- */

Rfft::Rfft(int32_t n, bool inverse /*=false*/) : n_(n), inverse_(inverse) {
  if (n <= 0 || (n & (n - 1)) != 0) {
    KNF_FATAL("rfft size must be a power of two");
  }
  if (inverse_) {
    KNF_FATAL("inverse rfft is not supported in this port");
  }

  int32_t half = n / 2;
  table_.resize(n);  // cos/sin interleaved for k in [0, n/2)
  for (int32_t k = 0; k < half; ++k) {
    double ang = -2.0 * M_PI * k / n;
    table_[2 * k] = static_cast<float>(cos(ang));
    table_[2 * k + 1] = static_cast<float>(sin(ang));
  }

  int32_t bits = 0;
  while ((1 << bits) < n) ++bits;
  rev_.resize(n);
  for (int32_t i = 0; i < n; ++i) {
    int32_t r = 0;
    for (int32_t b = 0; b < bits; ++b) {
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    }
    rev_[i] = r;
  }

  buf_.resize(2 * n);
}

Rfft::~Rfft() {}

void Rfft::Compute(float *in_out) {
  int32_t n = n_;
  float *re = vdata(buf_);
  // de-interleave: real part into first half, imag into second half
  float *im = vdata(buf_) + n;

  for (int32_t i = 0; i < n; ++i) {
    re[i] = in_out[i];
    im[i] = 0.0f;
  }

  // bit-reversal permutation
  for (int32_t i = 0; i < n; ++i) {
    int32_t j = rev_[i];
    if (j > i) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }

  // Cooley-Tukey DIT
  for (int32_t len = 2; len <= n; len <<= 1) {
    int32_t half = len >> 1;
    int32_t step = n / len;
    for (int32_t i = 0; i < n; i += len) {
      for (int32_t j = 0, k = 0; j < half; ++j, k += step) {
        float wr = table_[2 * k];
        float wi = table_[2 * k + 1];
        int32_t p = i + j;
        int32_t q = p + half;
        float tr = wr * re[q] - wi * im[q];
        float ti = wr * im[q] + wi * re[q];
        re[q] = re[p] - tr;
        im[q] = im[p] - ti;
        re[p] += tr;
        im[p] += ti;
      }
    }
  }

  // pack in kaldi format
  in_out[0] = re[0];
  in_out[1] = re[n / 2];
  for (int32_t k = 1; k < n / 2; ++k) {
    in_out[2 * k] = re[k];
    in_out[2 * k + 1] = im[k];
  }
}

void ComputePowerSpectrum(std::vector<float> *complex_fft) {
  int32_t dim = static_cast<int32_t>(complex_fft->size());

  // now we have in complex_fft, first half of complex spectrum
  // it's stored as [real0, realN/2, real1, im1, real2, im2, ...]

  float *p = vdata(*complex_fft);
  int32_t half_dim = dim / 2;
  float first_energy = p[0] * p[0];
  float last_energy = p[1] * p[1];  // handle this special case

  for (int32_t i = 1; i < half_dim; ++i) {
    float real = p[i * 2];
    float im = p[i * 2 + 1];
    p[i] = real * real + im * im;
  }
  p[0] = first_energy;
  p[half_dim] = last_energy;
}

void Sqrt(float *in_out, int32_t n) {
  for (int32_t i = 0; i != n; ++i) {
    *in_out = sqrtf(*in_out);
    ++in_out;
  }
}

/* ------------------------- fbank computer ------------------------- */

FbankComputer::FbankComputer(const FbankOptions &opts)
    : opts_(opts), rfft_(opts.frame_opts.PaddedWindowSize()) {
  if (opts.energy_floor > 0.0f) {
    log_energy_floor_ = logf(opts.energy_floor);
  }

  // We'll definitely need the filterbanks info for VTLN warping factor 1.0.
  GetMelBanks(1.0f);
}

FbankComputer::~FbankComputer() {
  for (auto iter = mel_banks_.begin(); iter != mel_banks_.end(); ++iter)
    delete iter->second;
}

const MelBanks *FbankComputer::GetMelBanks(float vtln_warp) {
  MelBanks *this_mel_banks = nullptr;

  auto iter = mel_banks_.find(vtln_warp);
  if (iter == mel_banks_.end()) {
    this_mel_banks = new MelBanks(opts_.mel_opts, opts_.frame_opts, vtln_warp);
    mel_banks_[vtln_warp] = this_mel_banks;
  } else {
    this_mel_banks = iter->second;
  }
  return this_mel_banks;
}

void FbankComputer::Compute(float signal_raw_log_energy, float vtln_warp,
                            std::vector<float> *signal_frame, float *feature) {
  const MelBanks &mel_banks = *(GetMelBanks(vtln_warp));

  KNF_CHECK_EQ(signal_frame->size(), opts_.frame_opts.PaddedWindowSize());

  // Compute energy after window function (not the raw one).
  if (opts_.use_energy && !opts_.raw_energy) {
    signal_raw_log_energy = logf(
        std::max<float>(InnerProduct(vdata(*signal_frame), vdata(*signal_frame),
                                     static_cast<int32_t>(signal_frame->size())),
                        std::numeric_limits<float>::epsilon()));
  }
  rfft_.Compute(vdata(*signal_frame));  // signal_frame is modified in-place
  ComputePowerSpectrum(signal_frame);

  // Use magnitude instead of power if requested.
  if (!opts_.use_power) {
    Sqrt(vdata(*signal_frame), static_cast<int32_t>(signal_frame->size()) / 2 + 1);
  }

  int32_t mel_offset = ((opts_.use_energy && !opts_.htk_compat) ? 1 : 0);

  // Its length is opts_.mel_opts.num_bins
  float *mel_energies = feature + mel_offset;

  // Sum with mel filter banks over the power spectrum
  mel_banks.Compute(vdata(*signal_frame), mel_energies);

  if (opts_.use_log_fbank) {
    // Avoid log of zero (which should be prevented anyway by dithering).
    for (int32_t i = 0; i != opts_.mel_opts.num_bins; ++i) {
      auto t = std::max(mel_energies[i], std::numeric_limits<float>::epsilon());
      mel_energies[i] = logf(t);
    }
  }

  // Copy energy as first value (or the last, if htk_compat == true).
  if (opts_.use_energy) {
    if (opts_.energy_floor > 0.0f && signal_raw_log_energy < log_energy_floor_) {
      signal_raw_log_energy = log_energy_floor_;
    }
    int32_t energy_index = opts_.htk_compat ? opts_.mel_opts.num_bins : 0;
    feature[energy_index] = signal_raw_log_energy;
  }
}

/* ------------------------- online feature ------------------------- */

RecyclingVector::RecyclingVector(int32_t items_to_hold)
    : items_to_hold_(items_to_hold == 0 ? -1 : items_to_hold),
      first_available_index_(0) {}

const float *RecyclingVector::At(int32_t index) const {
  if (index < first_available_index_) {
    KNF_FATAL("attempted to retrieve a feature vector that was recycled");
  }
  KNF_CHECK_LT(index - first_available_index_, items_.size());
  return vdata(items_[index - first_available_index_]);
}

void RecyclingVector::PushBack(std::vector<float> item) {
  // Note: -1 is a larger number when treated as unsigned
  if (items_.size() == static_cast<size_t>(items_to_hold_)) {
    items_.pop_front();
    ++first_available_index_;
  }
  items_.push_back(std::move(item));
}

int32_t RecyclingVector::Size() const {
  return first_available_index_ + static_cast<int32_t>(items_.size());
}

void RecyclingVector::Pop(int32_t n) {
  for (int32_t i = 0; i < n && !items_.empty(); ++i) {
    items_.pop_front();
    ++first_available_index_;
  }
}

template <class C>
OnlineGenericBaseFeature<C>::OnlineGenericBaseFeature(
    const typename C::Options &opts)
    : computer_(opts),
      window_function_(computer_.GetFrameOptions()),
      input_finished_(false),
      waveform_offset_(0) {}

template <class C>
void OnlineGenericBaseFeature<C>::AcceptWaveform(float sampling_rate,
                                                 const float *waveform,
                                                 int32_t n) {
  if (n == 0) {
    return;  // Nothing to do.
  }

  if (input_finished_) {
    KNF_FATAL("AcceptWaveform called after InputFinished() was called");
  }

  KNF_CHECK_EQ(sampling_rate, computer_.GetFrameOptions().samp_freq);

  for (int32_t i = 0; i < n; ++i) {
    waveform_remainder_.push_back(waveform[i]);
  }

  ComputeFeatures();
}

template <class C>
void OnlineGenericBaseFeature<C>::InputFinished() {
  input_finished_ = true;
  ComputeFeatures();
}

template <class C>
void OnlineGenericBaseFeature<C>::ComputeFeatures() {
  const FrameExtractionOptions &frame_opts = computer_.GetFrameOptions();

  int64_t num_samples_total =
      waveform_offset_ + static_cast<int64_t>(waveform_remainder_.size());

  int32_t num_frames_old = features_.Size();

  int32_t num_frames_new =
      NumFrames(num_samples_total, frame_opts, input_finished_);

  KNF_CHECK_GE(num_frames_new, num_frames_old);

  // note: this online feature-extraction code does not support VTLN.
  float vtln_warp = 1.0f;

  std::vector<float> window;
  bool need_raw_log_energy = computer_.NeedRawLogEnergy();

  for (int32_t frame = num_frames_old; frame < num_frames_new; ++frame) {
    std::fill(window.begin(), window.end(), 0.0f);
    float raw_log_energy = 0.0f;
    ExtractWindow(waveform_offset_, waveform_remainder_, frame, frame_opts,
                  window_function_, &window,
                  need_raw_log_energy ? &raw_log_energy : nullptr);

    std::vector<float> this_feature(computer_.Dim());

    computer_.Compute(raw_log_energy, vtln_warp, &window, vdata(this_feature));
    features_.PushBack(std::move(this_feature));
  }

  // OK, we will now discard any portion of the signal that will not be
  // necessary to compute frames in the future.
  int64_t first_sample_of_next_frame =
      FirstSampleOfFrame(num_frames_new, frame_opts);

  int32_t samples_to_discard =
      static_cast<int32_t>(first_sample_of_next_frame - waveform_offset_);

  if (samples_to_discard > 0) {
    // discard the leftmost part of the waveform that we no longer need.
    int32_t new_num_samples =
        static_cast<int32_t>(waveform_remainder_.size()) - samples_to_discard;

    if (new_num_samples <= 0) {
      // odd, but we'll try to handle it.
      waveform_offset_ += waveform_remainder_.size();
      waveform_remainder_.resize(0);
    } else {
      std::vector<float> new_remainder(new_num_samples);

      std::copy(waveform_remainder_.begin() + samples_to_discard,
                waveform_remainder_.end(), new_remainder.begin());
      waveform_offset_ += samples_to_discard;

      vswap(waveform_remainder_, new_remainder);
    }
  }
}

template class OnlineGenericBaseFeature<FbankComputer>;

}  // namespace knf
