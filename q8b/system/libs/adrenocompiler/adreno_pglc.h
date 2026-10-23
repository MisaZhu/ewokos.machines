/*
 * adreno_pglc.h - Adreno backend-private per-stage snapshot hung off
 * pglc_stage.backend.
 *
 * Produced by adreno_backend.c (the ir3 bridge) from one compiled
 * ir3_shader_variant, moved into pglc_stage by adreno_pglc.cpp, and consumed
 * only by adreno_blob.c (pglc_program_serialize), which lays the fields out
 * into the adreno_prog_blob_t header + payload the q8b g2dd daemon
 * (bsp_g3d_program -> adreno_3d.c) parses.  portablegl never sees this type:
 * it reaches the stage through the neutral pglc_stage fields only.
 *
 * Every scalar mirrors an adreno_prog_kernel_t field one-for-one (see the
 * ir3_shader_variant mapping in <adreno/adreno_prog.h>); the assembled binary
 * itself lives in pglc_stage.code / code_words.  The two owned arrays are the
 * payload sections that follow the binary in the blob.
 */
#ifndef ADRENO_PGLC_H
#define ADRENO_PGLC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADRENO_PGLC_MAX_SSBO        8u
#define ADRENO_PGLC_MAX_UBO_RANGES  8u

struct adreno_pglc_ubo_range {
   uint32_t src_byte;        /* byte offset into the per-draw uniform blob */
   uint32_t dst_vec4;        /* const-file vec4 the range is pushed to     */
   uint32_t size_bytes;      /* multiple of 16                             */
};

struct adreno_pglc_stage {
   bool      geom_pixel_owned; /* VERSION 3: raster grid covers pixels */
   uint32_t  instrlen;
   uint32_t  constlen;
   uint32_t  shared_size;
   uint32_t  local_size[3];

   uint8_t   max_reg;
   uint8_t   max_half_reg;
   bool      double_threadsize;
   bool      mergedregs;
   uint32_t  branchstack_hw;
   bool      early_preamble;
   bool      round_robin_mode;

   uint32_t  pvtmem_size;
   bool      pvtmem_per_wave;

   /* Immediate constants (v->imm_state): imm_count u32 values pushed at vec4
    * imm_base.  malloc()ed; NULL when imm_count == 0. */
   uint32_t  imm_count;
   uint32_t  imm_base;
   uint32_t *imm_values;

   /* Immutable constant data appended to the binary BO at constant_data_off
    * (consts_ubo descriptor target).  malloc()ed; NULL when size == 0. */
   uint32_t  constant_data_size;
   uint32_t  constant_data_off;
   uint32_t *constant_data;
   int32_t   consts_ubo_idx;

   /* UBO 0 == the per-draw uniform blob: descriptors the kernel still ldc's
    * (num_ubos) and the ranges ir3 chose to push into the const file. */
   uint32_t  num_ubos;
   uint32_t  num_ubo_ranges;
   struct adreno_pglc_ubo_range ubo_range[ADRENO_PGLC_MAX_UBO_RANGES];

   /* struct ir3_driver_params_cs slot; ADRENO_PROG_NO_DRIVER_PARAMS = none */
   uint32_t  driver_params_vec4_off;
   uint32_t  driver_params_dwords;

   uint32_t  local_invocation_id_regid;
   uint32_t  work_group_id_regid;

   /* UAV slot i (== linked SSBO block i) -> declared layout(binding). */
   uint32_t  num_ssbo;
   uint32_t  ssbo_binding[ADRENO_PGLC_MAX_SSBO];
};

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_PGLC_H */
