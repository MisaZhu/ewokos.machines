/*
 * adreno_backend.h - C bridge between the shared C++ GLSL->NIR frontend
 * (driven by adreno_pglc.cpp) and mesa's freedreno/ir3 compiler.
 *
 * WHY A BRIDGE: ir3_compiler.h / ir3_shader.h are C headers that are not
 * C++-clean (anonymous unions with designated initialisers, `enum` arithmetic,
 * flexible arrays), so the C++ TU that owns the frontend never includes them.
 * It sees only this header: nir.h (C++-safe) + plain structs.
 *
 * The compile unit is one GLSL ES 3.10 compute shader -> one a6xx compute
 * kernel (Adreno has no graphics-pipeline port here; both the shadertoy
 * fullscreen route and the two-pass geometry rasterizer are compute).
 */
#ifndef ADRENO_BACKEND_H
#define ADRENO_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler/nir/nir.h"
#include "adreno_pglc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of one compute compile: the assembled a6xx binary (malloc()ed u32
 * words, owned by the result until adreno_be_result_free() or until the
 * caller steals it by NULLing the pointer) plus the snapshot adreno_blob.c
 * serializes. */
struct adreno_be_result {
   uint32_t *bin;
   uint32_t  bin_words;
   struct adreno_pglc_stage st;
};

/* The ir3 compiler's nir_shader_compiler_options for the A690 target: the
 * frontend lowers against these.  Creates the (process-wide, lazily built)
 * ir3_compiler on first use; NULL if that fails. */
const struct nir_shader_compiler_options *adreno_be_nir_options(void);

/* Compile a linked compute NIR (as produced by pglc_fe_compile_compute: uniforms
 * lowered to byte-offset load_uniform, SSBOs to linked-block load/store_ssbo,
 * samplers to indices) to an a6xx kernel.  `cs` is NOT modified: the backend
 * clones it (ir3_finalize_nir strips the uniform variables the caller still
 * reflects by name).  On failure returns false with a message in log. */
bool adreno_be_compile_compute(nir_shader *cs, struct adreno_be_result *res,
                               char *log, size_t logsz);

void adreno_be_result_free(struct adreno_be_result *res);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_BACKEND_H */
