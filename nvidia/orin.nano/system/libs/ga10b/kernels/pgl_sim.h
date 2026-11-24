/* pgl_sim.h - compile pgl_geom.cu as plain C: the CPU reference the
 * on-board self-test and host tests compare the GPU result against.  The
 * caller drives pgl_sim_blockIdx/threadIdx/blockDim per invocation. */
#ifndef PGL_SIM_H
#define PGL_SIM_H

/* no libm dependency: the driver links libc only */
static inline float pgl_sim_floorf(float v)
{
    float t = (float)(int)v;
    return t > v ? t - 1.0f : t;
}
static inline float pgl_sim_ceilf(float v)
{
    float t = (float)(int)v;
    return t < v ? t + 1.0f : t;
}
static inline float pgl_sim_fminf(float a, float b) { return a < b ? a : b; }
static inline float pgl_sim_fmaxf(float a, float b) { return a > b ? a : b; }
/* sqrt without libm: the FPU instruction where there is one */
static inline float pgl_sim_sqrtf(float v)
{
#if defined(__aarch64__)
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(v));
    return r;
#else
    float r = v > 1.0f ? v : 1.0f;
    for (int i = 0; i < 24; i++)
        r = 0.5f * (r + v / r);
    return r;
#endif
}
#define sqrtf  pgl_sim_sqrtf
#define floorf pgl_sim_floorf
#define ceilf  pgl_sim_ceilf
#define fminf  pgl_sim_fminf
#define fmaxf  pgl_sim_fmaxf

struct pgl_sim_dim3 { unsigned x, y, z; };
extern struct pgl_sim_dim3 pgl_sim_threadIdx, pgl_sim_blockIdx,
                           pgl_sim_blockDim;

#define threadIdx pgl_sim_threadIdx
#define blockIdx  pgl_sim_blockIdx
#define blockDim  pgl_sim_blockDim
#define __device__ inline
#define PGL_KERNEL

static inline unsigned atomicMin(unsigned *p, unsigned v)
{
    unsigned old = *p;
    if (v < old)
        *p = v;
    return old;
}

static inline unsigned atomicAdd(unsigned *p, unsigned v)
{
    unsigned old = *p;
    *p = old + v;
    return old;
}

static inline unsigned atomicCAS(unsigned *p, unsigned cmp, unsigned v)
{
    unsigned old = *p;
    if (old == cmp)
        *p = v;
    return old;
}

#endif
