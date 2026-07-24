/**
 * onnx.cc - minimal self-contained ONNX inference engine (float32).
 * See onnx.h for the supported op set and design notes.
 */
#include "onnx.h"

#include "vecutil.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sonnx {

/* ============================ tensor basics ============================ */

int64_t Tensor::Numel() const {
  int64_t n = 1;
  for (size_t i = 0; i < shape.size(); ++i) n *= shape[i];
  return n;
}

/* Exact-size vector allocation. ewokstl's resize()/sized-ctor allocate
   2x the requested capacity, which doubles the resident size of every
   model weight; reserve() allocates exactly n, so reserve-then-resize
   keeps big payloads tight on target and is a no-op difference on host. */
template <typename V>
static inline void ResizeExact(V &v, size_t n) {
  v.reserve(n);
  v.resize(n);
}

Tensor Tensor::Float(std::vector<int64_t> shape) {
  Tensor t;
  t.dtype = kFloat;
  t.shape = std::move(shape);
  ResizeExact(t.f, static_cast<size_t>(t.Numel()));
  return t;
}

Tensor Tensor::Int(std::vector<int64_t> shape, int dtype) {
  Tensor t;
  t.dtype = dtype;
  t.shape = std::move(shape);
  ResizeExact(t.i, static_cast<size_t>(t.Numel()));
  return t;
}

Tensor Tensor::Byte(std::vector<int64_t> shape, int dtype) {
  Tensor t;
  t.dtype = dtype;
  t.shape = std::move(shape);
  ResizeExact(t.b, static_cast<size_t>(t.Numel()));
  return t;
}

static int64_t NumelOf(const std::vector<int64_t> &s) {
  int64_t n = 1;
  for (size_t i = 0; i < s.size(); ++i) n *= s[i];
  return n;
}

static std::vector<int64_t> StridesOf(const std::vector<int64_t> &s) {
  std::vector<int64_t> st(s.size(), 1);
  for (int i = static_cast<int>(s.size()) - 2; i >= 0; --i) {
    st[i] = st[i + 1] * s[i + 1];
  }
  return st;
}

/* ewokstl std::vector has no initializer_list ctor; small shape builders */
static std::vector<int64_t> Sh(int64_t a) {
  std::vector<int64_t> v;
  v.push_back(a);
  return v;
}
static std::vector<int64_t> Sh(int64_t a, int64_t b) {
  std::vector<int64_t> v;
  v.push_back(a);
  v.push_back(b);
  return v;
}
static bool Fail(const char *msg, const std::string &op) {
  fprintf(stderr, "sonnx: %s (op %s)\n", msg, op.c_str());
  return false;
}

/* Model::Load progress (0..100), polled from another thread by the UI. */
volatile int g_load_progress = 0;

/* ======================== SIMD micro-kernels ======================== */
/*
 * NEON paths for the GEMM / conv reductions that dominate decode time.
 * ASIMD is baseline on armv8-a, but gcc -O2 does not auto-vectorize the
 * scalar loops, so the hot inner products are written with intrinsics.
 * Every helper keeps a scalar fallback so non-NEON targets (32-bit arm
 * built without -mfpu=neon) compile unchanged.
 */
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(SONNX_NO_NEON)
#include <arm_neon.h>
#define SONNX_NEON 1
#endif

#ifdef SONNX_NEON
static inline float HSumF32(float32x4_t v) {
#ifdef __aarch64__
  return vaddvq_f32(v);
#else
  float32x2_t s = vadd_f32(vget_low_f32(v), vget_high_f32(v));
  s = vpadd_f32(s, s);
  return vget_lane_f32(s, 0);
#endif
}
static inline int32_t HSumS32(int32x4_t v) {
#ifdef __aarch64__
  return vaddvq_s32(v);
#else
  int32x2_t s = vadd_s32(vget_low_s32(v), vget_high_s32(v));
  s = vpadd_s32(s, s);
  return vget_lane_s32(s, 0);
#endif
}
/* widen 8 quantized bytes to int16 (sign depends on the tensor dtype) */
static inline int16x8_t WidenQ8(const uint8_t *p, bool sgn) {
  return sgn ? vmovl_s8(vld1_s8((const int8_t *)p))
             : vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p)));
}
#endif

/* y[0..n) += av * x[0..n)  (float GEMM row update) */
static inline void AxpyF32(float av, const float *x, float *y, int64_t n) {
#ifdef SONNX_NEON
  int64_t j = 0;
  float32x4_t va = vdupq_n_f32(av);
  for (; j + 8 <= n; j += 8) {
    float32x4_t y0 = vld1q_f32(y + j);
    float32x4_t y1 = vld1q_f32(y + j + 4);
#ifdef __aarch64__
    y0 = vfmaq_f32(y0, va, vld1q_f32(x + j));
    y1 = vfmaq_f32(y1, va, vld1q_f32(x + j + 4));
#else
    y0 = vmlaq_f32(y0, va, vld1q_f32(x + j));
    y1 = vmlaq_f32(y1, va, vld1q_f32(x + j + 4));
#endif
    vst1q_f32(y + j, y0);
    vst1q_f32(y + j + 4, y1);
  }
  for (; j < n; ++j) y[j] += av * x[j];
#else
  for (int64_t j = 0; j < n; ++j) y[j] += av * x[j];
#endif
}

/* Σ x[j]*w[j]  (float conv tap reduction) */
static inline float DotF32(const float *x, const float *w, int64_t n) {
#ifdef SONNX_NEON
  int64_t j = 0;
  float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
  for (; j + 8 <= n; j += 8) {
#ifdef __aarch64__
    a0 = vfmaq_f32(a0, vld1q_f32(x + j), vld1q_f32(w + j));
    a1 = vfmaq_f32(a1, vld1q_f32(x + j + 4), vld1q_f32(w + j + 4));
#else
    a0 = vmlaq_f32(a0, vld1q_f32(x + j), vld1q_f32(w + j));
    a1 = vmlaq_f32(a1, vld1q_f32(x + j + 4), vld1q_f32(w + j + 4));
#endif
  }
  float s = HSumF32(vaddq_f32(a0, a1));
  for (; j < n; ++j) s += x[j] * w[j];
  return s;
#else
  float s = 0.0f;
  for (int64_t j = 0; j < n; ++j) s += x[j] * w[j];
  return s;
#endif
}

/* acc[0..n) += av * (Q(b[j]) - bzp)  (int8 GEMM row update; av and
   b-bzp both fit int16, products accumulate exactly in int32) */
static inline void AxpyQ(int32_t av, const uint8_t *b, bool bsgn, int32_t bzp,
                         int32_t *acc, int64_t n) {
#ifdef SONNX_NEON
  int16_t av16 = (int16_t)av;
  int16x8_t vz = vdupq_n_s16((int16_t)bzp);
  int64_t j = 0;
  for (; j + 16 <= n; j += 16) {
    int16x8_t lo = vsubq_s16(WidenQ8(b + j, bsgn), vz);
    int16x8_t hi = vsubq_s16(WidenQ8(b + j + 8, bsgn), vz);
    int32x4_t a0 = vld1q_s32(acc + j);
    int32x4_t a1 = vld1q_s32(acc + j + 4);
    int32x4_t a2 = vld1q_s32(acc + j + 8);
    int32x4_t a3 = vld1q_s32(acc + j + 12);
    a0 = vmlal_n_s16(a0, vget_low_s16(lo), av16);
    a1 = vmlal_n_s16(a1, vget_high_s16(lo), av16);
    a2 = vmlal_n_s16(a2, vget_low_s16(hi), av16);
    a3 = vmlal_n_s16(a3, vget_high_s16(hi), av16);
    vst1q_s32(acc + j, a0);
    vst1q_s32(acc + j + 4, a1);
    vst1q_s32(acc + j + 8, a2);
    vst1q_s32(acc + j + 12, a3);
  }
  for (; j < n; ++j)
    acc[j] += av * ((bsgn ? (int32_t)(int8_t)b[j] : (int32_t)b[j]) - bzp);
#else
  for (int64_t j = 0; j < n; ++j)
    acc[j] += av * ((bsgn ? (int32_t)(int8_t)b[j] : (int32_t)b[j]) - bzp);
#endif
}

/* *dot += Σ Q(x)·Q(w), *sumx += Σ Q(x) over the RAW (un-zeropointed)
   values; the quantized conv uses the expansion
     Σ (x-xz)(w-wz) = dot - wz·sumx - xz·sumw + n·xz·wz
   so zero points never enter the vector loop */
static inline void DotSumQ(const uint8_t *x, bool xsgn, const uint8_t *w,
                           bool wsgn, int64_t n, int32_t *dot, int32_t *sumx) {
#ifdef SONNX_NEON
  int64_t j = 0;
  int32x4_t d0 = vdupq_n_s32(0), d1 = vdupq_n_s32(0);
  int32x4_t sx = vdupq_n_s32(0);
  for (; j + 8 <= n; j += 8) {
    int16x8_t xv = WidenQ8(x + j, xsgn);
    int16x8_t wv = WidenQ8(w + j, wsgn);
    d0 = vmlal_s16(d0, vget_low_s16(xv), vget_low_s16(wv));
    d1 = vmlal_s16(d1, vget_high_s16(xv), vget_high_s16(wv));
    sx = vpadalq_s16(sx, xv);
  }
  *dot += HSumS32(vaddq_s32(d0, d1));
  *sumx += HSumS32(sx);
  for (; j < n; ++j) {
    int32_t xq = xsgn ? (int32_t)(int8_t)x[j] : (int32_t)x[j];
    int32_t wq = wsgn ? (int32_t)(int8_t)w[j] : (int32_t)w[j];
    *dot += xq * wq;
    *sumx += xq;
  }
#else
  for (int64_t j = 0; j < n; ++j) {
    int32_t xq = xsgn ? (int32_t)(int8_t)x[j] : (int32_t)x[j];
    int32_t wq = wsgn ? (int32_t)(int8_t)w[j] : (int32_t)w[j];
    *dot += xq * wq;
    *sumx += xq;
  }
#endif
}

/* Σ Q(w) over raw values (per-row weight sums, hoisted out of the
   output loops of the quantized conv) */
static inline int32_t SumQ(const uint8_t *w, bool wsgn, int64_t n) {
#ifdef SONNX_NEON
  int64_t j = 0;
  int32x4_t s = vdupq_n_s32(0);
  for (; j + 8 <= n; j += 8) s = vpadalq_s16(s, WidenQ8(w + j, wsgn));
  int32_t r = HSumS32(s);
  for (; j < n; ++j) r += wsgn ? (int32_t)(int8_t)w[j] : (int32_t)w[j];
  return r;
#else
  int32_t r = 0;
  for (int64_t j = 0; j < n; ++j)
    r += wsgn ? (int32_t)(int8_t)w[j] : (int32_t)w[j];
  return r;
#endif
}

/* o[0..n) = (Q(q[j]) - z) * s  (per-tensor DequantizeLinear) */
static inline void DequantBytes(const uint8_t *q, bool qsgn, int32_t z,
                                float s, float *o, int64_t n) {
#ifdef SONNX_NEON
  int64_t j = 0;
  int16x8_t vz = vdupq_n_s16((int16_t)z);
  float32x4_t vs = vdupq_n_f32(s);
  for (; j + 8 <= n; j += 8) {
    int16x8_t v = vsubq_s16(WidenQ8(q + j, qsgn), vz);
    vst1q_f32(o + j,
              vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(v))), vs));
    vst1q_f32(o + j + 4,
              vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(v))), vs));
  }
  for (; j < n; ++j)
    o[j] = (float)((qsgn ? (int32_t)(int8_t)q[j] : (int32_t)q[j]) - z) * s;
#else
  for (int64_t j = 0; j < n; ++j)
    o[j] = (float)((qsgn ? (int32_t)(int8_t)q[j] : (int32_t)q[j]) - z) * s;
#endif
}

/* min/max over a float array, range seeded with 0 (ort semantics) */
static inline void MinMaxF32(const float *x, int64_t n, float *mn, float *mx) {
  float lo = 0.0f, hi = 0.0f;
#ifdef SONNX_NEON
  int64_t j = 0;
  float32x4_t vlo = vdupq_n_f32(0.0f), vhi = vdupq_n_f32(0.0f);
  for (; j + 4 <= n; j += 4) {
    float32x4_t v = vld1q_f32(x + j);
    vlo = vminq_f32(vlo, v);
    vhi = vmaxq_f32(vhi, v);
  }
  float l4[4], h4[4];
  vst1q_f32(l4, vlo);
  vst1q_f32(h4, vhi);
  for (int q = 0; q < 4; ++q) {
    if (l4[q] < lo) lo = l4[q];
    if (h4[q] > hi) hi = h4[q];
  }
  for (; j < n; ++j) {
    if (x[j] < lo) lo = x[j];
    if (x[j] > hi) hi = x[j];
  }
#else
  for (int64_t j = 0; j < n; ++j) {
    if (x[j] < lo) lo = x[j];
    if (x[j] > hi) hi = x[j];
  }
#endif
  *mn = lo;
  *mx = hi;
}

/* y[j] = clamp(floor(x[j]/scale + 0.5) + zp, 0, 255)  (DynamicQuantize;
   fdiv + floor keep it bit-exact with the scalar expression) */
static inline void QuantBytes(const float *x, float scale, int32_t zp,
                              uint8_t *y, int64_t n) {
  int64_t j = 0;
#if defined(SONNX_NEON) && defined(__aarch64__)
  float32x4_t vs = vdupq_n_f32(scale);
  float32x4_t vh = vdupq_n_f32(0.5f);
  int32x4_t vz = vdupq_n_s32(zp);
  for (; j + 8 <= n; j += 8) {
    int32x4_t q0 = vcvtmq_s32_f32(vaddq_f32(vdivq_f32(vld1q_f32(x + j), vs), vh));
    int32x4_t q1 = vcvtmq_s32_f32(vaddq_f32(vdivq_f32(vld1q_f32(x + j + 4), vs), vh));
    q0 = vaddq_s32(q0, vz);
    q1 = vaddq_s32(q1, vz);
    /* saturating narrows clamp to 0..255 */
    int16x8_t q16 = vcombine_s16(vqmovn_s32(q0), vqmovn_s32(q1));
    vst1_u8(y + j, vqmovun_s16(q16));
  }
#endif
  for (; j < n; ++j) {
    float q = floorf(x[j] / scale + 0.5f) + (float)zp;
    if (q < 0.0f) q = 0.0f;
    if (q > 255.0f) q = 255.0f;
    y[j] = (uint8_t)q;
  }
}

