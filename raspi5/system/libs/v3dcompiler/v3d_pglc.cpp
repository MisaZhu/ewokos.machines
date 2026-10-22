/*
 * v3d_pglc.cpp - the VideoCore V3D implementation of the backend-neutral
 * pglcompiler.h contract (compile half).
 *
 * Drives the shared GLSL->NIR frontend (projects/soft3d/libs/gles/pglcompiler,
 * reached through pglcompiler_frontend.h) with V3D's nir_shader_compiler_options,
 * hands the linked NIR to the C backend bridge (v3d_backend.h: mesa
 * broadcom/compiler, NIR->QPU) and packs the result into the neutral
 * pglc_stage: `code` is the QPU stream as little-endian u32 pairs (lo, hi per
 * 64-bit instruction) and `backend` is a struct v3d_pglc_stage snapshot of the
 * prog_data the daemon's shader-state records need (uniform stream, VPM
 * sizes, FS flags).  v3d_blob.c reads that snapshot to serialize the
 * g2d_pgl_program_blob_t wire format; nothing else ever looks at it.
 *
 * WHY C++: nir.h is C++-safe but v3d_compiler.h is not (see v3d_backend.h), so
 * this TU sees only the frontend API + the C bridge; it is C++ merely because
 * the frontend header pulls nir.h and we build it with the same flags as the
 * frontend.  Everything exported is extern "C".
 *
 * Machine library: machines/<hw>/system/libs/v3dcompiler, built by
 * gles/Makefile through PGL_G3D_COMPILER_DIR into libpglcompiler_be.a.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pglcompiler.h"
#include "pglcompiler_frontend.h"
#include "v3d_backend.h"
#include "v3d_pglc.h"

/* ---------------------------------------------------------------------------
 * NIR compiler options the frontend lowers against (was v3d_nir_options in
 * the old monolithic driver).  Built imperatively: the struct is ~1000 lines
 * and designated-initializer ordering is fragile in C++.
 * ------------------------------------------------------------------------- */
static struct nir_shader_compiler_options s_nir_options;

static const struct nir_shader_compiler_options *
v3d_nir_options(void)
{
   static bool done = false;
   if (!done) {
      memset(&s_nir_options, 0, sizeof(s_nir_options));
      s_nir_options.float_mul_add32 = (nir_float_muladd_support)0;
      s_nir_options.has_fsub = true;   /* V3D emits fsub/isub natively */
      s_nir_options.has_isub = true;
      s_nir_options.lower_fdiv = true;
      s_nir_options.lower_fsqrt = true;
      s_nir_options.lower_fpow = true;
      s_nir_options.lower_fmod = true;
      s_nir_options.lower_ffract = true;
      /* Compute (CSD route): nir_to_vir.c has no case for
       * load_local_invocation_id / load_global_invocation_id -- it only reads
       * load_workgroup_id (CSD payload r3/r2) and load_local_invocation_index
       * (payload r2 high bits).  nir_lower_compute_system_values honours this
       * option by rewriting local_invocation_id in terms of the index, and
       * nir_lower_system_values rewrites global_invocation_id as
       * workgroup_id * workgroup_size + local_invocation_id.  Same setting as
       * upstream gallium v3d. */
      s_nir_options.lower_cs_local_id_to_index = true;
      /* V3D 7.1 (Pi5) has a saturating float unpack (V3D71_QPU_UNPACK_SAT), so
       * nir_op_fsat -- which the GLSL frontend's opt_algebraic folds
       * clamp(x,0,1) into -- is emitted as the single-instruction fsat path and
       * must NOT be expanded back to fmin/fmax.  This backend targets only the
       * Pi5, so lower_fsat is unconditionally false and its emitted QPU is
       * byte-for-byte stable. */
      s_nir_options.lower_fsat = false;
      /* Loop unrolling.  nir_to_vir.c's optimisation loop only runs
       * nir_opt_loop_unroll when max_unroll_iterations > 0, and the memset
       * above left it at 0, so EVERY counted loop reached the QPU as a real
       * loop.  On V3D a loop iteration costs ~30 instructions of control
       * flow on top of the body (execute-mask bookkeeping in the
       * per-block-id register, the uniform bu.allna/bu.anyap pair and their
       * 3 delay-slot nops each) - for the small fixed-trip loops shader
       * ports are full of (3x3 / 5x5 neighbourhood scans, N-octave noise)
       * that overhead is comparable to the body itself.  Upstream gallium
       * v3d sets 16; keep parity.  nir_opt_loop_unroll still gates on body
       * cost (trip_count * instr_cost <= 16 * LOOP_UNROLL_LIMIT) so the
       * 60/90-step ray-march loops stay rolled. */
      s_nir_options.max_unroll_iterations = 16;
      done = true;
   }
   return &s_nir_options;
}

