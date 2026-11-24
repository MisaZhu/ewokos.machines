/*
 * nv_ref_math.h - float semantics shared by the SASS emulator and the NIR
 * reference interpreter, so a differential run compares code generation and
 * not two libm call sequences.  Host tests only.
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_REF_MATH_H
#define NV_REF_MATH_H

#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline float ref_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t ref_f2u_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* MUFU.SIN/COS take the angle in turns */
#define REF_INV_2PI_BITS 0x3e22f983u
static inline float ref_sin_turns(float t) { return sinf(t * 6.2831855f); }
static inline float ref_cos_turns(float t) { return cosf(t * 6.2831855f); }

static inline float ref_fsat(float x) {
    if (x != x) return 0.0f;
    if (x <= 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float ref_fmin(float a, float b) {
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}

static inline float ref_fmax(float a, float b) {
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}

/* FADD under an explicit rounding mode: 0 nearest, 1 down, 2 up, 3 to zero */
static inline float ref_fadd(float a, float b, int rnd) {
    static const int mode[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    volatile float x = a, y = b, r;
    if (rnd == 0)
        return a + b;
    fesetround(mode[rnd & 3]);
    r = x + y;
    fesetround(FE_TONEAREST);
    return r;
}

static inline float ref_frcp(float x) { return 1.0f / x; }
static inline float ref_frsq(float x) { return 1.0f / sqrtf(x); }

/* rnd: 0 nearest-even, 1 floor, 2 ceil, 3 trunc */
static inline float ref_frnd(float x, int rnd) {
    switch (rnd) {
    case 1: return floorf(x);
    case 2: return ceilf(x);
    case 3: return truncf(x);
    default: return rintf(x);
    }
}

static inline int32_t ref_f2i32(float x) {
    if (x != x) return 0;
    if (x >= 2147483648.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return (int32_t)x;
}

static inline uint32_t ref_f2u32(float x) {
    if (!(x > 0.0f)) return 0;
    if (x >= 4294967296.0f) return 0xffffffffu;
    return (uint32_t)x;
}

#endif /* NV_REF_MATH_H */