static inline bool ShapeEq(const std::vector<int64_t> &a,
                           const std::vector<int64_t> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

/* ============================ elementwise ============================ */

// numpy-style broadcast shape; returns false on mismatch.
static bool BroadcastShape(const std::vector<int64_t> &a,
                           const std::vector<int64_t> &b,
                           std::vector<int64_t> *out) {
  size_t ra = a.size(), rb = b.size();
  size_t r = std::max(ra, rb);
  out->assign(r, 1);
  for (size_t k = 0; k < r; ++k) {
    int64_t da = (k + ra >= r) ? a[k + ra - r] : 1;
    int64_t db = (k + rb >= r) ? b[k + rb - r] : 1;
    if (da != db && da != 1 && db != 1) return false;
    (*out)[k] = std::max(da, db);
  }
  return true;
}

template <typename F>
static void BinOp(const Tensor &a, const Tensor &b, Tensor *out, F fn) {
  std::vector<int64_t> oshape;
  if (!BroadcastShape(a.shape, b.shape, &oshape)) {
    *out = Tensor::Float(Sh(0));
    return;
  }
  *out = Tensor::Float(oshape);
  int64_t n = out->Numel();
  /* contiguous fast path (residual adds etc.): same shape, both float —
     skips the per-element index decomposition entirely */
  if (a.dtype == kFloat && b.dtype == kFloat && ShapeEq(a.shape, b.shape)) {
    const float *pa = a.pf(), *pb = b.pf();
    float *po = out->pf();
    for (int64_t i = 0; i < n; ++i) po[i] = fn(pa[i], pb[i]);
    return;
  }
  std::vector<int64_t> sa = StridesOf(a.shape);
  std::vector<int64_t> sb = StridesOf(b.shape);
  size_t r = oshape.size(), ra = a.shape.size(), rb = b.shape.size();
  for (int64_t flat = 0; flat < n; ++flat) {
    int64_t rem = flat, ao = 0, bo = 0;
    for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
      int64_t c = rem % oshape[d];
      rem /= oshape[d];
      int da = d - static_cast<int>(r - ra);
      if (da >= 0 && a.shape[da] > 1) ao += c * sa[da];
      int db = d - static_cast<int>(r - rb);
      if (db >= 0 && b.shape[db] > 1) bo += c * sb[db];
    }
    out->f[flat] = fn(a.AtFloat(ao), b.AtFloat(bo));
  }
}

// integer variant: keeps shape arithmetic (Add/Mul/Div on int64) exact
template <typename F>
static void BinOpInt(const Tensor &a, const Tensor &b, Tensor *out, F fn) {
  std::vector<int64_t> oshape;
  if (!BroadcastShape(a.shape, b.shape, &oshape)) {
    *out = Tensor::Int(Sh(0));
    return;
  }
  *out = Tensor::Int(oshape);
  std::vector<int64_t> sa = StridesOf(a.shape);
  std::vector<int64_t> sb = StridesOf(b.shape);
  size_t r = oshape.size(), ra = a.shape.size(), rb = b.shape.size();
  int64_t n = out->Numel();
  for (int64_t flat = 0; flat < n; ++flat) {
    int64_t rem = flat, ao = 0, bo = 0;
    for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
      int64_t c = rem % oshape[d];
      rem /= oshape[d];
      int da = d - static_cast<int>(r - ra);
      if (da >= 0 && a.shape[da] > 1) ao += c * sa[da];
      int db = d - static_cast<int>(r - rb);
      if (db >= 0 && b.shape[db] > 1) bo += c * sb[db];
    }
    out->i[flat] = fn(a.AtInt(ao), b.AtInt(bo));
  }
}

template <typename F>
static void UnOp(const Tensor &a, Tensor *out, F fn) {
  *out = Tensor::Float(a.shape);
  int64_t n = a.Numel();
  if (a.dtype == kFloat) {  // skip the AtFloat dtype branch per element
    const float *pa = a.pf();
    float *po = out->pf();
    for (int64_t i = 0; i < n; ++i) po[i] = fn(pa[i]);
    return;
  }
  for (int64_t i = 0; i < n; ++i) out->f[i] = fn(a.AtFloat(i));
}

static float ErfApprox(float x) {
  // Abramowitz & Stegun 7.1.26, |err| <= 1.5e-7
  float sign = x < 0 ? -1.0f : 1.0f;
  float ax = fabsf(x);
  float t = 1.0f / (1.0f + 0.3275911f * ax);
  float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t -
                     0.284496736f) * t + 0.254829592f) * t * expf(-ax * ax);
  return sign * y;
}

/* ============================ shape helpers ============================ */

// data-movement ops must preserve the input dtype (this graph runs
// Gather/Concat/Slice/... on int64 shape tensors, not only floats)
static Tensor MakeLike(const Tensor &x, const std::vector<int64_t> &shape) {
  if (x.dtype == kFloat) return Tensor::Float(shape);
  if (x.IsByte()) return Tensor::Byte(shape, x.dtype);
  return Tensor::Int(shape, x.dtype);
}

static inline void CopyElem(const Tensor &src, int64_t si, Tensor *dst,
                            int64_t di) {
  if (dst->dtype == kFloat)
    dst->f[di] = src.AtFloat(si);
  else if (dst->IsByte())
    dst->b[di] = static_cast<uint8_t>(src.AtInt(si) & 0xff);
  else
    dst->i[di] = src.AtInt(si);
}

static void TransposeOp(const Tensor &a, const std::vector<int64_t> &perm_in,
                        Tensor *out) {
  size_t r = a.shape.size();
  std::vector<int64_t> perm = perm_in;
  if (perm.empty()) {
    for (size_t i = 0; i < r; ++i) perm.push_back(r - 1 - i);
  }
  std::vector<int64_t> oshape(r);
  for (size_t i = 0; i < r; ++i) oshape[i] = a.shape[perm[i]];
  *out = MakeLike(a, oshape);
  std::vector<int64_t> sa = StridesOf(a.shape);
  std::vector<int64_t> so = StridesOf(oshape);
  int64_t n = a.Numel();
  if (a.dtype == kFloat) {
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, src = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t c = rem / so[d];
        rem %= so[d];
        src += c * sa[perm[d]];
      }
      out->f[flat] = a.f[src];
    }
  } else {
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, src = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t c = rem / so[d];
        rem %= so[d];
        src += c * sa[perm[d]];
      }
      CopyElem(a, src, out, flat);
    }
  }
}

static void ConcatOp(const std::vector<const Tensor *> &xs, int64_t axis,
                     Tensor *out) {
  size_t r = xs[0]->shape.size();
  if (axis < 0) axis += r;
  int64_t outer = 1, inner = 1, adim = 0;
  for (int64_t i = 0; i < axis; ++i) outer *= xs[0]->shape[i];
  for (size_t i = axis + 1; i < r; ++i) inner *= xs[0]->shape[i];
  std::vector<int64_t> oshape = xs[0]->shape;
  for (size_t k = 0; k < xs.size(); ++k) adim += xs[k]->shape[axis];
  oshape[axis] = adim;
  *out = MakeLike(*xs[0], oshape);
  if (out->dtype == kFloat) {
    float *dst = out->pf();
    for (int64_t o = 0; o < outer; ++o) {
      for (size_t k = 0; k < xs.size(); ++k) {
        int64_t block = xs[k]->shape[axis] * inner;
        const float *src = xs[k]->pf() + o * block;
        memcpy(dst, src, static_cast<size_t>(block) * sizeof(float));
        dst += block;
      }
    }
  } else {
    int64_t di = 0;
    for (int64_t o = 0; o < outer; ++o) {
      for (size_t k = 0; k < xs.size(); ++k) {
        int64_t block = xs[k]->shape[axis] * inner;
        for (int64_t e = 0; e < block; ++e)
          CopyElem(*xs[k], o * block + e, out, di++);
      }
    }
  }
}

// broadcast a to shape target (right-aligned rules), any dtype
static void ExpandTo(const Tensor &a, const std::vector<int64_t> &target,
                     Tensor *out) {
  *out = MakeLike(a, target);
  size_t r = target.size(), ra = a.shape.size();
  std::vector<int64_t> sa = StridesOf(a.shape);
  int64_t n = out->Numel();
  for (int64_t flat = 0; flat < n; ++flat) {
    int64_t rem = flat, ao = 0;
    for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
      int64_t c = rem % target[d];
      rem /= target[d];
      int da = d - static_cast<int>(r - ra);
      if (da >= 0 && a.shape[da] > 1) ao += c * sa[da];
    }
    CopyElem(a, ao, out, flat);
  }
}

static void SoftmaxLike(const Tensor &a, int64_t axis, Tensor *out,
                        bool log_out) {
  size_t r = a.shape.size();
  if (axis < 0) axis += r;
  int64_t outer = 1, inner = 1, dim = a.shape[axis];
  for (int64_t i = 0; i < axis; ++i) outer *= a.shape[i];
  for (size_t i = axis + 1; i < r; ++i) inner *= a.shape[i];
  *out = Tensor::Float(a.shape);
  for (int64_t o = 0; o < outer; ++o) {
    for (int64_t in = 0; in < inner; ++in) {
      const float *base = a.pf() + o * dim * inner + in;
      float mx = base[0];
      for (int64_t d = 1; d < dim; ++d) mx = std::max(mx, base[d * inner]);
      float sum = 0;
      for (int64_t d = 0; d < dim; ++d) sum += expf(base[d * inner] - mx);
      float lse = mx + logf(sum);
      float *ob = out->pf() + o * dim * inner + in;
      for (int64_t d = 0; d < dim; ++d) {
        float v = base[d * inner] - lse;
        ob[d * inner] = log_out ? v : expf(v);
      }
    }
  }
}

/* ============================ matmul / gemm ============================ */

static bool MatMulOp(const Tensor &a, const Tensor &b, Tensor *out) {
  size_t ra = a.shape.size(), rb = b.shape.size();
  if (ra < 2 || rb < 2) return false;
  int64_t m = a.shape[ra - 2], k = a.shape[ra - 1];
  int64_t k2 = b.shape[rb - 2], n = b.shape[rb - 1];
  if (k != k2) return false;

  std::vector<int64_t> ba, bb;
  for (size_t i = 0; i + 2 < ra; ++i) ba.push_back(a.shape[i]);
  for (size_t i = 0; i + 2 < rb; ++i) bb.push_back(b.shape[i]);
  std::vector<int64_t> batch;
  if (!BroadcastShape(ba, bb, &batch)) return false;

  std::vector<int64_t> oshape = batch;
  oshape.push_back(m);
  oshape.push_back(n);
  *out = Tensor::Float(oshape);

  int64_t nb = NumelOf(batch);
  std::vector<int64_t> sa = StridesOf(ba);
  std::vector<int64_t> sb = StridesOf(bb);
  size_t rba = ba.size(), rbb = bb.size(), rbz = batch.size();

  for (int64_t bi = 0; bi < nb; ++bi) {
    int64_t rem = bi, ao = 0, bo = 0;
    for (int d = static_cast<int>(rbz) - 1; d >= 0; --d) {
      int64_t c = rem % batch[d];
      rem /= batch[d];
      int da = d - static_cast<int>(rbz - rba);
      if (da >= 0 && ba[da] > 1) ao += c * sa[da];
      int db = d - static_cast<int>(rbz - rbb);
      if (db >= 0 && bb[db] > 1) bo += c * sb[db];
    }
    const float *pa = a.pf() + ao * m * k;
    const float *pb = b.pf() + bo * k * n;
    float *po = out->pf() + bi * m * n;
    for (int64_t i = 0; i < m; ++i) {
      float *orow = po + i * n;
      memset(orow, 0, static_cast<size_t>(n) * sizeof(float));
      const float *arow = pa + i * k;
      for (int64_t kk = 0; kk < k; ++kk)
        AxpyF32(arow[kk], pb + kk * n, orow, n);  // NEON row update
    }
  }
  return true;
}

/* ============================ conv / pool ============================ */

static bool ConvOp(const Tensor &x, const Tensor &w, const Tensor *bias,
                   const std::vector<int64_t> &strides,
                   const std::vector<int64_t> &pads,
                   const std::vector<int64_t> &dilations, int64_t group,
                   Tensor *out) {
  size_t sp = x.shape.size() - 2;  // 1 or 2 spatial dims
  if (sp != 1 && sp != 2) return false;
  int64_t n = x.shape[0], c = x.shape[1];
  int64_t m = w.shape[0];
  int64_t cpw = w.shape[1];
  if (group < 1 || c % group != 0 || m % group != 0 || cpw * group != c)
    return false;

  int64_t xs[2] = {1, 1}, ks[2] = {1, 1}, st[2] = {1, 1}, pd[2] = {0, 0},
          pe[2] = {0, 0}, dl[2] = {1, 1}, os[2] = {1, 1};
  for (size_t i = 0; i < sp; ++i) {
    xs[i] = x.shape[2 + i];
    ks[i] = w.shape[2 + i];
    if (i < strides.size()) st[i] = strides[i];
    if (i < dilations.size()) dl[i] = dilations[i];
    if (i < pads.size()) pd[i] = pads[i];
    if (i + sp < pads.size()) pe[i] = pads[i + sp];
    os[i] = (xs[i] + pd[i] + pe[i] - dl[i] * (ks[i] - 1) - 1) / st[i] + 1;
    if (os[i] <= 0) return false;
  }

  std::vector<int64_t> oshape = Sh(n, m);
  for (size_t i = 0; i < sp; ++i) oshape.push_back(os[i]);
  *out = Tensor::Float(oshape);

  int64_t mpg = m / group;    // output channels per group
  int64_t cpg = c / group;    // input channels per group
  int64_t ow = os[1], oh = os[0];
  int64_t xw = xs[1], xh = xs[0];
  int64_t kw = ks[1], kh = ks[0];

  const float *px = x.pf();
  const float *pw = w.pf();
  /* contiguous-reduction fast paths (NEON dot): 1-D conv reduces over
     ky (x and w both contiguous when dilation is 1); 2-D reduces over
     kx per kernel row. Boundary clipping shrinks the dot range. */
  bool fast1d = (xw == 1 && kw == 1 && dl[0] == 1);
  bool fast2d = (!fast1d && dl[1] == 1);

  for (int64_t ni = 0; ni < n; ++ni) {
    for (int64_t g = 0; g < group; ++g) {
      for (int64_t mi = 0; mi < mpg; ++mi) {
        int64_t mo = g * mpg + mi;
        for (int64_t oy = 0; oy < oh; ++oy) {
          for (int64_t ox = 0; ox < ow; ++ox) {
            float sum = bias ? bias->f[mo] : 0.0f;
            for (int64_t ci = 0; ci < cpg; ++ci) {
              int64_t ch = g * cpg + ci;
              const float *xc = px + (ni * c + ch) * xh * xw;
              const float *wc = pw + (mo * cpw + ci) * kh * kw;
              if (fast1d) {
                int64_t iy0 = oy * st[0] - pd[0];
                int64_t k0 = iy0 < 0 ? -iy0 : 0;
                int64_t k1 = kh < xh - iy0 ? kh : xh - iy0;
                if (k1 > k0) sum += DotF32(xc + iy0 + k0, wc + k0, k1 - k0);
              } else if (fast2d) {
                int64_t ix0 = ox * st[1] - pd[1];
                int64_t k0 = ix0 < 0 ? -ix0 : 0;
                int64_t k1 = kw < xw - ix0 ? kw : xw - ix0;
                if (k1 <= k0) continue;
                for (int64_t ky = 0; ky < kh; ++ky) {
                  int64_t iy = oy * st[0] + ky * dl[0] - pd[0];
                  if (iy < 0 || iy >= xh) continue;
                  sum += DotF32(xc + iy * xw + ix0 + k0, wc + ky * kw + k0,
                                k1 - k0);
                }
              } else {  // dilated: original scalar taps
                for (int64_t ky = 0; ky < kh; ++ky) {
                  int64_t iy = oy * st[0] + ky * dl[0] - pd[0];
                  if (iy < 0 || iy >= xh) continue;
                  for (int64_t kx = 0; kx < kw; ++kx) {
                    int64_t ix = ox * st[1] + kx * dl[1] - pd[1];
                    if (ix < 0 || ix >= xw) continue;
                    sum += xc[iy * xw + ix] * wc[ky * kw + kx];
                  }
                }
              }
            }
            out->f[((ni * m + mo) * oh + oy) * ow + ox] = sum;
          }
        }
      }
    }
  }
  return true;
}

