/*
 * nv_meta.h - pure-data kernel launch metadata shared by the NIR->SASS backend
 * (nv_backend.h) and the cubin synthesizer (nv_cubin.h).  No NIR dependency, so
 * the synthesizer and the ga10b launch path can use it without the frontend.
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_META_H
#define NV_META_H

#include <stdint.h>

/* Constant-bank-0 layout for compiled compute kernels, shared by the backend
 * (load_uniform bias), the cubin synthesizer (cb0 size) and the daemon launch
 * (ga10b_kernel_launch_compute).  The QMD launch state owns the head:
 *   [0x00] blockDim.xyz  [0x0c] gridDim.xyz  [0x28] stack pointer
 * Uniforms (frontend load_uniform dword offsets, biased by NV_UNIFORM_BASE_OFF)
 * follow; SSBO base VAs sit at NV_SSBO_BASE_OFF.  Uniforms must stay below the
 * SSBO region (NV_SSBO_BASE_OFF - NV_UNIFORM_BASE_OFF bytes). */
#define NV_UNIFORM_BASE_OFF 0x40u
#define NV_SSBO_BASE_OFF    0x200u
#define NV_MAX_SSBO         8u
#define NV_CB0_SIZE         (NV_SSBO_BASE_OFF + NV_MAX_SSBO * 8u)  /* 0x240 */

/* Kernel metadata the QMD builder / blob serializer needs (mirrors the fields
 * cubin_kernel carries for nvcc kernels, so the ga10b launch path is shared). */
struct nv_kernel_meta {
    uint32_t num_gprs;        /* GPRs per thread (QMD REGISTER_COUNT_V, raw) */
    uint32_t num_barriers;    /* scoreboards used (QMD barrier count) */
    uint32_t shared_bytes;    /* static shared memory */
    uint32_t local_bytes;     /* per-thread local memory (frame + stack) */
    uint32_t local_size[3];   /* workgroup shape */
    uint32_t param_base;      /* cbank0 byte offset of the param window */
    uint32_t param_bytes;     /* total kernel param bytes */
    uint32_t num_ubos;        /* constant banks bound (0 = uniforms in cb0) */
};

#endif /* NV_META_H */