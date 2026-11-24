/*
 * nv_cubin.h - synthesize a minimal NVIDIA cubin (CUDA ELF64) from compiled
 * sm_87 SASS + launch metadata, so the existing ga10b / projects-cuda cubin
 * loader + QMD launch path consumes compiler output unchanged.
 *
 * The emitted ELF carries exactly the sections cubin_parse (projects/cuda/
 * libs/cuda/src/cubin.c) reads: .text.<name> (SASS), .nv.info[.<name>]
 * (REGCOUNT / PARAM_CBANK / KPARAM_INFO / EXIT offsets), .nv.constant0.<name>,
 * .symtab/.strtab/.shstrtab, and .nv.shared.<name> when shared memory is used.
 * Verified on host against cubin_parse and nvdisasm.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_CUBIN_H
#define NV_CUBIN_H

#include <stdint.h>
#include <stddef.h>
#include "nv_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One kernel parameter in the cbank0 param window (mirrors cubin_param). */
struct nv_cubin_param {
    uint16_t ordinal;
    uint16_t offset;   /* byte offset within the param window */
    uint16_t size;     /* bytes */
};

/* Build a single-kernel cubin.  sass/sass_words is the instruction stream
 * (4 u32 per instruction); meta supplies regcount / shared / local / param
 * layout; params[nparams] describes the cbank0 param window; cbank0 is the
 * initial constant-bank-0 image (may be NULL when cbank0_size==0).  Writes the
 * ELF into out (cap bytes); returns the total byte size, or 0 on overflow /
 * bad input.  out==NULL returns the required size. */
uint32_t nv_cubin_build(const uint32_t* sass, uint32_t sass_words,
                        const struct nv_kernel_meta* meta, const char* name,
                        const struct nv_cubin_param* params, uint32_t nparams,
                        const uint8_t* cbank0, uint32_t cbank0_size,
                        uint8_t* out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NV_CUBIN_H */