// ConvInteger: quantized conv (x uint8/int8, w int8/uint8), int32 accumulate.
// Mirrors ConvOp; zero points are scalar for x and scalar/per-channel for w.
static bool ConvIntegerOp(const Tensor &x, const Tensor &w, const Tensor *xzp,
                          const Tensor *wzp,
                          const std::vector<int64_t> &strides,
                          const std::vector<int64_t> &pads,
                          const std::vector<int64_t> &dilations, int64_t group,
                          Tensor *out) {
  size_t sp = x.shape.size() - 2;  // 1 or 2 spatial dims
  if (sp != 1 && sp != 2) return false;
  int64_t n = x.shape[0], c = x.shape[1];
  int64_t m = w.shape[0];
  int64_t cpw = w.shape[1];
  if (group < 1 || c % group != 0 || m % group != 0 || cpw * group != c)
    return false;

  int64_t xs[2] = {1, 1}, ks[2] = {1, 1}, st[2] = {1, 1}, pd[2] = {0, 0},
          pe[2] = {0, 0}, dl[2] = {1, 1}, os[2] = {1, 1};
  for (size_t i = 0; i < sp; ++i) {
    xs[i] = x.shape[2 + i];
    ks[i] = w.shape[2 + i];
    if (i < strides.size()) st[i] = strides[i];
    if (i < dilations.size()) dl[i] = dilations[i];
    if (i < pads.size()) pd[i] = pads[i];
    if (i + sp < pads.size()) pe[i] = pads[i + sp];
    os[i] = (xs[i] + pd[i] + pe[i] - dl[i] * (ks[i] - 1) - 1) / st[i] + 1;
    if (os[i] <= 0) return false;
  }

  std::vector<int64_t> oshape = Sh(n, m);
  for (size_t i = 0; i < sp; ++i) oshape.push_back(os[i]);
  *out = Tensor::Int(oshape, kInt32);

  int32_t xz = xzp ? static_cast<int32_t>(xzp->AtInt(0)) : 0;
  bool wz_per_ch = wzp && wzp->Numel() > 1;
  int32_t wz0 = wzp ? static_cast<int32_t>(wzp->AtInt(0)) : 0;

  int64_t mpg = m / group;    // output channels per group
  int64_t cpg = c / group;    // input channels per group
  int64_t ow = os[1], oh = os[0];
  int64_t xw = xs[1], xh = xs[0];
  int64_t kw = ks[1], kh = ks[0];

  const uint8_t *px = x.pb();
  const uint8_t *pw = w.pb();
  bool xsgn = x.dtype == kInt8;
  bool wsgn = w.dtype == kInt8;
  /*
   * Vector fast paths (NEON widening dot) via the expansion
   *   Σ_taps (x-xz)(w-wz) = dot(x,w) - wz·Σx - xz·Σw + n_taps·xz·wz
   * which also absorbs the zero-padding taps (x = 0 there): with Σw
   * taken over the WHOLE kernel row and dot/Σx over the clipped range,
   * the padding contribution (-xz)(w-wz) falls out exactly.
   */
  bool fast1d = (xw == 1 && kw == 1 && dl[0] == 1);
  bool fast2d = (!fast1d && dl[1] == 1);
  std::vector<int32_t> wsum;  // per-(ci,row) raw weight sums for current mo
  if (fast1d)
    ResizeExact(wsum, static_cast<size_t>(cpg));
  else if (fast2d)
    ResizeExact(wsum, static_cast<size_t>(cpg * kh));
  int32_t *pws = vdata(wsum);

  for (int64_t ni = 0; ni < n; ++ni) {
    for (int64_t g = 0; g < group; ++g) {
      for (int64_t mi = 0; mi < mpg; ++mi) {
        int64_t mo = g * mpg + mi;
        int32_t wz = wz_per_ch ? static_cast<int32_t>(wzp->AtInt(mo)) : wz0;
        if (fast1d) {
          for (int64_t ci = 0; ci < cpg; ++ci)
            pws[ci] = SumQ(pw + (mo * cpw + ci) * kh, wsgn, kh);
        } else if (fast2d) {
          for (int64_t ci = 0; ci < cpg; ++ci)
            for (int64_t ky = 0; ky < kh; ++ky)
              pws[ci * kh + ky] =
                  SumQ(pw + ((mo * cpw + ci) * kh + ky) * kw, wsgn, kw);
        }
        for (int64_t oy = 0; oy < oh; ++oy) {
          for (int64_t ox = 0; ox < ow; ++ox) {
            int32_t sum = 0;
            for (int64_t ci = 0; ci < cpg; ++ci) {
              int64_t ch = g * cpg + ci;
              const uint8_t *xc = px + (ni * c + ch) * xh * xw;
              const uint8_t *wc = pw + (mo * cpw + ci) * kh * kw;
              if (fast1d) {
                int64_t iy0 = oy * st[0] - pd[0];
                int64_t k0 = iy0 < 0 ? -iy0 : 0;
                int64_t k1 = kh < xh - iy0 ? kh : xh - iy0;
                int32_t dot = 0, sx = 0;
                if (k1 > k0)
                  DotSumQ(xc + iy0 + k0, xsgn, wc + k0, wsgn, k1 - k0, &dot,
                          &sx);
                sum += dot - wz * sx - xz * pws[ci] +
                       static_cast<int32_t>(kh) * xz * wz;
              } else if (fast2d) {
                int64_t ix0 = ox * st[1] - pd[1];
                int64_t k0 = ix0 < 0 ? -ix0 : 0;
                int64_t k1 = kw < xw - ix0 ? kw : xw - ix0;
                for (int64_t ky = 0; ky < kh; ++ky) {
                  int64_t iy = oy * st[0] + ky * dl[0] - pd[0];
                  int32_t dot = 0, sx = 0;
                  if (iy >= 0 && iy < xh && k1 > k0)
                    DotSumQ(xc + iy * xw + ix0 + k0, xsgn, wc + ky * kw + k0,
                            wsgn, k1 - k0, &dot, &sx);
                  sum += dot - wz * sx - xz * pws[ci * kh + ky] +
                         static_cast<int32_t>(kw) * xz * wz;
                }
              } else {  // dilated: original scalar taps
                for (int64_t ky = 0; ky < kh; ++ky) {
                  int64_t iy = oy * st[0] + ky * dl[0] - pd[0];
                  if (iy < 0 || iy >= xh) {
                    // zero-padded input still contributes (-xz) * w
                    for (int64_t kx = 0; kx < kw; ++kx) {
                      int32_t wv =
                          w.QAt(((mo * cpw + ci) * kh + ky) * kw + kx) - wz;
                      sum += (0 - xz) * wv;
                    }
                    continue;
                  }
                  for (int64_t kx = 0; kx < kw; ++kx) {
                    int64_t ix = ox * st[1] + kx * dl[1] - pd[1];
                    int32_t wv =
                        w.QAt(((mo * cpw + ci) * kh + ky) * kw + kx) - wz;
                    if (ix < 0 || ix >= xw) {
                      sum += (0 - xz) * wv;
                      continue;
                    }
                    int32_t xv =
                        x.QAt(((ni * c + ch) * xh + iy) * xw + ix) - xz;
                    sum += xv * wv;
                  }
                }
              }
            }
            out->i[((ni * m + mo) * oh + oy) * ow + ox] = sum;
          }
        }
      }
    }
  }
  return true;
}