/* ---------------------------------------------------------------------------
 * Version gate
 * ------------------------------------------------------------------------- */

/* This backend is built only for the raspi5 machine, whose single chip is the
 * Pi5 / BCM2712 VideoCore VII (V3D 7.1).  The compile target is fixed at ver 71
 * in v3d_backend.c, so there is nothing to select at runtime: pglc_set_version()
 * is a no-op kept only to satisfy the backend-neutral pglcompiler.h contract the
 * shared portablegl client links against. */
extern "C" void
pglc_set_version(int ver)
{
   (void)ver;
}

/* g2d_get_version() reports major*10+minor.  Only the Pi5's V3D 7.1 (71) is
 * supported by this single-chip backend; every other value -- 21 (VC4/Pi3), 42
 * (Pi4), 0 ("daemon not queried yet") -- is declined so portablegl keeps the
 * CPU/NEON rasterizer instead of compiling for silicon this build cannot
 * target. */
extern "C" bool
pglc_version_supported(uint32_t ver)
{
   return ver == 71u;
}

/* ---------------------------------------------------------------------------
 * Stage packing
 * ------------------------------------------------------------------------- */

/* Re-pack the backend's 64-bit QPU words into the neutral little-endian u32
 * stream (two words per instruction: lo then hi) and deep-copy the prog_data
 * snapshot (uniform stream + VPM/attribute/FS-input fields) into st->backend.
 * Takes ownership of nothing: be stays valid and is freed by
 * v3d_be_result_free().  Returns false on any allocation failure (st is left
 * safe to free). */
static bool
pack_stage(const struct v3d_be_stage *be, struct pglc_stage *st)
{
   struct v3d_pglc_stage *vb;

   st->code = NULL;
   st->code_words = 0;
   st->backend = NULL;
   memcpy(st->local_size, be->local_size, sizeof(st->local_size));

   vb = (struct v3d_pglc_stage *)calloc(1, sizeof(*vb));
   if (!vb)
      return false;
   st->backend = vb;

   vb->qpu_instr_count   = be->qpu_instr_count;
   vb->num_uniforms      = be->num_uniforms;
   vb->num_outputs       = be->num_outputs;
   vb->needs_coords      = be->needs_coords;
   vb->threads           = be->threads;
   vb->single_seg        = be->single_seg;
   memcpy(vb->local_size, be->local_size, sizeof(vb->local_size));
   vb->shared_size       = be->shared_size;
   vb->spill_size        = be->spill_size;
   vb->vpm_input_size    = be->vpm_input_size;
   vb->vpm_output_size   = be->vpm_output_size;
   vb->separate_segments = be->separate_segments;
   vb->vcm_cache_size    = be->vcm_cache_size;
   memcpy(vb->vattr_sizes, be->vattr_sizes, sizeof(vb->vattr_sizes));
   memcpy(vb->driver_location_map, be->driver_location_map,
          sizeof(vb->driver_location_map));
   vb->num_inputs        = be->num_inputs;
   memcpy(vb->input_slots, be->input_slots, sizeof(vb->input_slots));
   vb->uses_center_w     = be->uses_center_w;
   vb->writes_z          = be->writes_z;
   vb->disable_ez        = be->disable_ez;
   vb->uses_implicit_point_line_varyings = be->uses_implicit_point_line_varyings;

   /* Uniform stream: fresh malloc'd copies (be's arrays belong to the backend
    * result). */
   if (be->num_uniforms != 0) {
      size_t bytes = (size_t)be->num_uniforms * sizeof(uint32_t);
      vb->uniform_contents = (uint32_t *)malloc(bytes);
      vb->uniform_data     = (uint32_t *)malloc(bytes);
      if (!vb->uniform_contents || !vb->uniform_data)
         return false;
      memcpy(vb->uniform_contents, be->uniform_contents, bytes);
      memcpy(vb->uniform_data,     be->uniform_data,     bytes);
   }

   if (be->qpu_instr_count == 0)
      return true;

   st->code_words = be->qpu_instr_count * 2u;
   st->code = (uint32_t *)malloc((size_t)st->code_words * sizeof(uint32_t));
   if (!st->code) {
      st->code_words = 0;
      return false;
   }
   for (uint32_t i = 0; i < be->qpu_instr_count; i++) {
      uint64_t w = be->qpu[i];
      st->code[2 * i + 0] = (uint32_t)(w & 0xffffffffull);
      st->code[2 * i + 1] = (uint32_t)(w >> 32);
   }
   return true;
}

