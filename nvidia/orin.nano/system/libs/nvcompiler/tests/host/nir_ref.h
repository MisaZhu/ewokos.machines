/*
 * nir_ref.h - host-only reference interpreter for the lowered compute NIR the
 * GA10B backend generates code from.  One thread at a time, no warp model: it
 * is the oracle the SASS emulator's output is compared against.
 * SPDX-License-Identifier: MIT
 */
#ifndef NIR_REF_H
#define NIR_REF_H

#include <stddef.h>
#include <stdint.h>

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NIR_REF_MAX_SSBO 8

struct nir_ref_env {
    const uint8_t* uniforms;       /* uniform blob (load_uniform byte offsets) */
    uint32_t uniform_bytes;
    struct { uint8_t* p; uint64_t size; } ssbo[NIR_REF_MAX_SSBO];
    uint32_t num_workgroups[3];
};

struct nir_ref;

struct nir_ref* nir_ref_create(nir_shader* nir, const struct nir_ref_env* env);
void nir_ref_destroy(struct nir_ref* r);

/* Run one invocation.  0 on success, -1 on an unsupported construct (err). */
int nir_ref_run(struct nir_ref* r, const uint32_t workgroup[3],
                const uint32_t local[3], char* err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* NIR_REF_H */
