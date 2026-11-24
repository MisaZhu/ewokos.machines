/*
 * nv_backend.h - C bridge between the shared C++ GLSL->NIR frontend and the
 * GA10B (sm_87) NIR->SASS compiler (nv_sass encoder + nv_codegen lowering).
 *
 * Mirrors machines/qcom/8gen3/.../adreno_backend.h: the frontend (pglcompiler)
 * produces linked compute NIR; this backend lowers it and emits sm_87 SASS.
 * Only nir.h crosses the boundary (C++-safe); the SASS encoder (nv_sass.h) is
 * plain C and never exposed to the C++ frontend TU.
 *
 * The compile unit is one GLSL ES 3.10 compute shader -> one GA10B compute
 * kernel (C7C0 class, QMD v3.0 launch).  GA10B has no graphics-pipeline port
 * here; both the shadertoy fullscreen route and the geometry rasterizer are
 * compute, exactly like the Adreno back end.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_BACKEND_H
#define NV_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Result of one compute compile: the assembled sm_87 SASS (malloc()ed u32
 * words, 4 per instruction, owned by the result until nv_be_result_free() or
 * stolen by NULLing the pointer) plus launch metadata. */
struct nv_be_result {
    uint32_t *bin;            /* SASS instruction words */
    uint32_t  bin_words;      /* = 4 * num_instructions */
    struct nv_kernel_meta meta;
};

/* nir_shader_compiler_options for the sm_87 target: the frontend lowers
 * against these.  NULL if the compiler cannot be initialised. */
const struct nir_shader_compiler_options *nv_be_nir_options(void);

/* Compile a linked compute NIR (uniforms -> load_uniform byte offsets, SSBOs
 * -> linked-block load/store_ssbo, samplers -> indices) to sm_87 SASS.  `cs`
 * is NOT modified: the backend clones it.  On failure returns false with a
 * message in log. */
bool nv_be_compile_compute(nir_shader *cs, struct nv_be_result *res,
                           char *log, size_t logsz);

void nv_be_result_free(struct nv_be_result *res);

#ifdef __cplusplus
}
#endif

#endif /* NV_BACKEND_H */