static void
stage_free(struct pglc_stage *st)
{
   struct v3d_pglc_stage *vb = (struct v3d_pglc_stage *)st->backend;
   if (vb) {
      free(vb->uniform_contents);
      free(vb->uniform_data);
      free(vb);
   }
   pglc_fe_stage_free_common(st);   /* code + uniform_bindings, then zero */
}

extern "C" void
pglc_program_free(struct pglc_program *res)
{
   if (!res)
      return;
   for (int i = 0; i < PGLC_STAGE_COUNT; i++)
      stage_free(&res->stage[i]);
   res->success = false;
}

/* ---------------------------------------------------------------------------
 * Graphics (binner) route
 * ------------------------------------------------------------------------- */
extern "C" bool
pglc_compile_glsl(const char *vert_src, const char *frag_src,
                  struct pglc_program *res)
{
   struct pglc_fe_shaders *sh = NULL;
   struct v3d_be_result be;
   nir_shader *vs_nir, *fs_nir;
   bool packed;

   if (!res)
      return false;
   memset(res, 0, sizeof(*res));

   if (!vert_src || !frag_src) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_glsl: both vertex and fragment source are required "
               "(vert=%p frag=%p)", (const void *)vert_src,
               (const void *)frag_src);
      return false;
   }

   if (!pglc_fe_compile_glsl(vert_src, frag_src, v3d_nir_options(), &sh,
                             res->log, sizeof(res->log)))
      goto fail;
   vs_nir = pglc_fe_nir(sh, PGLC_STAGE_VERTEX);
   fs_nir = pglc_fe_nir(sh, PGLC_STAGE_FRAGMENT);
   if (!vs_nir || !fs_nir) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_glsl: linked program lacks a VS/FS NIR");
      goto fail;
   }

   /* Run the broadcom backend. */
   memset(&be, 0, sizeof(be));
   if (!v3d_be_compile_program(vs_nir, fs_nir, &be, res->log, sizeof(res->log))) {
      v3d_be_result_free(&be);
      goto fail;
   }
   packed = pack_stage(&be.vs, &res->stage[PGLC_STAGE_VERTEX]) &&
            pack_stage(&be.fs, &res->stage[PGLC_STAGE_FRAGMENT]) &&
            pack_stage(&be.coord, &res->stage[PGLC_STAGE_COORD]);
   v3d_be_result_free(&be);
   if (!packed) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_glsl: out of memory packing QPU");
      goto fail;
   }

   /* Export the name -> dword-offset reflection for the uniforms each stage
    * actually reads.  v3d_compile() clones the NIR (vir.c), so vs_nir / fs_nir
    * still carry the driver_locations the frontend assigned; the client packs
    * the per-draw blob by name through this table.  The COORD stage is the
    * same linked VS NIR compiled with is_coord=true, so it shares the VS's
    * driver_locations: reflect vs_nir for it too, giving the client one blob
    * that feeds both the coord and render-VS uniform expansion. */
   if (!pglc_fe_build_uniform_bindings(vs_nir, &res->stage[PGLC_STAGE_VERTEX]) ||
       !pglc_fe_build_uniform_bindings(fs_nir, &res->stage[PGLC_STAGE_FRAGMENT]) ||
       !pglc_fe_build_uniform_bindings(vs_nir, &res->stage[PGLC_STAGE_COORD])) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_glsl: out of memory building uniform reflection");
      goto fail;
   }

   pglc_fe_shaders_free(sh);
   res->success = true;
   return true;

