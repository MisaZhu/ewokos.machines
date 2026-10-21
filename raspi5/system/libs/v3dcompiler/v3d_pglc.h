/*
 * v3d_pglc.h - V3D backend-private per-stage snapshot hung off
 * pglc_stage.backend.
 *
 * Produced by v3d_pglc.cpp (pack_stage) from the C bridge's v3d_be_stage and
 * consumed only by v3d_blob.c (pglc_program_serialize), which lays the fields
 * out into the g2d_pgl_program_blob_t header + payload the raspi5/raspix
 * g2dd daemon (bsp_g3d_program -> videocore v3d_3d.c) parses.  portablegl
 * never sees this type: it reaches the stage through the neutral
 * pglc_stage fields only.
 *
 * All members mirror struct v3d_be_stage one-for-one minus the 64-bit qpu
 * array (already re-packed into pglc_stage.code as lo/hi u32 pairs).
 */
#ifndef V3D_PGLC_H
#define V3D_PGLC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct v3d_pglc_stage {
   uint32_t  qpu_instr_count;      /* == pglc_stage.code_words / 2 */
   uint32_t  num_uniforms;
   uint32_t  num_outputs;
   bool      needs_coords;

   /* Deep-copied uniform stream (num_uniforms entries each, NULL when 0):
    * uniform_contents[i] is the enum quniform_contents value, uniform_data[i]
    * its 32-bit argument.  malloc()ed; freed by pglc_program_free(). */
   uint32_t *uniform_contents;
   uint32_t *uniform_data;

   uint8_t   threads;
   bool      single_seg;

   /* Compute stage only. */
   uint32_t  local_size[3];
   uint32_t  shared_size;
   uint32_t  spill_size;

   /* Vertex / coord stage only. */
   uint32_t  vpm_input_size;
   uint32_t  vpm_output_size;
   bool      separate_segments;
   uint32_t  vcm_cache_size;
   uint8_t   vattr_sizes[64 / 4];
   int32_t   driver_location_map[64];

   /* Fragment stage only. */
   uint32_t  num_inputs;
   uint32_t  input_slots[64];
   bool      uses_center_w;
   bool      writes_z;
   bool      disable_ez;
   bool      uses_implicit_point_line_varyings;
};

#ifdef __cplusplus
}
#endif

#endif /* V3D_PGLC_H */
