/*
 * nv_pglc.h - GA10B backend-private per-stage snapshot hung off
 * pglc_stage.backend.
 *
 * Produced by nv_pglc.cpp (the compile half) and consumed only by nv_blob.c
 * (pglc_program_serialize), which lays it out into the ga10b wire blob the
 * Orin g2dd daemon (bsp_g3d_program -> ga10b_pgl.c) parses.  portablegl never
 * sees this type: it reaches the stage through the neutral pglc_stage fields.
 *
 * Two kinds of compute stage exist on this backend:
 *   - a precompiled geometry twin (kernel_id != 0): the fixed geom_raster_cs.h
 *     texts mapped onto nvcc kernels embedded in the driver; pglc_stage.code
 *     holds the single id word, exactly as before.
 *   - a real on-device compile (kernel_id == 0): nv_be_compile_compute emitted
 *     sm_87 SASS (pglc_stage.code / code_words) and nv_cubin_build wrapped it
 *     into a cubin the ga10b_dev module loader consumes; `cubin` owns that ELF.
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_PGLC_H
#define NV_PGLC_H

#include <stdint.h>
#include "nv_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nv_pglc_stage {
    uint32_t kernel_id;        /* GA10B_PGL_K_* twin id, 0 for a real compile */
    uint32_t tex_mask;         /* bit i: kernel samples ctex unit i (nv_tex.c) */
    struct nv_kernel_meta meta;
    uint8_t* cubin;            /* synthesized cubin ELF (malloc'd), or NULL */
    uint32_t cubin_size;
};

#ifdef __cplusplus
}
#endif

#endif /* NV_PGLC_H */