fail:
   pglc_fe_shaders_free(sh);
   {
      char keep[sizeof(res->log)];
      memcpy(keep, res->log, sizeof(keep));
      pglc_program_free(res);
      memcpy(res->log, keep, sizeof(keep));
   }
   return false;
}

/* ---------------------------------------------------------------------------
 * Compute (CSD) route
 * ------------------------------------------------------------------------- */
static bool
compile_compute_source(const char *cs_src, struct pglc_program *res)
{
   struct pglc_fe_shaders *sh = NULL;
   struct v3d_be_result be;
   nir_shader *cs_nir;
   bool packed;

   if (!res)
      return false;
   memset(res, 0, sizeof(*res));
   if (!cs_src) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: compute source is required");
      return false;
   }

   if (!pglc_fe_compile_compute(cs_src, v3d_nir_options(), &sh,
                                res->log, sizeof(res->log)))
      goto fail;
   cs_nir = pglc_fe_nir(sh, PGLC_STAGE_COMPUTE);
   if (!cs_nir) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: linked program lacks a CS NIR");
      goto fail;
   }

   memset(&be, 0, sizeof(be));
   if (!v3d_be_compile_compute(cs_nir, &be, res->log, sizeof(res->log))) {
      v3d_be_result_free(&be);
      goto fail;
   }

   /* SSBO block-index -> binding remap: gl_nir_lower_buffers records the
    * linked BLOCK INDEX in each QUNIFORM_SSBO_OFFSET slot's data, but the
    * daemon's expand_uniforms resolves the slot against its ssbo_iova[] table
    * BY BINDING.  With a single SSBO (shadertoy) index==binding coincides;
    * with several (the geometry rasterizer) the linker orders blocks by hash
    * iteration, permuting the mapping, so rewrite each slot's data to the
    * block's declared Binding before packing. */
   {
      uint32_t ssbo_off = v3d_be_quniform_ssbo_offset();
      uint32_t nblk = pglc_fe_num_ssbos(sh);
      for (uint32_t i = 0; i < be.cs.num_uniforms; i++) {
         if (be.cs.uniform_contents[i] == ssbo_off &&
             be.cs.uniform_data[i] < nblk)
            be.cs.uniform_data[i] = pglc_fe_ssbo_binding(sh, be.cs.uniform_data[i]);
      }
   }

   packed = pack_stage(&be.cs, &res->stage[PGLC_STAGE_COMPUTE]);
   v3d_be_result_free(&be);
   if (!packed) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: out of memory packing QPU");
      goto fail;
   }

   if (!pglc_fe_build_uniform_bindings(cs_nir, &res->stage[PGLC_STAGE_COMPUTE])) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: out of memory building uniform reflection");
      goto fail;
   }

   pglc_fe_shaders_free(sh);
   res->success = true;
   return true;

fail:
   pglc_fe_shaders_free(sh);
   {
      char keep[sizeof(res->log)];
      memcpy(keep, res->log, sizeof(keep));
      pglc_program_free(res);
      memcpy(res->log, keep, sizeof(keep));
   }
   return false;
}

extern "C" bool
pglc_compile_compute(const char *frag_src, struct pglc_program *res)
{
   char *cs_src;
   bool ok;

   if (!frag_src) {
      if (res) {
         memset(res, 0, sizeof(*res));
         snprintf(res->log, sizeof(res->log),
                  "pglc_compile_compute: fragment source is required");
      }
      return false;
   }
   cs_src = pglc_fe_fragment_to_compute_source(frag_src);
   if (!cs_src) {
      if (res) {
         memset(res, 0, sizeof(*res));
         snprintf(res->log, sizeof(res->log),
                  "pglc_compile_compute: out of memory building compute source");
      }
      return false;
   }
   ok = compile_compute_source(cs_src, res);
   free(cs_src);
   return ok;
}

/* Verbatim compute entry: a complete GLSL ES 3.10 compute shader (e.g. the
 * geom_raster_cs.h kernels) with no fragment wrapping. */
extern "C" bool
pglc_compile_compute_raw(const char *cs_src, struct pglc_program *res)
{
   return compile_compute_source(cs_src, res);
}