static bool PoolOp(const Tensor &x, const std::vector<int64_t> &kernel,
                   const std::vector<int64_t> &strides,
                   const std::vector<int64_t> &pads, bool is_max,
                   bool count_include_pad, Tensor *out) {
  size_t sp = x.shape.size() - 2;
  if (sp != 1 && sp != 2) return false;
  int64_t n = x.shape[0], c = x.shape[1];

  int64_t xs[2] = {1, 1}, ks[2] = {1, 1}, st[2] = {1, 1}, pd[2] = {0, 0},
          os[2] = {1, 1};
  for (size_t i = 0; i < sp; ++i) {
    xs[i] = x.shape[2 + i];
    ks[i] = kernel[i];
    if (i < strides.size()) st[i] = strides[i];
    if (i < pads.size()) pd[i] = pads[i];
    os[i] = (xs[i] + 2 * pd[i] - ks[i]) / st[i] + 1;
    if (os[i] <= 0) return false;
  }

  std::vector<int64_t> oshape = Sh(n, c);
  for (size_t i = 0; i < sp; ++i) oshape.push_back(os[i]);
  *out = Tensor::Float(oshape);

  int64_t ow = os[1], oh = os[0];
  int64_t xw = xs[1], xh = xs[0];
  int64_t kw = ks[1], kh = ks[0];

  for (int64_t ni = 0; ni < n; ++ni) {
    for (int64_t ch = 0; ch < c; ++ch) {
      for (int64_t oy = 0; oy < oh; ++oy) {
        for (int64_t ox = 0; ox < ow; ++ox) {
          float acc = is_max ? -3.4e38f : 0.0f;
          int64_t cnt = 0;
          for (int64_t ky = 0; ky < kh; ++ky) {
            int64_t iy = oy * st[0] + ky - pd[0];
            bool yok = (iy >= 0 && iy < xh);
            for (int64_t kx = 0; kx < kw; ++kx) {
              int64_t ix = ox * st[1] + kx - pd[1];
              bool ok = yok && (ix >= 0 && ix < xw);
              if (is_max) {
                if (ok) {
                  float xv = sp == 1
                                 ? x.f[(ni * c + ch) * xw + ix]
                                 : x.f[((ni * c + ch) * xh + iy) * xw + ix];
                  acc = std::max(acc, xv);
                }
              } else {
                if (ok) {
                  float xv = sp == 1
                                 ? x.f[(ni * c + ch) * xw + ix]
                                 : x.f[((ni * c + ch) * xh + iy) * xw + ix];
                  acc += xv;
                  ++cnt;
                } else if (count_include_pad) {
                  ++cnt;
                }
              }
            }
          }
          int64_t oidx = sp == 1 ? (ni * c + ch) * ow + ox
                                 : ((ni * c + ch) * oh + oy) * ow + ox;
          out->f[oidx] = is_max ? acc
                                : (cnt > 0 ? acc / static_cast<float>(cnt)
                                           : 0.0f);
        }
      }
    }
  }
  return true;
}

}  // namespace sonnx
namespace sonnx {

/* ============================ op dispatcher ============================ */

static const Model::Attr *FindAttr(const Model::Node &node, const char *name) {
  auto it = node.attr.find(name);
  return it == node.attr.end() ? nullptr : &it->second;
}

static int64_t AttrInt(const Model::Node &node, const char *name, int64_t dflt) {
  const Model::Attr *a = FindAttr(node, name);
  return (a && a->type == 1) ? a->i : dflt;
}

static float AttrFloat(const Model::Node &node, const char *name, float dflt) {
  const Model::Attr *a = FindAttr(node, name);
  return (a && a->type == 2) ? a->f : dflt;
}

static std::vector<int64_t> AttrInts(const Model::Node &node,
                                     const char *name) {
  const Model::Attr *a = FindAttr(node, name);
  if (a && a->type == 5) return a->ints;
  return {};
}

static int64_t FixAxis(int64_t axis, size_t rank) {
  if (axis < 0) axis += rank;
  return axis;
}

// fetch an int tensor's values (float accepted: shape math may round-trip
// through float ops; values are small and exact)
static bool IntValues(const Tensor *t, std::vector<int64_t> *out) {
  if (!t) return false;
  out->clear();
  for (int64_t i = 0; i < t->Numel(); ++i) out->push_back(t->AtInt(i));
  return true;
}

/* ---------- nested graphs (Loop / If) ---------- */

// value scope for subgraph execution: local env -> local initializers ->
// enclosing scope (ONNX subgraphs may read outer values by name)
struct Scope {
  const std::map<std::string, Tensor> *env = nullptr;
  const std::map<std::string, Tensor> *inits = nullptr;
  const Scope *parent = nullptr;
};

static const Tensor *ScopeLookup(const Scope *s, const std::string &name) {
  for (; s; s = s->parent) {
    if (s->env) {
      auto it = s->env->find(name);
      if (it != s->env->end()) return &it->second;
    }
    if (s->inits) {
      auto it = s->inits->find(name);
      if (it != s->inits->end()) return &it->second;
    }
  }
  return nullptr;
}

static bool RunGraph(const std::vector<Model::Node> &nodes,
                     const std::map<std::string, Tensor> &inits,
                     const std::vector<std::string> &output_names,
                     std::map<std::string, Tensor> *env, const Scope *parent,
                     std::vector<Tensor> *outputs);

// stack tensors along a new leading axis (Loop scan outputs)
static void StackTensors(std::vector<Tensor> *xs, Tensor *out) {
  if (xs->empty()) {
    *out = Tensor::Float(Sh(0));
    return;
  }
  std::vector<const Tensor *> ptrs;
  for (size_t k = 0; k < xs->size(); ++k) {
    (*xs)[k].shape.insert((*xs)[k].shape.begin(), 1);
    ptrs.push_back(&(*xs)[k]);
  }
  ConcatOp(ptrs, 0, out);
}

static bool ExecNode(const Model::Node &node,
                     const std::vector<const Tensor *> &in,
                     std::vector<Tensor> *outs, const Scope *scope) {
  const std::string &op = node.op;
  outs->clear();

  /* ---------- elementwise binary ---------- */
  if (op == "Add" || op == "Sub" || op == "Mul" || op == "Div" || op == "Pow") {
    if (in.size() < 2 || !in[0] || !in[1]) return Fail("missing input", op);
    Tensor o;
    bool ints = in[0]->IsInt() && !in[0]->IsByte() && in[1]->IsInt() &&
                !in[1]->IsByte() && op != "Pow";
    if (ints) {  // ONNX integer arithmetic (Div truncates toward zero)
      if (op == "Add") BinOpInt(*in[0], *in[1], &o, [](int64_t a, int64_t b) { return a + b; });
      if (op == "Sub") BinOpInt(*in[0], *in[1], &o, [](int64_t a, int64_t b) { return a - b; });
      if (op == "Mul") BinOpInt(*in[0], *in[1], &o, [](int64_t a, int64_t b) { return a * b; });
      if (op == "Div") BinOpInt(*in[0], *in[1], &o, [](int64_t a, int64_t b) { return b ? a / b : 0; });
    } else {
      if (op == "Add") BinOp(*in[0], *in[1], &o, [](float a, float b) { return a + b; });
      if (op == "Sub") BinOp(*in[0], *in[1], &o, [](float a, float b) { return a - b; });
      if (op == "Mul") BinOp(*in[0], *in[1], &o, [](float a, float b) { return a * b; });
      if (op == "Div") BinOp(*in[0], *in[1], &o, [](float a, float b) { return a / b; });
      if (op == "Pow") BinOp(*in[0], *in[1], &o, [](float a, float b) { return powf(a, b); });
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- elementwise unary ---------- */
  if (op == "Relu" || op == "Sigmoid" || op == "Tanh" || op == "Erf" ||
      op == "Exp" || op == "Log" || op == "Sqrt" || op == "Abs" ||
      op == "Neg" || op == "Floor" || op == "Softplus" || op == "Sign" ||
      op == "Reciprocal" || op == "Sin" || op == "Cos" || op == "Not") {
    if (!in[0]) return Fail("missing input", op);
    Tensor o;
    if (op == "Relu") UnOp(*in[0], &o, [](float a) { return a > 0 ? a : 0.0f; });
    if (op == "Sigmoid") UnOp(*in[0], &o, [](float a) { return 1.0f / (1.0f + expf(-a)); });
    if (op == "Tanh") UnOp(*in[0], &o, [](float a) { return tanhf(a); });
    if (op == "Erf") UnOp(*in[0], &o, [](float a) { return ErfApprox(a); });
    if (op == "Exp") UnOp(*in[0], &o, [](float a) { return expf(a); });
    if (op == "Log") UnOp(*in[0], &o, [](float a) { return logf(a); });
    if (op == "Sqrt") UnOp(*in[0], &o, [](float a) { return sqrtf(a); });
    if (op == "Abs") UnOp(*in[0], &o, [](float a) { return fabsf(a); });
    if (op == "Neg") UnOp(*in[0], &o, [](float a) { return -a; });
    if (op == "Floor") UnOp(*in[0], &o, [](float a) { return floorf(a); });
    if (op == "Softplus") UnOp(*in[0], &o, [](float a) { return a > 20.0f ? a : logf(1.0f + expf(a)); });
    if (op == "Sign") UnOp(*in[0], &o, [](float a) { return a > 0 ? 1.0f : (a < 0 ? -1.0f : 0.0f); });
    if (op == "Reciprocal") UnOp(*in[0], &o, [](float a) { return 1.0f / a; });
    if (op == "Sin") UnOp(*in[0], &o, [](float a) { return sinf(a); });
    if (op == "Cos") UnOp(*in[0], &o, [](float a) { return cosf(a); });
    if (op == "Not") {  // logical not -> bool tensor
      const Tensor &x = *in[0];
      o = Tensor::Byte(x.shape, kBool);
      for (int64_t i = 0; i < x.Numel(); ++i)
        o.b[i] = (x.AtFloat(i) != 0.0f) ? 0 : 1;
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Clip") {
    if (!in[0]) return Fail("missing input", op);
    float mn = -3.4e38f, mx = 3.4e38f;
    const Model::Attr *amin = FindAttr(node, "min");
    const Model::Attr *amax = FindAttr(node, "max");
    if (amin && amin->type == 2) mn = amin->f;
    if (amax && amax->type == 2) mx = amax->f;
    if (in.size() > 1 && in[1]) mn = in[1]->AtFloat(0);
    if (in.size() > 2 && in[2]) mx = in[2]->AtFloat(0);
    Tensor o;
    UnOp(*in[0], &o, [mn, mx](float a) { return a < mn ? mn : (a > mx ? mx : a); });
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Identity" || op == "Dropout") {
    outs->push_back(*in[0]);
    if (op == "Dropout" && node.output.size() > 1) {
      // mask output: all true, matching input shape
      Tensor mask = Tensor::Int(in[0]->shape, kInt64);
      for (size_t i = 0; i < mask.i.size(); ++i) mask.i[i] = 1;
      outs->push_back(std::move(mask));
    }
    return true;
  }

  /* ---------- matmul / gemm ---------- */
  if (op == "MatMul") {
    Tensor o;
    if (!in[0] || !in[1] || !MatMulOp(*in[0], *in[1], &o))
      return Fail("matmul failed", op);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Gemm") {
    if (!in[0] || !in[1]) return Fail("missing input", op);
    float alpha = AttrFloat(node, "alpha", 1.0f);
    float beta = AttrFloat(node, "beta", 1.0f);
    int64_t ta = AttrInt(node, "transA", 0);
    int64_t tb = AttrInt(node, "transB", 0);
    Tensor a = *in[0], b = *in[1];
    if (ta) TransposeOp(a, {}, &a);
    if (tb) TransposeOp(b, {}, &b);
    Tensor o;
    if (!MatMulOp(a, b, &o)) return Fail("gemm matmul failed", op);
    if (alpha != 1.0f)
      for (size_t i = 0; i < o.f.size(); ++i) o.f[i] *= alpha;
    if (in.size() > 2 && in[2]) {
      Tensor c = *in[2];
      Tensor oc;
      BinOp(o, c, &oc, [beta](float x, float y) { return x + beta * y; });
      o = std::move(oc);
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- conv / pool ---------- */
  if (op == "Conv") {
    if (!in[0] || !in[1]) return Fail("missing input", op);
    Tensor o;
    if (!ConvOp(*in[0], *in[1], in.size() > 2 ? in[2] : nullptr,
                AttrInts(node, "strides"), AttrInts(node, "pads"),
                AttrInts(node, "dilations"), AttrInt(node, "group", 1), &o))
      return Fail("conv failed", op);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "MaxPool" || op == "AveragePool") {
    Tensor o;
    if (!PoolOp(*in[0], AttrInts(node, "kernel_shape"),
                AttrInts(node, "strides"), AttrInts(node, "pads"),
                op == "MaxPool",
                AttrInt(node, "count_include_pad", 0) != 0, &o))
      return Fail("pool failed", op);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "GlobalAveragePool" || op == "GlobalMaxPool") {
    const Tensor &x = *in[0];
    size_t sp = x.shape.size() - 2;
    int64_t n = x.shape[0], c = x.shape[1];
    int64_t sz = 1;
    for (size_t i = 2; i < x.shape.size(); ++i) sz *= x.shape[i];
    std::vector<int64_t> oshape = Sh(n, c);
    for (size_t i = 0; i < sp; ++i) oshape.push_back(1);
    Tensor o = Tensor::Float(oshape);
    bool is_max = op == "GlobalMaxPool";
    for (int64_t i = 0; i < n; ++i) {
      for (int64_t ch = 0; ch < c; ++ch) {
        const float *base = x.pf() + (i * c + ch) * sz;
        float acc = is_max ? -3.4e38f : 0.0f;
        for (int64_t s = 0; s < sz; ++s)
          acc = is_max ? std::max(acc, base[s]) : acc + base[s];
        o.f[(i * c + ch)] = is_max ? acc : acc / static_cast<float>(sz);
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- normalization ---------- */
  if (op == "BatchNormalization") {
    if (in.size() < 5) return Fail("missing input", op);
    const Tensor &x = *in[0];
    float eps = AttrFloat(node, "epsilon", 1e-5f);
    int64_t n = x.shape[0], c = x.shape[1];
    int64_t sz = x.Numel() / (n * c);
    Tensor o = Tensor::Float(x.shape);
    for (int64_t ch = 0; ch < c; ++ch) {
      float scale = in[1]->f[ch];
      float b = in[2]->f[ch];
      float mean = in[3]->f[ch];
      float var = in[4]->f[ch];
      float inv = scale / sqrtf(var + eps);
      for (int64_t i = 0; i < n; ++i) {
        const float *src = x.pf() + (i * c + ch) * sz;
        float *dst = o.pf() + (i * c + ch) * sz;
        for (int64_t s = 0; s < sz; ++s) dst[s] = (src[s] - mean) * inv + b;
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "LayerNormalization") {
    if (in.size() < 3) return Fail("missing input", op);
    const Tensor &x = *in[0];
    int64_t axis = FixAxis(AttrInt(node, "axis", -1), x.shape.size());
    float eps = AttrFloat(node, "epsilon", 1e-5f);
    int64_t inner = 1, outer = 1;
    for (int64_t i = axis; i < static_cast<int64_t>(x.shape.size()); ++i)
      inner *= x.shape[i];
    for (int64_t i = 0; i < axis; ++i) outer *= x.shape[i];
    Tensor o = Tensor::Float(x.shape);
    for (int64_t u = 0; u < outer; ++u) {
      const float *src = x.pf() + u * inner;
      float *dst = o.pf() + u * inner;
      float mean = 0;
      for (int64_t i = 0; i < inner; ++i) mean += src[i];
      mean /= static_cast<float>(inner);
      float var = 0;
      for (int64_t i = 0; i < inner; ++i) {
        float d = src[i] - mean;
        var += d * d;
      }
      var /= static_cast<float>(inner);
      float inv = 1.0f / sqrtf(var + eps);
      for (int64_t i = 0; i < inner; ++i) {
        float norm = (src[i] - mean) * inv;
        float s = in[1]->f[i % in[1]->Numel()];
        float b = in.size() > 2 && in[2] ? in[2]->f[i % in[2]->Numel()] : 0.0f;
        dst[i] = norm * s + b;
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- softmax family ---------- */
  if (op == "Softmax" || op == "LogSoftmax") {
    int64_t axis = AttrInt(node, "axis", -1);
    Tensor o;
    SoftmaxLike(*in[0], axis, &o, op == "LogSoftmax");
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "ReduceMean") {
    const Tensor &x = *in[0];
    std::vector<int64_t> axes = AttrInts(node, "axes");
    if (in.size() > 1 && in[1] && in[1]->IsInt()) IntValues(in[1], &axes);
    int64_t keep = AttrInt(node, "keepdims", 1);
    size_t r = x.shape.size();
    if (axes.empty()) {
      for (size_t i = 0; i < r; ++i) axes.push_back(i);
    }
    for (size_t i = 0; i < axes.size(); ++i) axes[i] = FixAxis(axes[i], r);
    std::vector<int64_t> oshape;
    for (size_t i = 0; i < r; ++i) {
      bool red = std::find(axes.begin(), axes.end(), (int64_t)i) != axes.end();
      if (red) {
        if (keep) oshape.push_back(1);
      } else {
        oshape.push_back(x.shape[i]);
      }
    }
    Tensor o = Tensor::Float(oshape.empty() ? std::vector<int64_t>{1} : oshape);
    std::vector<int64_t> cnt(o.Numel(), 0);
    std::vector<int64_t> so = StridesOf(o.shape);
    // iterate over x, accumulate
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = x.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, oidx = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / sx[d];
        rem %= sx[d];
        bool red = std::find(axes.begin(), axes.end(), (int64_t)d) != axes.end();
        if (!red) {
          // find position in out shape
          size_t kept = 0;
          for (size_t e = 0; e < d; ++e)
            if (std::find(axes.begin(), axes.end(), (int64_t)e) == axes.end())
              ++kept;
          if (keep)
            oidx += cd * so[d];
          else
            oidx += cd * so[kept];
        }
      }
      o.f[oidx] += x.f[flat];
      cnt[oidx]++;
    }
    for (int64_t i = 0; i < o.Numel(); ++i)
      if (cnt[i] > 0) o.f[i] /= static_cast<float>(cnt[i]);
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- shape manipulation ---------- */
  if (op == "Transpose") {
    Tensor o;
    TransposeOp(*in[0], AttrInts(node, "perm"), &o);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Reshape") {
    const Tensor &x = *in[0];
    std::vector<int64_t> dims;
    if (in.size() > 1 && in[1]) {
      if (!IntValues(in[1], &dims)) return Fail("bad shape input", op);
    } else {
      dims = AttrInts(node, "shape");
    }
    bool allowzero = AttrInt(node, "allowzero", 0) != 0;
    std::vector<int64_t> oshape;
    int64_t known = 1;
    int infer = -1;
    for (size_t i = 0; i < dims.size(); ++i) {
      int64_t d = dims[i];
      if (d == 0 && !allowzero) {
        d = x.shape[i];
      }
      if (d == -1) {
        infer = static_cast<int>(i);
        oshape.push_back(1);
      } else {
        oshape.push_back(d);
        known *= d;
      }
    }
    if (infer >= 0) {
      int64_t total = x.Numel();
      oshape[infer] = known != 0 ? total / known : 0;
    }
    Tensor o = x;
    o.shape = oshape;
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Flatten") {
    const Tensor &x = *in[0];
    int64_t axis = FixAxis(AttrInt(node, "axis", 1), x.shape.size());
    int64_t d0 = 1, d1 = 1;
    for (int64_t i = 0; i < axis; ++i) d0 *= x.shape[i];
    for (size_t i = axis; i < x.shape.size(); ++i) d1 *= x.shape[i];
    Tensor o = x;
    o.shape = Sh(d0, d1);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Squeeze" || op == "Unsqueeze") {
    const Tensor &x = *in[0];
    std::vector<int64_t> axes = AttrInts(node, "axes");
    if (in.size() > 1 && in[1] && in[1]->IsInt()) IntValues(in[1], &axes);
    size_t r = x.shape.size();
    Tensor o = x;
    if (op == "Squeeze") {
      if (axes.empty()) {
        std::vector<int64_t> s;
        for (size_t i = 0; i < r; ++i)
          if (x.shape[i] != 1) s.push_back(x.shape[i]);
        o.shape = s.empty() ? std::vector<int64_t>{1} : s;
      } else {
        for (size_t i = 0; i < axes.size(); ++i) axes[i] = FixAxis(axes[i], r);
        std::vector<int64_t> s;
        for (size_t i = 0; i < r; ++i)
          if (std::find(axes.begin(), axes.end(), (int64_t)i) == axes.end())
            s.push_back(x.shape[i]);
        o.shape = s.empty() ? std::vector<int64_t>{1} : s;
      }
    } else {
      std::vector<int64_t> s = x.shape;
      size_t out_r = r + axes.size();
      for (size_t i = 0; i < axes.size(); ++i) {
        int64_t a = axes[i];
        if (a < 0) a += out_r;
        s.insert(s.begin() + a, 1);
      }
      o.shape = s;
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Concat") {
    int64_t axis = AttrInt(node, "axis", 0);
    std::vector<const Tensor *> xs;
    for (size_t i = 0; i < in.size(); ++i)
      if (in[i]) xs.push_back(in[i]);
    if (xs.empty()) return Fail("no inputs", op);
    Tensor o;
    ConcatOp(xs, axis, &o);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Split") {
    const Tensor &x = *in[0];
    int64_t axis = FixAxis(AttrInt(node, "axis", 0), x.shape.size());
    std::vector<int64_t> split = AttrInts(node, "split");
    if (in.size() > 1 && in[1] && in[1]->IsInt()) split = in[1]->i;
    if (split.empty()) {
      size_t nout = node.output.size();
      int64_t each = x.shape[axis] / static_cast<int64_t>(nout);
      split.assign(nout, each);
    }
    int64_t outer = 1, inner = 1;
    for (int64_t i = 0; i < axis; ++i) outer *= x.shape[i];
    for (size_t i = axis + 1; i < x.shape.size(); ++i) inner *= x.shape[i];
    int64_t off = 0;
    for (size_t k = 0; k < split.size(); ++k) {
      std::vector<int64_t> oshape = x.shape;
      oshape[axis] = split[k];
      Tensor o = MakeLike(x, oshape);
      int64_t block = split[k] * inner;
      for (int64_t u = 0; u < outer; ++u) {
        int64_t sbase = (u * x.shape[axis] + off) * inner;
        if (x.dtype == kFloat) {
          memcpy(o.pf() + u * block, x.pf() + sbase,
                 static_cast<size_t>(block) * sizeof(float));
        } else {
          for (int64_t e = 0; e < block; ++e)
            CopyElem(x, sbase + e, &o, u * block + e);
        }
      }
      off += split[k];
      outs->push_back(std::move(o));
    }
    return true;
  }

  if (op == "Slice") {
    const Tensor &x = *in[0];
    std::vector<int64_t> starts, ends, axes, steps;
    if (in.size() > 1 && in[1]) IntValues(in[1], &starts);
    if (in.size() > 2 && in[2]) IntValues(in[2], &ends);
    if (in.size() > 3 && in[3]) IntValues(in[3], &axes);
    if (in.size() > 4 && in[4]) IntValues(in[4], &steps);
    if (starts.empty()) starts = AttrInts(node, "starts");
    if (ends.empty()) ends = AttrInts(node, "ends");
    if (axes.empty()) axes = AttrInts(node, "axes");
    if (axes.empty())
      for (size_t i = 0; i < starts.size(); ++i) axes.push_back(i);
    if (steps.empty()) steps.assign(starts.size(), 1);

    size_t r = x.shape.size();
    std::vector<int64_t> b(r), e(r), s(r, 1);
    for (size_t i = 0; i < r; ++i) e[i] = x.shape[i];
    for (size_t k = 0; k < starts.size(); ++k) {
      int64_t a = FixAxis(axes[k], r);
      int64_t dim = x.shape[a];
      int64_t st = starts[k], en = ends[k];
      int64_t sp = k < steps.size() ? steps[k] : 1;
      if (st < 0) st += dim;
      if (en < 0) en += dim;
      if (sp > 0) {  // forward: st in [0,dim], en in [0,dim]
        st = std::max<int64_t>(0, std::min<int64_t>(st, dim));
        en = std::max<int64_t>(0, std::min<int64_t>(en, dim));
      } else {  // reverse: st in [0,dim-1], en in [-1,dim-1]
        st = std::max<int64_t>(0, std::min<int64_t>(st, dim - 1));
        en = std::max<int64_t>(-1, std::min<int64_t>(en, dim - 1));
      }
      b[a] = st;
      e[a] = en;
      s[a] = sp;
    }
    std::vector<int64_t> oshape(r);
    for (size_t i = 0; i < r; ++i)
      oshape[i] = std::max<int64_t>(
          0, (e[i] - b[i] + s[i] + (s[i] > 0 ? -1 : 1)) / s[i]);
    Tensor o = MakeLike(x, oshape);
    std::vector<int64_t> so = StridesOf(o.shape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = o.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, src = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / so[d];
        rem %= so[d];
        src += (b[d] + cd * s[d]) * sx[d];
      }
      CopyElem(x, src, &o, flat);
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Expand") {
    std::vector<int64_t> target;
    if (!IntValues(in[1], &target)) return Fail("bad shape", op);
    // ONNX Expand: output = broadcast(input.shape, target)
    std::vector<int64_t> oshape;
    if (!BroadcastShape(in[0]->shape, target, &oshape))
      return Fail("expand mismatch", op);
    Tensor o;
    ExpandTo(*in[0], oshape, &o);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Tile") {
    std::vector<int64_t> reps;
    if (!IntValues(in[1], &reps)) return Fail("bad repeats", op);
    const Tensor &x = *in[0];
    size_t r = x.shape.size();
    std::vector<int64_t> oshape(r);
    for (size_t i = 0; i < r; ++i) oshape[i] = x.shape[i] * reps[i];
    Tensor o = MakeLike(x, oshape);
    std::vector<int64_t> so = StridesOf(o.shape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = o.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, src = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / so[d];
        rem %= so[d];
        src += (cd % x.shape[d]) * sx[d];
      }
      CopyElem(x, src, &o, flat);
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Pad") {
    const Tensor &x = *in[0];
    std::vector<int64_t> pads;
    if (in.size() > 1 && in[1]) IntValues(in[1], &pads);
    if (pads.empty()) pads = AttrInts(node, "pads");
    float cval = 0.0f;
    if (in.size() > 2 && in[2]) cval = in[2]->AtFloat(0);
    const Model::Attr *av = FindAttr(node, "value");
    if (av && av->type == 2) cval = av->f;
    size_t r = x.shape.size();
    std::vector<int64_t> oshape(r);
    for (size_t i = 0; i < r; ++i) oshape[i] = x.shape[i] + pads[i] + pads[i + r];
    Tensor o = Tensor::Float(oshape);
    for (size_t i = 0; i < o.f.size(); ++i) o.f[i] = cval;
    std::vector<int64_t> so = StridesOf(o.shape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = x.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, dst = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / sx[d];
        rem %= sx[d];
        dst += (cd + pads[d]) * so[d];
      }
      o.f[dst] = x.f[flat];
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Gather") {
    const Tensor &x = *in[0];
    const Tensor &idx = *in[1];
    int64_t axis = FixAxis(AttrInt(node, "axis", 0), x.shape.size());
    int64_t outer = 1, inner = 1, dim = x.shape[axis];
    for (int64_t i = 0; i < axis; ++i) outer *= x.shape[i];
    for (size_t i = axis + 1; i < x.shape.size(); ++i) inner *= x.shape[i];
    std::vector<int64_t> oshape;
    for (int64_t i = 0; i < axis; ++i) oshape.push_back(x.shape[i]);
    for (size_t i = 0; i < idx.shape.size(); ++i) oshape.push_back(idx.shape[i]);
    for (size_t i = static_cast<size_t>(axis) + 1; i < x.shape.size(); ++i)
      oshape.push_back(x.shape[i]);
    Tensor o = MakeLike(x, oshape);
    int64_t ni = idx.Numel();
    for (int64_t u = 0; u < outer; ++u) {
      for (int64_t j = 0; j < ni; ++j) {
        int64_t ix = idx.AtInt(j);
        if (ix < 0) ix += dim;
        int64_t sbase = (u * dim + ix) * inner;
        int64_t dbase = (u * ni + j) * inner;
        if (x.dtype == kFloat) {
          memcpy(o.pf() + dbase, x.pf() + sbase,
                 static_cast<size_t>(inner) * sizeof(float));
        } else {
          for (int64_t e = 0; e < inner; ++e)
            CopyElem(x, sbase + e, &o, dbase + e);
        }
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- misc ---------- */
  if (op == "Shape") {
    const Tensor &x = *in[0];
    Tensor o = Tensor::Int(Sh(static_cast<int64_t>(x.shape.size())));
    for (size_t i = 0; i < x.shape.size(); ++i) o.i[i] = x.shape[i];
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Size") {
    Tensor o = Tensor::Int(Sh(1));
    o.i[0] = in[0]->Numel();
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Cast") {
    int64_t to = AttrInt(node, "to", 1);
    const Tensor &x = *in[0];
    if (to == kFloat) {
      Tensor o = Tensor::Float(x.shape);
      for (int64_t i = 0; i < x.Numel(); ++i) o.f[i] = x.AtFloat(i);
      outs->push_back(std::move(o));
    } else if (to == kBool || to == kUint8 || to == kInt8) {
      Tensor o = Tensor::Byte(x.shape, static_cast<int>(to));
      for (int64_t i = 0; i < x.Numel(); ++i) {
        if (to == kBool)
          o.b[i] = (x.AtFloat(i) != 0.0f) ? 1 : 0;
        else
          o.b[i] = static_cast<uint8_t>(x.AtInt(i) & 0xff);
      }
      outs->push_back(std::move(o));
    } else {
      Tensor o = Tensor::Int(x.shape, to == kInt32 ? kInt32 : kInt64);
      for (int64_t i = 0; i < x.Numel(); ++i) o.i[i] = x.AtInt(i);
      outs->push_back(std::move(o));
    }
    return true;
  }

  if (op == "Constant") {
    const Model::Attr *av = FindAttr(node, "value");
    if (av && av->type == 4) {
      outs->push_back(av->t);
      return true;
    }
    const Model::Attr *af = FindAttr(node, "value_float");
    if (af && af->type == 2) {
      Tensor o = Tensor::Float(Sh(1));
      o.f[0] = af->f;
      outs->push_back(std::move(o));
      return true;
    }
    const Model::Attr *ai = FindAttr(node, "value_int");
    if (ai && ai->type == 1) {
      Tensor o = Tensor::Int(Sh(1));
      o.i[0] = ai->i;
      outs->push_back(std::move(o));
      return true;
    }
    const Model::Attr *afs = FindAttr(node, "value_floats");
    if (afs && afs->type == 6) {
      Tensor o = Tensor::Float(Sh(static_cast<int64_t>(afs->floats.size())));
      for (size_t i = 0; i < afs->floats.size(); ++i) o.f[i] = afs->floats[i];
      outs->push_back(std::move(o));
      return true;
    }
    const Model::Attr *ais = FindAttr(node, "value_ints");
    if (ais && ais->type == 5) {
      Tensor o = Tensor::Int(Sh(static_cast<int64_t>(ais->ints.size())));
      o.i = ais->ints;
      outs->push_back(std::move(o));
      return true;
    }
    return Fail("unsupported constant", op);
  }

  if (op == "ConstantOfShape") {
    std::vector<int64_t> dims;
    if (!IntValues(in[0], &dims)) return Fail("bad shape", op);
    const Model::Attr *av = FindAttr(node, "value");
    if (av && av->type == 4 && av->t.Numel() > 0 && av->t.dtype != kFloat) {
      // int/bool fill (index masks, scatter zeros, ...)
      Tensor o = MakeLike(av->t, dims);
      for (int64_t i = 0; i < o.Numel(); ++i) CopyElem(av->t, 0, &o, i);
      outs->push_back(std::move(o));
      return true;
    }
    float val = 0.0f;
    if (av && av->type == 4 && av->t.Numel() > 0) val = av->t.AtFloat(0);
    Tensor o = Tensor::Float(dims);
    for (size_t i = 0; i < o.f.size(); ++i) o.f[i] = val;
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "DequantizeLinear") {
    const Tensor &x = *in[0];
    const Tensor &scale = *in[1];
    const Tensor *zp = in.size() > 2 ? in[2] : nullptr;
    int64_t axis = in.size() > 3 ? 1 : 1;  // per-axis not common; scalar default
    (void)axis;
    Tensor o = Tensor::Float(x.shape);
    int64_t n = x.Numel();
    if (scale.Numel() == 1) {
      float s = scale.AtFloat(0);
      int64_t z = zp ? zp->AtInt(0) : 0;
      if (x.IsByte()) {  // NEON widen+convert fast path
        DequantBytes(x.pb(), x.dtype == kInt8, static_cast<int32_t>(z), s,
                     o.pf(), n);
      } else {
        for (int64_t i = 0; i < n; ++i)
          o.f[i] = (static_cast<float>(x.AtInt(i) - z)) * s;
      }
    } else {
      // per-channel along axis 1 (conv weights) or 0
      int64_t ch_dim = x.shape.size() > 1 ? 1 : 0;
      int64_t block = x.Numel() / x.shape[ch_dim];
      for (int64_t i = 0; i < n; ++i) {
        int64_t ch = i / block;
        float s = scale.AtFloat(ch);
        int64_t z = zp ? zp->AtInt(ch) : 0;
        o.f[i] = (static_cast<float>(x.AtInt(i) - z)) * s;
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- quantization (onnxruntime dynamic-quant pattern) ---------- */
  if (op == "DynamicQuantizeLinear") {
    const Tensor &x = *in[0];
    int64_t n = x.Numel();
    const float *pxf = x.dtype == kFloat ? x.pf() : nullptr;
    float mn = 0.0f, mx = 0.0f;  // range always includes 0 (ort behavior)
    if (pxf) {
      MinMaxF32(pxf, n, &mn, &mx);  // NEON min/max
    } else {
      for (int64_t i = 0; i < n; ++i) {
        float v = x.AtFloat(i);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
    }
    float scale = (mx - mn) / 255.0f;
    if (scale <= 0.0f) scale = 1e-8f;
    float zpf = floorf((0.0f - mn) / scale + 0.5f);
    if (zpf < 0.0f) zpf = 0.0f;
    if (zpf > 255.0f) zpf = 255.0f;
    int32_t zp = static_cast<int32_t>(zpf);
    Tensor y = Tensor::Byte(x.shape, kUint8);
    if (pxf) {
      QuantBytes(pxf, scale, zp, y.pb(), n);  // NEON quantize
    } else {
      for (int64_t i = 0; i < n; ++i) {
        float q = floorf(x.AtFloat(i) / scale + 0.5f) + static_cast<float>(zp);
        if (q < 0.0f) q = 0.0f;
        if (q > 255.0f) q = 255.0f;
        y.b[i] = static_cast<uint8_t>(q);
      }
    }
    std::vector<int64_t> scalar;  // 0-d shape
    Tensor ys = Tensor::Float(scalar);
    ys.f[0] = scale;
    Tensor yzp = Tensor::Byte(scalar, kUint8);
    yzp.b[0] = static_cast<uint8_t>(zp);
    outs->push_back(std::move(y));
    outs->push_back(std::move(ys));
    outs->push_back(std::move(yzp));
    return true;
  }

  if (op == "MatMulInteger") {
    const Tensor &a = *in[0], &b = *in[1];
    if (a.shape.size() < 2 || b.shape.size() < 2) return Fail("rank < 2", op);
    // zero points: scalar, per-row [M] for a, or per-column [N] for b
    const Tensor *azpt = in.size() > 2 && in[2] ? in[2] : nullptr;
    const Tensor *bzpt = in.size() > 3 && in[3] ? in[3] : nullptr;
    int32_t azp0 = azpt ? static_cast<int32_t>(azpt->AtInt(0)) : 0;
    int32_t bzp0 = bzpt ? static_cast<int32_t>(bzpt->AtInt(0)) : 0;
    bool azp_row = azpt && azpt->Numel() > 1;
    bool bzp_col = bzpt && bzpt->Numel() > 1;
    size_t ra = a.shape.size(), rb = b.shape.size();
    int64_t m = a.shape[ra - 2], k = a.shape[ra - 1];
    int64_t n = b.shape[rb - 1];
    if (b.shape[rb - 2] != k) return Fail("dim mismatch", op);
    std::vector<int64_t> ba, bb;
    for (size_t i = 0; i + 2 < ra; ++i) ba.push_back(a.shape[i]);
    for (size_t i = 0; i + 2 < rb; ++i) bb.push_back(b.shape[i]);
    std::vector<int64_t> batch;
    if (!BroadcastShape(ba, bb, &batch)) return Fail("bad batch dims", op);
    std::vector<int64_t> oshape = batch;
    oshape.push_back(m);
    oshape.push_back(n);
    Tensor o = Tensor::Int(oshape, kInt32);
    int64_t nb = NumelOf(batch);
    std::vector<int64_t> sa = StridesOf(ba), sb = StridesOf(bb);
    size_t rba = ba.size(), rbb = bb.size(), rbz = batch.size();
    const uint8_t *pa = a.pb();
    const uint8_t *pbq = b.pb();
    bool asgn = a.dtype == kInt8;
    bool bsgn = b.dtype == kInt8;
    /* int32 row accumulator: the NEON kernel needs a dense int32 target
       (the Tensor int payload is int64 lanes), widened once per row */
    std::vector<int32_t> acc;
    ResizeExact(acc, static_cast<size_t>(n));
    int32_t *pacc = vdata(acc);
    int64_t *poi = o.pi();
    for (int64_t bi = 0; bi < nb; ++bi) {
      int64_t rem = bi, ao = 0, bo = 0;
      for (int d = static_cast<int>(rbz) - 1; d >= 0; --d) {
        int64_t c = rem % batch[d];
        rem /= batch[d];
        int da = d - static_cast<int>(rbz - rba);
        if (da >= 0 && ba[da] > 1) ao += c * sa[da];
        int db = d - static_cast<int>(rbz - rbb);
        if (db >= 0 && bb[db] > 1) bo += c * sb[db];
      }
      int64_t abase = ao * m * k, bbase = bo * k * n, obase = bi * m * n;
      for (int64_t i = 0; i < m; ++i) {
        int32_t azp = azp_row ? static_cast<int32_t>(azpt->AtInt(i)) : azp0;
        memset(pacc, 0, static_cast<size_t>(n) * sizeof(int32_t));
        if (!bzp_col) {
          const uint8_t *arow = pa + abase + i * k;
          for (int64_t kk = 0; kk < k; ++kk) {
            int32_t av =
                (asgn ? (int32_t)(int8_t)arow[kk] : (int32_t)arow[kk]) - azp;
            if (av)  // zero rows are common when azp matches; skip them
              AxpyQ(av, pbq + bbase + kk * n, bsgn, bzp0, pacc, n);
          }
        } else {  // rare per-column zero point: scalar path
          for (int64_t kk = 0; kk < k; ++kk) {
            int32_t av = a.QAt(abase + i * k + kk) - azp;
            int64_t brow = bbase + kk * n;
            for (int64_t j = 0; j < n; ++j)
              pacc[j] += av * (b.QAt(brow + j) -
                               static_cast<int32_t>(bzpt->AtInt(j)));
          }
        }
        int64_t orow = obase + i * n;
        for (int64_t j = 0; j < n; ++j) poi[orow + j] = pacc[j];
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- comparison / selection ---------- */
  if (op == "Less" || op == "Greater" || op == "Equal" ||
      op == "LessOrEqual" || op == "GreaterOrEqual") {
    const Tensor &a = *in[0], &b = *in[1];
    std::vector<int64_t> oshape;
    if (!BroadcastShape(a.shape, b.shape, &oshape)) return Fail("bad broadcast", op);
    Tensor o = Tensor::Byte(oshape, kBool);
    std::vector<int64_t> sa = StridesOf(a.shape), sb = StridesOf(b.shape);
    size_t r = oshape.size(), ra = a.shape.size(), rb = b.shape.size();
    int64_t n = o.Numel();
    int cmp = op == "Less" ? 0 : op == "Greater" ? 1 : op == "Equal" ? 2
              : op == "LessOrEqual" ? 3 : 4;
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, ao = 0, bo = 0;
      for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
        int64_t c = rem % oshape[d];
        rem /= oshape[d];
        int da = d - static_cast<int>(r - ra);
        if (da >= 0 && a.shape[da] > 1) ao += c * sa[da];
        int db = d - static_cast<int>(r - rb);
        if (db >= 0 && b.shape[db] > 1) bo += c * sb[db];
      }
      float va = a.AtFloat(ao), vb = b.AtFloat(bo);
      bool res = cmp == 0 ? va < vb : cmp == 1 ? va > vb : cmp == 2 ? va == vb
                 : cmp == 3 ? va <= vb : va >= vb;
      o.b[flat] = res ? 1 : 0;
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Where") {
    if (in.size() < 3) return Fail("needs 3 inputs", op);
    const Tensor &c = *in[0], &x = *in[1], &y = *in[2];
    std::vector<int64_t> s1, oshape;
    if (!BroadcastShape(c.shape, x.shape, &s1) ||
        !BroadcastShape(s1, y.shape, &oshape))
      return Fail("bad broadcast", op);
    bool isfloat = x.dtype == kFloat || y.dtype == kFloat;
    std::vector<int64_t> sc = StridesOf(c.shape), sx = StridesOf(x.shape),
                         sy = StridesOf(y.shape);
    size_t r = oshape.size(), rc = c.shape.size(), rx = x.shape.size(),
           ry = y.shape.size();
    int64_t n = NumelOf(oshape);
    Tensor o = isfloat ? Tensor::Float(oshape) : Tensor::Int(oshape, kInt64);
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, co = 0, xo = 0, yo = 0;
      for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
        int64_t cd = rem % oshape[d];
        rem /= oshape[d];
        int dc = d - static_cast<int>(r - rc);
        if (dc >= 0 && c.shape[dc] > 1) co += cd * sc[dc];
        int dx = d - static_cast<int>(r - rx);
        if (dx >= 0 && x.shape[dx] > 1) xo += cd * sx[dx];
        int dy = d - static_cast<int>(r - ry);
        if (dy >= 0 && y.shape[dy] > 1) yo += cd * sy[dy];
      }
      if (isfloat)
        o.f[flat] = c.AtInt(co) ? x.AtFloat(xo) : y.AtFloat(yo);
      else
        o.i[flat] = c.AtInt(co) ? x.AtInt(xo) : y.AtInt(yo);
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "Range") {
    if (in.size() < 3) return Fail("needs 3 inputs", op);
    if (in[0]->dtype == kFloat) {
      float s = in[0]->AtFloat(0), l = in[1]->AtFloat(0), d = in[2]->AtFloat(0);
      int64_t n = 0;
      if (d > 0)
        for (float v = s; v < l; v += d) ++n;
      else if (d < 0)
        for (float v = s; v > l; v += d) ++n;
      Tensor o = Tensor::Float(Sh(n));
      for (int64_t i = 0; i < n; ++i) o.f[i] = s + d * static_cast<float>(i);
      outs->push_back(std::move(o));
    } else {
      int64_t s = in[0]->AtInt(0), l = in[1]->AtInt(0), d = in[2]->AtInt(0);
      int64_t n = 0;
      if (d > 0)
        for (int64_t v = s; v < l; v += d) ++n;
      else if (d < 0)
        for (int64_t v = s; v > l; v += d) ++n;
      Tensor o = Tensor::Int(Sh(n), kInt64);
      for (int64_t i = 0; i < n; ++i) o.i[i] = s + d * i;
      outs->push_back(std::move(o));
    }
    return true;
  }

  if (op == "ReduceMax") {
    const Tensor &x = *in[0];
    std::vector<int64_t> axes = AttrInts(node, "axes");
    if (in.size() > 1 && in[1] && in[1]->IsInt()) IntValues(in[1], &axes);
    int64_t keep = AttrInt(node, "keepdims", 1);
    size_t r = x.shape.size();
    if (axes.empty()) {
      for (size_t i = 0; i < r; ++i) axes.push_back(i);
    }
    for (size_t i = 0; i < axes.size(); ++i) axes[i] = FixAxis(axes[i], r);
    std::vector<int64_t> oshape;
    for (size_t i = 0; i < r; ++i) {
      bool red = std::find(axes.begin(), axes.end(), (int64_t)i) != axes.end();
      if (red) {
        if (keep) oshape.push_back(1);
      } else {
        oshape.push_back(x.shape[i]);
      }
    }
    Tensor o = MakeLike(x, oshape.empty() ? Sh(1) : oshape);
    bool flt = x.dtype == kFloat;
    for (int64_t i = 0; i < o.Numel(); ++i) {
      if (flt)
        o.f[i] = -3.4e38f;
      else
        o.i[i] = INT64_MIN;
    }
    std::vector<int64_t> so = StridesOf(o.shape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = x.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, oidx = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / sx[d];
        rem %= sx[d];
        bool red = std::find(axes.begin(), axes.end(), (int64_t)d) != axes.end();
        if (!red) {
          size_t kept = 0;
          for (size_t e = 0; e < d; ++e)
            if (std::find(axes.begin(), axes.end(), (int64_t)e) == axes.end())
              ++kept;
          if (keep)
            oidx += cd * so[d];
          else
            oidx += cd * so[kept];
        }
      }
      if (flt) {
        if (x.f[flat] > o.f[oidx]) o.f[oidx] = x.f[flat];
      } else {
        int64_t v = x.AtInt(flat);
        if (v > o.i[oidx]) o.i[oidx] = v;
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- logical binary ---------- */
  if (op == "And" || op == "Or" || op == "Xor") {
    if (in.size() < 2 || !in[0] || !in[1]) return Fail("missing input", op);
    const Tensor &a = *in[0], &b = *in[1];
    std::vector<int64_t> oshape;
    if (!BroadcastShape(a.shape, b.shape, &oshape)) return Fail("bad broadcast", op);
    Tensor o = Tensor::Byte(oshape, kBool);
    std::vector<int64_t> sa = StridesOf(a.shape), sb = StridesOf(b.shape);
    size_t r = oshape.size(), ra = a.shape.size(), rb = b.shape.size();
    int64_t n = o.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, ao = 0, bo = 0;
      for (int d = static_cast<int>(r) - 1; d >= 0; --d) {
        int64_t c = rem % oshape[d];
        rem /= oshape[d];
        int da = d - static_cast<int>(r - ra);
        if (da >= 0 && a.shape[da] > 1) ao += c * sa[da];
        int db = d - static_cast<int>(r - rb);
        if (db >= 0 && b.shape[db] > 1) bo += c * sb[db];
      }
      bool va = a.AtInt(ao) != 0, vb = b.AtInt(bo) != 0;
      bool res = op == "And" ? (va && vb) : op == "Or" ? (va || vb) : (va != vb);
      o.b[flat] = res ? 1 : 0;
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "ReduceSum") {
    const Tensor &x = *in[0];
    std::vector<int64_t> axes = AttrInts(node, "axes");
    if (in.size() > 1 && in[1] && in[1]->IsInt()) IntValues(in[1], &axes);
    int64_t keep = AttrInt(node, "keepdims", 1);
    size_t r = x.shape.size();
    if (axes.empty()) {
      if (AttrInt(node, "noop_with_empty_axes", 0)) {
        outs->push_back(x);
        return true;
      }
      for (size_t i = 0; i < r; ++i) axes.push_back(i);
    }
    for (size_t i = 0; i < axes.size(); ++i) axes[i] = FixAxis(axes[i], r);
    std::vector<int64_t> oshape;
    for (size_t i = 0; i < r; ++i) {
      bool red = std::find(axes.begin(), axes.end(), (int64_t)i) != axes.end();
      if (red) {
        if (keep) oshape.push_back(1);
      } else {
        oshape.push_back(x.shape[i]);
      }
    }
    if (oshape.empty()) oshape = Sh(1);
    bool flt = x.dtype == kFloat;
    Tensor o = flt ? Tensor::Float(oshape) : Tensor::Int(oshape, kInt64);
    std::vector<int64_t> so = StridesOf(o.shape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t n = x.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      int64_t rem = flat, oidx = 0;
      for (size_t d = 0; d < r; ++d) {
        int64_t cd = rem / sx[d];
        rem %= sx[d];
        bool red = std::find(axes.begin(), axes.end(), (int64_t)d) != axes.end();
        if (!red) {
          size_t kept = 0;
          for (size_t e = 0; e < d; ++e)
            if (std::find(axes.begin(), axes.end(), (int64_t)e) == axes.end())
              ++kept;
          if (keep)
            oidx += cd * so[d];
          else
            oidx += cd * so[kept];
        }
      }
      if (flt)
        o.f[oidx] += x.f[flat];
      else
        o.i[oidx] += x.AtInt(flat);
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- indexing ---------- */
  if (op == "NonZero") {
    const Tensor &x = *in[0];
    size_t r = x.shape.size();
    std::vector<int64_t> sx = StridesOf(x.shape);
    std::vector<int64_t> idxs;  // flat indices of nonzero elements
    int64_t n = x.Numel();
    for (int64_t flat = 0; flat < n; ++flat) {
      bool nz = x.dtype == kFloat ? x.f[flat] != 0.0f : x.AtInt(flat) != 0;
      if (nz) idxs.push_back(flat);
    }
    int64_t nnz = static_cast<int64_t>(idxs.size());
    Tensor o = Tensor::Int(Sh(static_cast<int64_t>(r), nnz), kInt64);
    for (int64_t j = 0; j < nnz; ++j) {
      int64_t rem = idxs[j];
      for (size_t d = 0; d < r; ++d) {
        o.i[d * nnz + j] = rem / sx[d];
        rem %= sx[d];
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "GatherND") {
    if (in.size() < 2 || !in[0] || !in[1]) return Fail("missing input", op);
    const Tensor &x = *in[0], &idx = *in[1];
    int64_t b = AttrInt(node, "batch_dims", 0);
    size_t rx = x.shape.size(), ri = idx.shape.size();
    if (ri < 1) return Fail("bad indices", op);
    int64_t k = idx.shape[ri - 1];
    if (b + k > static_cast<int64_t>(rx)) return Fail("bad indices depth", op);
    std::vector<int64_t> oshape;
    for (size_t d = 0; d + 1 < ri; ++d) oshape.push_back(idx.shape[d]);
    for (size_t d = static_cast<size_t>(b + k); d < rx; ++d)
      oshape.push_back(x.shape[d]);
    Tensor o = MakeLike(x, oshape.empty() ? Sh(1) : oshape);
    std::vector<int64_t> sx = StridesOf(x.shape);
    int64_t inner = 1;
    for (size_t d = static_cast<size_t>(b + k); d < rx; ++d) inner *= x.shape[d];
    int64_t batch = 1;
    for (int64_t d = 0; d < b; ++d) batch *= idx.shape[d];
    int64_t tuples = 1;
    for (size_t d = static_cast<size_t>(b); d + 1 < ri; ++d)
      tuples *= idx.shape[d];
    int64_t bstride = b > 0 ? sx[b - 1] : 0;
    int64_t di = 0;
    for (int64_t bi = 0; bi < batch; ++bi) {
      for (int64_t t = 0; t < tuples; ++t) {
        int64_t off = bi * bstride;
        for (int64_t e = 0; e < k; ++e) {
          int64_t c = idx.AtInt((bi * tuples + t) * k + e);
          if (c < 0) c += x.shape[b + e];
          off += c * sx[b + e];
        }
        for (int64_t e = 0; e < inner; ++e) CopyElem(x, off + e, &o, di++);
      }
    }
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "ScatterND") {
    if (in.size() < 3 || !in[0] || !in[1] || !in[2])
      return Fail("missing input", op);
    const Tensor &idx = *in[1], &upd = *in[2];
    Tensor o = *in[0];
    size_t ri = idx.shape.size();
    if (ri < 1) return Fail("bad indices", op);
    int64_t k = idx.shape[ri - 1];
    int64_t tuples = 1;
    for (size_t d = 0; d + 1 < ri; ++d) tuples *= idx.shape[d];
    std::vector<int64_t> sx = StridesOf(o.shape);
    int64_t inner = 1;
    for (size_t d = static_cast<size_t>(k); d < o.shape.size(); ++d)
      inner *= o.shape[d];
    for (int64_t t = 0; t < tuples; ++t) {
      int64_t off = 0;
      for (int64_t e = 0; e < k; ++e) {
        int64_t c = idx.AtInt(t * k + e);
        if (c < 0) c += o.shape[e];
        off += c * sx[e];
      }
      for (int64_t e = 0; e < inner; ++e)
        CopyElem(upd, t * inner + e, &o, off + e);
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- quantized conv ---------- */
  if (op == "ConvInteger") {
    if (in.size() < 2 || !in[0] || !in[1]) return Fail("missing input", op);
    Tensor o;
    if (!ConvIntegerOp(*in[0], *in[1], in.size() > 2 ? in[2] : nullptr,
                       in.size() > 3 ? in[3] : nullptr,
                       AttrInts(node, "strides"), AttrInts(node, "pads"),
                       AttrInts(node, "dilations"), AttrInt(node, "group", 1),
                       &o))
      return Fail("conv failed", op);
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- sequences ---------- */
  if (op == "SequenceEmpty") {
    Tensor o;
    o.seq = new std::vector<Tensor>();
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "SequenceInsert") {
    if (in.size() < 2 || !in[0] || !in[0]->seq || !in[1])
      return Fail("bad inputs", op);
    Tensor o = *in[0];  // deep-copies the sequence
    int64_t sz = static_cast<int64_t>(o.seq->size());
    int64_t pos = sz;
    if (in.size() > 2 && in[2]) {
      pos = in[2]->AtInt(0);
      if (pos < 0) pos += sz + 1;
    }
    if (pos < 0 || pos > sz) return Fail("bad position", op);
    o.seq->insert(o.seq->begin() + pos, *in[1]);
    outs->push_back(std::move(o));
    return true;
  }

  if (op == "ConcatFromSequence") {
    if (!in[0] || !in[0]->seq) return Fail("not a sequence", op);
    const std::vector<Tensor> &xs = *in[0]->seq;
    int64_t axis = AttrInt(node, "axis", 0);
    int64_t new_axis = AttrInt(node, "new_axis", 0);
    if (xs.empty()) {
      outs->push_back(Tensor::Float(Sh(0)));
      return true;
    }
    Tensor o;
    if (new_axis) {
      int64_t r = static_cast<int64_t>(xs[0].shape.size()) + 1;
      int64_t a = axis < 0 ? axis + r : axis;
      std::vector<Tensor> tmp = xs;
      std::vector<const Tensor *> ptrs;
      for (size_t k = 0; k < tmp.size(); ++k) {
        tmp[k].shape.insert(tmp[k].shape.begin() + a, 1);
        ptrs.push_back(&tmp[k]);
      }
      ConcatOp(ptrs, a, &o);
    } else {
      std::vector<const Tensor *> ptrs;
      for (size_t k = 0; k < xs.size(); ++k) ptrs.push_back(&xs[k]);
      ConcatOp(ptrs, axis, &o);
    }
    outs->push_back(std::move(o));
    return true;
  }

  /* ---------- control flow ---------- */
  if (op == "If") {
    if (in.empty() || !in[0]) return Fail("missing cond", op);
    const Model::Attr *ab =
        FindAttr(node, in[0]->AtInt(0) ? "then_branch" : "else_branch");
    if (!ab || ab->type != 7 || !ab->g) return Fail("missing branch", op);
    const Graph &g = *ab->g;
    std::map<std::string, Tensor> env;
    if (!RunGraph(g.nodes, g.initializers, g.output_names, &env, scope, outs))
      return Fail("branch failed", op);
    return true;
  }

  if (op == "Loop") {
    const Model::Attr *ab = FindAttr(node, "body");
    if (!ab || ab->type != 7 || !ab->g) return Fail("missing body", op);
    const Graph &body = *ab->g;
    size_t ncar = in.size() > 2 ? in.size() - 2 : 0;
    if (body.input_names.size() < 2 + ncar ||
        body.output_names.size() < 1 + ncar)
      return Fail("bad body signature", op);
    size_t nscan = body.output_names.size() - 1 - ncar;
    bool has_max = in.size() > 0 && in[0] && in[0]->Numel() > 0;
    int64_t max_iter = has_max ? in[0]->AtInt(0) : -1;
    bool cond = true;
    if (in.size() > 1 && in[1] && in[1]->Numel() > 0)
      cond = in[1]->AtInt(0) != 0;
    std::vector<Tensor> carried(ncar);
    for (size_t i = 0; i < ncar; ++i)
      if (in[2 + i]) carried[i] = *in[2 + i];
    std::vector<std::vector<Tensor>> scans(nscan);
    for (int64_t iter = 0; cond && (!has_max || iter < max_iter); ++iter) {
      std::map<std::string, Tensor> env;
      Tensor it = Tensor::Int(std::vector<int64_t>(), kInt64);  // 0-d scalar
      it.i[0] = iter;
      env[body.input_names[0]] = std::move(it);
      Tensor c = Tensor::Byte(std::vector<int64_t>(), kBool);
      c.b[0] = 1;
      env[body.input_names[1]] = std::move(c);
      for (size_t i = 0; i < ncar; ++i)
        env[body.input_names[2 + i]] = carried[i];
      std::vector<Tensor> bouts;
      if (!RunGraph(body.nodes, body.initializers, body.output_names, &env,
                    scope, &bouts))
        return Fail("body failed", op);
      cond = bouts[0].Numel() > 0 ? bouts[0].AtInt(0) != 0 : true;
      for (size_t i = 0; i < ncar; ++i) carried[i] = std::move(bouts[1 + i]);
      for (size_t k = 0; k < nscan; ++k)
        scans[k].push_back(std::move(bouts[1 + ncar + k]));
    }
    for (size_t i = 0; i < ncar; ++i) outs->push_back(std::move(carried[i]));
    for (size_t k = 0; k < nscan; ++k) {
      Tensor st;
      StackTensors(&scans[k], &st);
      outs->push_back(std::move(st));
    }
    return true;
  }

  return Fail("unsupported op", op);
}

/* ============================ executor ============================ */

// record ni as the last reader of every name the node touches, including
// outer-scope names referenced from nested subgraphs (Loop/If bodies);
// over-approximating with locally-produced subgraph names is harmless
static void CollectReads(const Model::Node &node, size_t ni,
                         std::map<std::string, size_t> *last_use) {
  for (size_t k = 0; k < node.input.size(); ++k)
    if (!node.input[k].empty()) (*last_use)[node.input[k]] = ni;
  for (auto it = node.attr.begin(); it != node.attr.end(); ++it) {
    if (it->second.type == 7 && it->second.g) {
      const Graph &g = *it->second.g;
      for (size_t j = 0; j < g.nodes.size(); ++j)
        CollectReads(g.nodes[j], ni, last_use);
    }
  }
}

static bool RunGraph(const std::vector<Model::Node> &nodes,
                     const std::map<std::string, Tensor> &inits,
                     const std::vector<std::string> &output_names,
                     std::map<std::string, Tensor> *env, const Scope *parent,
                     std::vector<Tensor> *outputs) {
  Scope scope;
  scope.env = env;
  scope.inits = &inits;
  scope.parent = parent;

  // last node index that reads each intermediate, so it can be freed early
  std::map<std::string, size_t> last_use;
  for (size_t ni = 0; ni < nodes.size(); ++ni)
    CollectReads(nodes[ni], ni, &last_use);
  for (size_t i = 0; i < output_names.size(); ++i)
    last_use[output_names[i]] = nodes.size();  // keep graph outputs

  for (size_t ni = 0; ni < nodes.size(); ++ni) {
    const Model::Node &node = nodes[ni];
    std::vector<const Tensor *> ins(node.input.size(), nullptr);
    for (size_t k = 0; k < node.input.size(); ++k) {
      const std::string &name = node.input[k];
      if (name.empty()) continue;
      ins[k] = ScopeLookup(&scope, name);
      if (!ins[k]) {
        fprintf(stderr, "sonnx: missing input %s for node %s (%s)\n",
                name.c_str(), node.op.c_str(),
                node.output.empty() ? "" : node.output[0].c_str());
        return false;
      }
    }

    std::vector<Tensor> result;
    if (!ExecNode(node, ins, &result, &scope)) {
      fprintf(stderr, "sonnx: failed to execute node %d: %s\n",
              static_cast<int>(ni), node.op.c_str());
      return false;
    }

    for (size_t k = 0; k < node.output.size(); ++k) {
      if (node.output[k].empty()) continue;
      if (k < result.size()) {
        (*env)[node.output[k]] = std::move(result[k]);
      } else {
        (*env)[node.output[k]] = Tensor();
      }
    }

    // free local intermediates whose last consumer was this node
    for (size_t k = 0; k < node.input.size(); ++k) {
      const std::string &name = node.input[k];
      if (name.empty()) continue;
      auto lu = last_use.find(name);
      if (lu != last_use.end() && lu->second == ni) env->erase(name);
    }
  }

  outputs->clear();
  for (size_t i = 0; i < output_names.size(); ++i) {
    auto it = env->find(output_names[i]);
    if (it != env->end()) {
      outputs->push_back(std::move(it->second));
      continue;
    }
    const Tensor *t = ScopeLookup(&scope, output_names[i]);
    if (!t) {
      fprintf(stderr, "sonnx: missing output %s\n", output_names[i].c_str());
      return false;
    }
    outputs->push_back(*t);
  }
  return true;
}

bool Model::Run(const std::vector<const Tensor *> &inputs,
                std::vector<Tensor> *outputs) const {
  if (inputs.size() != input_names_.size()) {
    fprintf(stderr, "sonnx: expected %d inputs, got %d\n", NumInputs(),
            static_cast<int>(inputs.size()));
    return false;
  }

  // intermediates live in env; weights are borrowed from initializers_
  // (never copied) to keep the peak footprint near the model size
  std::map<std::string, Tensor> env;
  for (size_t i = 0; i < inputs.size(); ++i) {
    env[input_names_[i]] = *inputs[i];
  }
  return RunGraph(nodes_, initializers_, output_names_, &env, nullptr,
                  outputs);
}

std::vector<int64_t> Model::InputShape(int index) const {
  if (index < 0 || index >= static_cast<int>(input_shapes_.size())) return {};
  return input_shapes_[index];
}

}  // namespace sonnx
namespace sonnx {

/* ============================ protobuf wire parser ============================ */

struct Reader {
  const uint8_t *p = nullptr;
  size_t len = 0;
  size_t pos = 0;

  Reader() = default;
  Reader(const uint8_t *p_, size_t len_) : p(p_), len(len_) {}

  bool eof() const { return pos >= len; }

  uint64_t varint() {
    uint64_t r = 0;
    int s = 0;
    while (pos < len) {
      uint8_t b = p[pos++];
      r |= static_cast<uint64_t>(b & 0x7f) << s;
      if (!(b & 0x80)) break;
      s += 7;
    }
    return r;
  }

  uint32_t fixed32() {
    uint32_t r = 0;
    if (pos + 4 <= len) memcpy(&r, p + pos, 4);
    pos += 4;
    return r;
  }

  uint64_t fixed64() {
    uint64_t r = 0;
    if (pos + 8 <= len) memcpy(&r, p + pos, 8);
    pos += 8;
    return r;
  }

  Reader sub() {
    uint64_t n = varint();
    if (n > len - pos) n = len - pos;
    Reader r(p + pos, static_cast<size_t>(n));
    pos += static_cast<size_t>(n);
    return r;
  }

  std::string str() {
    uint64_t n = varint();
    if (n > len - pos) n = len - pos;
    std::string s(reinterpret_cast<const char *>(p + pos),
                  static_cast<size_t>(n));
    pos += static_cast<size_t>(n);
    return s;
  }

  void skip(int wire) {
    switch (wire) {
      case 0:
        varint();
        break;
      case 1:
        pos += 8;
        break;
      case 2: {
        uint64_t n = varint();
        pos += static_cast<size_t>(n);
        break;
      }
      case 5:
        pos += 4;
        break;
      default:
        pos = len;  // bail out
        break;
    }
    if (pos > len) pos = len;
  }

  bool next(int *field, int *wire) {
    if (eof()) return false;
    uint64_t t = varint();
    *field = static_cast<int>(t >> 3);
    *wire = static_cast<int>(t & 7);
    return true;
  }
};

static void ParsePackedVarints(Reader *r, int wire,
                               std::vector<int64_t> *out) {
  if (wire == 2) {
    Reader s = r->sub();
    while (!s.eof()) out->push_back(static_cast<int64_t>(s.varint()));
  } else {
    out->push_back(static_cast<int64_t>(r->varint()));
  }
}

static void ParsePackedFloats(Reader *r, int wire, std::vector<float> *out) {
  if (wire == 2) {
    Reader s = r->sub();
    while (!s.eof()) {
      uint32_t u = s.fixed32();
      float f;
      memcpy(&f, &u, 4);
      out->push_back(f);
    }
  } else {
    uint32_t u = r->fixed32();
    float f;
    memcpy(&f, &u, 4);
    out->push_back(f);
  }
}

// TensorProto: dims=1, data_type=2, float_data=4, int32_data=5,
// string_data=6, int64_data=7, name=8, raw_data=9, double_data=10.
static void ParseTensorMsg(Reader *r, Tensor *t, std::string *name) {
  int dtype = 0;
  std::vector<int64_t> dims;
  std::vector<float> fdata;
  std::vector<int64_t> idata;
  /* raw payload stays a view into the file buffer (alive for the whole
     parse); the old std::string copy doubled the transient footprint of
     every weight tensor */
  const uint8_t *rawp = nullptr;
  size_t rawn = 0;

  int field, wire;
  while (r->next(&field, &wire)) {
    switch (field) {
      case 1:
        ParsePackedVarints(r, wire, &dims);
        break;
      case 2:
        dtype = static_cast<int>(r->varint());
        break;
      case 4:
        ParsePackedFloats(r, wire, &fdata);
        break;
      case 5:
        ParsePackedVarints(r, wire, &idata);
        break;
      case 7:
        ParsePackedVarints(r, wire, &idata);
        break;
      case 8:
        *name = r->str();
        break;
      case 9: {
        uint64_t n = r->varint();
        if (n > r->len - r->pos) n = r->len - r->pos;
        rawp = r->p + r->pos;
        rawn = static_cast<size_t>(n);
        r->pos += static_cast<size_t>(n);
        break;
      }
      default:
        r->skip(wire);
        break;
    }
  }

  t->shape = std::move(dims);
  int64_t numel = t->Numel();

  if (rawp != nullptr) {
    const uint8_t *d = rawp;
    if (dtype == kFloat) {
      t->dtype = kFloat;
      ResizeExact(t->f, static_cast<size_t>(numel));
      size_t bytes = static_cast<size_t>(numel) * 4;
      if (bytes > rawn) bytes = rawn / 4 * 4;
      memcpy(t->pf(), d, bytes);
    } else if (dtype == kInt64 || dtype == kInt32) {
      t->dtype = dtype;
      ResizeExact(t->i, static_cast<size_t>(numel));
      if (dtype == kInt64) {
        for (int64_t i = 0; i < numel; ++i) {
          int64_t v = 0;
          if (static_cast<size_t>(i + 1) * 8 <= rawn)
            memcpy(&v, d + i * 8, 8);
          t->i[i] = v;
        }
      } else {
        for (int64_t i = 0; i < numel; ++i) {
          int32_t v = 0;
          if (static_cast<size_t>(i + 1) * 4 <= rawn)
            memcpy(&v, d + i * 4, 4);
          t->i[i] = v;
        }
      }
    } else if (dtype == kInt8 || dtype == kUint8 || dtype == kBool) {
      t->dtype = dtype;
      ResizeExact(t->b, static_cast<size_t>(numel));
      size_t ncp = rawn < static_cast<size_t>(numel) ? rawn
                                                     : static_cast<size_t>(numel);
      memcpy(t->pb(), d, ncp);
      if (ncp < static_cast<size_t>(numel))
        memset(t->pb() + ncp, 0, static_cast<size_t>(numel) - ncp);
    } else {
      // unsupported dtype, keep raw as floats zero
      t->dtype = kFloat;
      t->f.assign(static_cast<size_t>(numel), 0.0f);
    }
    return;
  }

  if (dtype == kFloat || (!fdata.empty() && idata.empty())) {
    t->dtype = kFloat;
    t->f = std::move(fdata);
    if (static_cast<int64_t>(t->f.size()) < numel)
      t->f.resize(static_cast<size_t>(numel), 0.0f);
  } else if (dtype == kInt8 || dtype == kUint8 || dtype == kBool) {
    t->dtype = dtype;
    t->b.resize(static_cast<size_t>(numel), 0);
    size_t ncp = idata.size() < static_cast<size_t>(numel)
                     ? idata.size()
                     : static_cast<size_t>(numel);
    for (size_t i = 0; i < ncp; ++i)
      t->b[i] = static_cast<uint8_t>(idata[i] & 0xff);
  } else {
    t->dtype = dtype == 0 ? kInt64 : dtype;
    t->i = std::move(idata);
    if (static_cast<int64_t>(t->i.size()) < numel)
      t->i.resize(static_cast<size_t>(numel), 0);
  }
}

// AttributeProto: name=1, f=2, i=3, s=4, t=5, g=6, floats=7, ints=8.
static void ParseGraphMsg(Reader *r, Graph *graph,
                          std::vector<std::vector<int64_t>> *shapes,
                          bool top_level);  // fwd (subgraph attrs recurse)
static void ParseAttrMsg(Reader *r, Model::Attr *a, std::string *name) {
  int field, wire;
  while (r->next(&field, &wire)) {
    switch (field) {
      case 1:
        *name = r->str();
        break;
      case 2: {
        uint32_t u = r->fixed32();
        memcpy(&a->f, &u, 4);
        a->type = 2;
        break;
      }
      case 3:
        a->i = static_cast<int64_t>(r->varint());
        a->type = 1;
        break;
      case 4:
        a->s = r->str();
        a->type = 3;
        break;
      case 5: {
        Reader s = r->sub();
        std::string tname;
        ParseTensorMsg(&s, &a->t, &tname);
        a->type = 4;
        break;
      }
      case 6: {
        Reader s = r->sub();
        a->g = new Graph();
        ParseGraphMsg(&s, a->g, nullptr, false);
        a->type = 7;
        break;
      }
      case 7:
        ParsePackedFloats(r, wire, &a->floats);
        a->type = 6;
        break;
      case 8:
        ParsePackedVarints(r, wire, &a->ints);
        a->type = 5;
        break;
      default:
        r->skip(wire);
        break;
    }
  }
}

// NodeProto: input=1, output=2, name=3, op_type=4, attribute=5, domain=7.
static void ParseNodeMsg(Reader *r, Model::Node *node) {
  int field, wire;
  while (r->next(&field, &wire)) {
    switch (field) {
      case 1:
        node->input.push_back(r->str());
        break;
      case 2:
        node->output.push_back(r->str());
        break;
      case 4:
        node->op = r->str();
        break;
      case 5: {
        Reader s = r->sub();
        Model::Attr a;
        std::string name;
        ParseAttrMsg(&s, &a, &name);
        if (!name.empty()) node->attr[name] = std::move(a);
        break;
      }
      default:
        r->skip(wire);
        break;
    }
  }
}

// ValueInfoProto: name=1, type=2 -> TypeProto: tensor_type=1 ->
//   TypeProto.Tensor: elem_type=1, shape=2 -> TensorShapeProto: dim=1 ->
//     Dimension: dim_value=1, dim_param=2.
static void ParseValueInfoMsg(Reader *r, std::string *name,
                              std::vector<int64_t> *shape) {
  int field, wire;
  while (r->next(&field, &wire)) {
    if (field == 1) {
      *name = r->str();
    } else if (field == 2) {
      Reader tp = r->sub();
      int f2, w2;
      while (tp.next(&f2, &w2)) {
        if (f2 != 1) {
          tp.skip(w2);
          continue;
        }
        Reader tt = tp.sub();
        int f3, w3;
        while (tt.next(&f3, &w3)) {
          if (f3 != 2) {
            tt.skip(w3);
            continue;
          }
          Reader sh = tt.sub();
          int f4, w4;
          while (sh.next(&f4, &w4)) {
            if (f4 != 1) {
              sh.skip(w4);
              continue;
            }
            Reader dim = sh.sub();
            int f5, w5;
            int64_t dv = -1;
            while (dim.next(&f5, &w5)) {
              if (f5 == 1) {
                dv = static_cast<int64_t>(dim.varint());
              } else {
                dim.skip(w5);
              }
            }
            shape->push_back(dv);
          }
        }
      }
    } else {
      r->skip(wire);
    }
  }
}

// GraphProto: node=1, name=2, initializer=5, input=11, output=12.
// Fills a Graph; used for the top-level graph and nested subgraphs
// (attribute field g). shapes (optional) receives external input shapes;
// top_level enables load-progress reporting.
// collect every name a node reads, including nested subgraph reads
static void CollectReadNames(const Node &node,
                             std::map<std::string, int> *names) {
  for (size_t k = 0; k < node.input.size(); ++k)
    if (!node.input[k].empty()) (*names)[node.input[k]] = 1;
  for (auto it = node.attr.begin(); it != node.attr.end(); ++it) {
    if (it->second.type == 7 && it->second.g) {
      const Graph &g = *it->second.g;
      for (size_t j = 0; j < g.nodes.size(); ++j)
        CollectReadNames(g.nodes[j], names);
    }
  }
}

// Exporters may emit nodes out of order with respect to subgraph
// outer-scope references (a Loop body reading a value produced by a later
// top-level node); onnxruntime re-sorts nodes, so do the same here.
static void TopoSortGraph(Graph *graph) {
  size_t n = graph->nodes.size();
  if (n < 2) return;

  std::map<std::string, size_t> producer;
  for (size_t i = 0; i < n; ++i)
    for (size_t k = 0; k < graph->nodes[i].output.size(); ++k)
      if (!graph->nodes[i].output[k].empty())
        producer[graph->nodes[i].output[k]] = i;

  std::vector<std::vector<size_t>> succ(n);
  std::vector<int64_t> indeg(n, 0);
  for (size_t i = 0; i < n; ++i) {
    std::map<std::string, int> reads;
    CollectReadNames(graph->nodes[i], &reads);
    std::map<size_t, int> deps;  // dedupe producer edges
    for (auto it = reads.begin(); it != reads.end(); ++it) {
      auto p = producer.find(it->first);
      if (p != producer.end() && p->second != i) deps[p->second] = 1;
    }
    for (auto d = deps.begin(); d != deps.end(); ++d) {
      succ[d->first].push_back(i);
      ++indeg[i];
    }
  }

  // Kahn's algorithm, stable (smallest original index first)
  std::vector<size_t> order;
  order.reserve(n);
  std::map<size_t, int> ready;
  for (size_t i = 0; i < n; ++i)
    if (indeg[i] == 0) ready[i] = 1;
  while (!ready.empty()) {
    size_t i = ready.begin()->first;
    ready.erase(ready.begin());
    order.push_back(i);
    for (size_t k = 0; k < succ[i].size(); ++k)
      if (--indeg[succ[i][k]] == 0) ready[succ[i][k]] = 1;
  }
  if (order.size() != n) return;  // cycle: keep original order

  bool sorted = true;
  for (size_t i = 0; i < n; ++i) {
    if (order[i] != i) {
      sorted = false;
      break;
    }
  }
  if (sorted) return;

  std::vector<Node> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i)
    out.push_back(std::move(graph->nodes[order[i]]));
  vswap(graph->nodes, out);
}

static void ParseGraphMsg(Reader *r, Graph *graph,
                          std::vector<std::vector<int64_t>> *shapes,
                          bool top_level) {
  std::vector<std::pair<std::string, std::vector<int64_t>>> graph_inputs;
  int field, wire;
  while (r->next(&field, &wire)) {
    /* parse phase covers 95..100% of the load progress */
    if (top_level && r->len > 0)
      g_load_progress = 95 + static_cast<int>((r->pos * 5) / r->len);
    switch (field) {
      case 1: {
        Reader s = r->sub();
        Node node;
        ParseNodeMsg(&s, &node);
        graph->nodes.push_back(std::move(node));
        break;
      }
      case 5: {
        Reader s = r->sub();
        /* Pre-scan the tensor name (field 8) so the payload can be parsed
           straight into its map slot. Going through a local Tensor and
           std::move deep-copies every weight on ewokstl (its vectors have
           no move semantics), doubling both load time and peak memory. */
        Reader scan = s;
        std::string name;
        int sf, sw;
        while (scan.next(&sf, &sw)) {
          if (sf == 8) {
            name = scan.str();
            break;
          }
          scan.skip(sw);
        }
        if (!name.empty()) {
          std::string dummy;
          ParseTensorMsg(&s, &graph->initializers[name], &dummy);
        }
        break;
      }
      case 11: {
        Reader s = r->sub();
        std::string name;
        std::vector<int64_t> shape;
        ParseValueInfoMsg(&s, &name, &shape);
        if (!name.empty()) {
          graph_inputs.push_back(std::make_pair(name, std::move(shape)));
        }
        break;
      }
      case 12: {
        Reader s = r->sub();
        std::string name;
        std::vector<int64_t> shape;
        ParseValueInfoMsg(&s, &name, &shape);
        if (!name.empty()) graph->output_names.push_back(name);
        break;
      }
      default:
        r->skip(wire);
        break;
    }
  }

  // external inputs = graph inputs that are not initializers
  for (size_t i = 0; i < graph_inputs.size(); ++i) {
    if (graph->initializers.find(graph_inputs[i].first) ==
        graph->initializers.end()) {
      graph->input_names.push_back(graph_inputs[i].first);
      if (shapes) shapes->push_back(graph_inputs[i].second);
    }
  }

  TopoSortGraph(graph);
}

// StringStringEntryProto: key=1, value=2.
static void ParseMetaEntryMsg(Reader *r, Model *m) {
  int field, wire;
  std::string key, value;
  while (r->next(&field, &wire)) {
    if (field == 1 && wire == 2) {
      key = r->str();
    } else if (field == 2 && wire == 2) {
      value = r->str();
    } else {
      r->skip(wire);
    }
  }
  if (!key.empty()) m->metadata_[key] = value;
}

// ModelProto: ir_version=1, graph=7, opset_import=8, metadata_props=14.
bool Model::Parse(const uint8_t *data, size_t len) {
  Reader r(data, len);
  int field, wire;
  bool got_graph = false;
  while (r.next(&field, &wire)) {
    if (field == 7 && wire == 2) {
      Reader g = r.sub();
      Graph graph;
      ParseGraphMsg(&g, &graph, &input_shapes_, true);
      nodes_ = std::move(graph.nodes);
      /* swap, not move: ewokstl containers deep-copy on "move", and the
         initializer map holds the entire model weights */
      initializers_.swap(graph.initializers);
      input_names_ = std::move(graph.input_names);
      output_names_ = std::move(graph.output_names);
      got_graph = true;
    } else if (field == 14 && wire == 2) {
      Reader e = r.sub();
      ParseMetaEntryMsg(&e, this);
    } else {
      r.skip(wire);
    }
  }

  if (!got_graph) {
    fprintf(stderr, "sonnx: no graph found in model\n");
    return false;
  }
  return true;
}

bool Model::Load(const char *path) {
  g_load_progress = 0;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "sonnx: cannot open %s\n", path);
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(fp);
    fprintf(stderr, "sonnx: empty model file %s\n", path);
    return false;
  }
  /* exact-size malloc: ewokstl's sized vector ctor allocates 2x the
     requested capacity, which alone doubled the biggest allocation of
     the whole load on target */
  uint8_t *buf = static_cast<uint8_t *>(malloc(static_cast<size_t>(sz)));
  if (!buf) {
    fclose(fp);
    fprintf(stderr, "sonnx: out of memory loading %s\n", path);
    return false;
  }
  /* chunked read so the file phase reports 0..95% progress;
     reading dominates load time on SD-based targets, parsing
     (memcpy-bound) is only a small fraction */
  size_t n = 0;
  while (n < static_cast<size_t>(sz)) {
    size_t want = static_cast<size_t>(sz) - n;
    if (want > (1u << 20)) want = 1u << 20;
    size_t got = fread(buf + n, 1, want, fp);
    if (got == 0) break;
    n += got;
    g_load_progress = static_cast<int>((n * 95) / static_cast<size_t>(sz));
  }
  fclose(fp);
  if (n != static_cast<size_t>(sz)) {
    free(buf);
    fprintf(stderr, "sonnx: short read on %s\n", path);
    return false;
  }
  bool ok = Parse(buf, static_cast<size_t>(sz));
  free(buf);
  if (!ok) return false;
  g_load_progress = 100;

  fprintf(stderr, "sonnx: loaded %s: %d nodes, %d initializers, %d inputs, %d outputs\n",
          path, static_cast<int>(nodes_.size()),
          static_cast<int>(initializers_.size()), NumInputs(), NumOutputs());
  return true;
}

}  // namespace sonnx
