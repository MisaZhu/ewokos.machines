/* Internal C-linkage bridge between the GLSL frontend (C++) and the V3D
 * backend (C) inside the machine v3dcompiler backend library.
 *
 * WHY A SEPARATE C BRIDGE
 * -----------------------
 * v3d_compiler.h has no `extern "C"` guard and transitively pulls in
 * util/enum_operators.h, which includes the C++ header <type_traits>. Including
 * it from a C++ TU (even inside an `extern "C" { }` wrapper) would put a C++
 * standard-library header under C language linkage and fail to compile. The v3d
 * backend is C-only anyway, so all backend calls (v3d_compiler_init/v3d_compile,
 * the hand-built device info, the FS/VS keys and prog_data reads) live in
 * v3d_backend.c, and the C++ contract driver (v3d_pglc.cpp)
 * talks to it through this small, C++-safe interface.
 *
 * Only nir.h crosses the boundary: it has a proper `extern "C"` guard and is
 * routinely included from mesa's own C++ frontend, so it is safe on both sides.
 *
 * Platform-INDEPENDENT compiler code. Upstream pin
 * f45825a0c724d06f231a1210484fe9af71a3ffab. See VENDORED.txt.
 */
#ifndef V3D_BACKEND_H
#define V3D_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "compiler/nir/nir.h"   /* nir_shader (has its own extern "C" guard) */

#ifdef __cplusplus
extern "C" {
#endif

/* One backend-compiled stage.
 *
 * qpu points at a malloc()ed array of qpu_instr_count 64-bit V3D QPU
 * instructions (exactly what v3d_compile() returns); the owner releases it with
 * v3d_be_result_free() (which calls free()). The C++ entry point re-packs these
 * 64-bit words into the little-endian u32 stream the public API documents.
 *
 * PROG_DATA SNAPSHOT (WS4): uniforms are deep-copied out of the ralloc'd
 * prog_data (which v3d_be_compile_program frees before returning) into the two
 * malloc()ed u32 arrays below, and the scalar VPM / attribute / FS-input fields
 * the shader-state record needs are copied verbatim. All of it is released by
 * v3d_be_result_free(); the C++ pack_stage() re-copies it into the public
 * pglc_stage (+ v3d_pglc_stage snapshot). */
struct v3d_be_stage {
   uint64_t *qpu;
   uint32_t  qpu_instr_count;
   uint32_t  num_uniforms;
   uint32_t  num_outputs;
   bool      needs_coords;

   /* Deep-copied uniform stream (num_uniforms entries each, NULL when 0).
    * uniform_contents[i] is the enum quniform_contents value; uniform_data[i]
    * is its 32-bit argument. */
   uint32_t *uniform_contents;
   uint32_t *uniform_data;

   uint8_t   threads;
   bool      single_seg;

   /* Compute stage only (v3d_compute_prog_data). */
   uint32_t  local_size[3];
   uint32_t  shared_size;
   uint32_t  spill_size;   /* base.spill_size: bytes/thread of register-spill scratch */

   /* Vertex stage only. */
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

struct v3d_be_result {
   bool success;
   struct v3d_be_stage vs;
   struct v3d_be_stage fs;
   /* Coordinate-shader (binning) variant of the vertex stage: the same VS NIR
    * compiled with v3d_vs_key.is_coord = true, so it outputs only gl_Position
    * (no varyings).  Only qpu / num_uniforms / uniform_contents / uniform_data
    * and the VPM size fields are meaningful; the FS-only and attribute-map
    * fields are zero.  Required so arbitrary-geometry draws bin with the real
    * model/view/projection transform instead of the fixed pass-through coord
    * shader. */
   struct v3d_be_stage coord;
   /* Compute stage (CSD route): only qpu / qpu_instr_count / num_uniforms /
    * uniform_contents / uniform_data / threads / single_seg / local_size[] /
    * shared_size are meaningful; everything else is zero.  Filled only by
    * v3d_be_compile_compute(), never by v3d_be_compile_program(). */
   struct v3d_be_stage cs;
};

/* Compile a linked vertex+fragment NIR pair to VideoCore V3D 4.2 QPU.
 *
 * vs and fs are the gl_nir_link_glsl()-produced linked shaders (IO already
 * lowered to intrinsics). Neither is mutated: v3d_compile() clones internally.
 * The fragment stage is compiled FIRST because the vertex stage's used_outputs
 * (and therefore its VPM varying layout) must match the fragment stage's
 * input_slots exactly; those are read back from the compiled FS prog_data and
 * fed into the VS key.
 *
 * On success returns true and fills res->vs / res->fs (qpu buffers malloc()ed,
 * release with v3d_be_result_free()). On failure returns false and writes a
 * human-readable diagnostic into log (up to logsz bytes, NUL-terminated). */
bool v3d_be_compile_program(nir_shader *vs, nir_shader *fs,
                            struct v3d_be_result *res,
                            char *log, size_t logsz);

/* Compile a linked compute NIR shader (MESA_SHADER_COMPUTE) to V3D 4.2 QPU and
 * fill res->cs (qpu, uniform stream, threads/single_seg, local_size[],
 * shared_size).  The cs NIR is not mutated (v3d_compile clones internally).
 * Returns false and writes a diagnostic into log on failure. */
bool v3d_be_compile_compute(nir_shader *cs,
                            struct v3d_be_result *res,
                            char *log, size_t logsz);

/* Release the QPU buffers owned by a result. Safe on a zeroed/failed result. */
void v3d_be_result_free(struct v3d_be_result *res);

/* Return the backend's enum quniform_contents value for QUNIFORM_SSBO_OFFSET.
 * The C++ frontend cannot name the enum (it never includes v3d_compiler.h), but
 * needs the value to remap SSBO_OFFSET uniform slots after a compute compile. */
uint32_t v3d_be_quniform_ssbo_offset(void);

/* Select the device info the backend compiles against, by live V3D version
 * (ver == major*10 + minor: 42 = V3D 4.2 / Pi4).  This raspix backend supports
 * V3D 4.2 only; any other value (21 on Pi3, 71 on Pi5) leaves the 42 default in
 * place and is declined by pglc_version_supported() before a compile is issued.
 * Must be called BEFORE the first v3d_be_compile_*() on a machine that ships one
 * binary across chip generations (raspix).  The selected devinfo pointer is
 * retained for the process lifetime. */
void v3d_be_set_version(int ver);

/* Whether the currently-selected device can emit a saturating float unpack
 * (V3D71_QPU_UNPACK_SAT), i.e. v3d_device_has_unpack_sat() for the devinfo
 * chosen by the last v3d_be_set_version().  This raspix backend only ever
 * selects V3D 4.2 (Pi4, ver < 45), which CANNOT, so it always returns false:
 * the frontend sets nir_shader_compiler_options.lower_fsat accordingly and the
 * focused nir_opt_algebraic() (pglcompiler_algebraic.c) expands nir_op_fsat into
 * fmin/fmax before it reaches ntq_emit_alu(), which asserts the capability. */
bool v3d_be_has_unpack_sat(void);

#ifdef __cplusplus
}
#endif

#endif /* V3D_BACKEND_H */
