/*
 * adreno_pglc.cpp - the Adreno (q8b / A690) implementation of the
 * backend-neutral pglcompiler.h contract (compile half).
 *
 * Drives the shared GLSL->NIR frontend (projects/opengl/libs/gles/pglcompiler,
 * reached through pglcompiler_frontend.h) with ir3's nir_shader_compiler_options,
 * hands the linked compute NIR to the C backend bridge (adreno_backend.h: mesa
 * freedreno/ir3, NIR->a6xx) and packs the result into the neutral pglc_stage:
 * `code` is the assembled a6xx instruction stream as u32 words and `backend`
 * is a struct adreno_pglc_stage snapshot of the ir3_shader_variant fields the
 * daemon's compute dispatch needs.  adreno_blob.c reads that snapshot to
 * serialize the adreno_prog_blob_t wire format; nothing else ever looks at it.
 *
 * ROUTES: Adreno has no graphics-pipeline port here, so pglc_compile_glsl()
 * declines (portablegl keeps that program on its CPU rasterizer or routes it
 * to a compute kernel); both compute entries - the shadertoy fragment->compute
 * wrapper and the verbatim geom_raster_cs.h kernels - compile.
 *
 * WHY C++: this TU is C++ only because the frontend header pulls nir.h and we
 * build it with the same flags as the frontend; ir3 headers are not C++-clean
 * and stay behind the C bridge.  Everything exported is extern "C".
 *
 * Machine library: machines/q8b/system/libs/adrenocompiler, built by
 * gles/Makefile through PGL_G3D_COMPILER_DIR into libpglcompiler_be.a.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pglcompiler.h"
#include "pglcompiler_frontend.h"
#include "adreno_backend.h"
#include "adreno_pglc.h"
#include "adreno_geom_source.h"

/* ---------------------------------------------------------------------------
 * Version gate
 * ------------------------------------------------------------------------- */

/* This backend is built only for the q8b machine, whose single GPU is the
 * SC8280XP's Adreno 690.  The compile target is fixed in adreno_backend.c, so
 * there is nothing to select at runtime: pglc_set_version() is a no-op kept
 * only to satisfy the backend-neutral pglcompiler.h contract. */
extern "C" void
pglc_set_version(int ver)
{
   (void)ver;
}

/* g2d_get_version() on q8b (bsp_g2d_version) reports 690.  Everything else -
 * V3D codes, 0 ("daemon not queried yet") - is declined so portablegl keeps
 * the CPU/NEON rasterizer instead of compiling for silicon this build cannot
 * target. */
extern "C" bool
pglc_version_supported(uint32_t ver)
{
   return ver == 690u;
}

/* ---------------------------------------------------------------------------
 * Stage packing
 * ------------------------------------------------------------------------- */

/* Move the backend result into the neutral stage: the binary becomes
 * st->code (ownership transferred), the snapshot (with its two owned arrays)
 * is copied into a calloc'd st->backend.  On false, st is left safe to free
 * and `be` still owns whatever was not moved. */
static bool
pack_stage(struct adreno_be_result *be, struct pglc_stage *st)
{
   struct adreno_pglc_stage *ab;

   st->code = NULL;
   st->code_words = 0;
   st->backend = NULL;
   memcpy(st->local_size, be->st.local_size, sizeof(st->local_size));

   ab = (struct adreno_pglc_stage *)calloc(1, sizeof(*ab));
   if (!ab)
      return false;
   *ab = be->st;
   be->st.imm_values = NULL;      /* arrays now owned by ab */
   be->st.constant_data = NULL;
   st->backend = ab;

   st->code = be->bin;
   st->code_words = be->bin_words;
   be->bin = NULL;
   be->bin_words = 0;
   return true;
}

static void
stage_free(struct pglc_stage *st)
{
   struct adreno_pglc_stage *ab = (struct adreno_pglc_stage *)st->backend;
   if (ab) {
      free(ab->imm_values);
      free(ab->constant_data);
      free(ab);
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
 * Graphics route: not ported
 * ------------------------------------------------------------------------- */
extern "C" bool
pglc_validate_glsl(const char *vert_src, const char *frag_src,
                   char *log, uint32_t logsz)
{
   struct pglc_fe_shaders *sh = NULL;
   bool ok = pglc_fe_compile_glsl(vert_src, frag_src, adreno_be_nir_options(),
                                 &sh, log, logsz);
   pglc_fe_shaders_free(sh);
   return ok;
}

extern "C" bool
pglc_compile_glsl(const char *vert_src, const char *frag_src,
                  struct pglc_program *res)
{
   (void)vert_src;
   (void)frag_src;
   if (!res)
      return false;
   memset(res, 0, sizeof(*res));
   snprintf(res->log, sizeof(res->log),
            "pglc_compile_glsl: the Adreno backend has no graphics pipeline "
            "port; use the compute route (pglc_compile_compute*)");
   return false;
}

/* ---------------------------------------------------------------------------
 * Compute route
 * ------------------------------------------------------------------------- */
static bool
compile_compute_source(const char *cs_src, struct pglc_program *res)
{
   struct pglc_fe_shaders *sh = NULL;
   struct adreno_be_result be;
   const struct nir_shader_compiler_options *nir_opts;
   nir_shader *cs_nir;
   bool packed;

   if (!res)
      return false;
   memset(res, 0, sizeof(*res));
   memset(&be, 0, sizeof(be));
   if (!cs_src) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: compute source is required");
      return false;
   }

   nir_opts = adreno_be_nir_options();
   if (!nir_opts) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: ir3 compiler unavailable for A690");
      return false;
   }

   if (!pglc_fe_compile_compute(cs_src, nir_opts, &sh, res->log, sizeof(res->log)))
      goto fail;
   cs_nir = pglc_fe_nir(sh, PGLC_STAGE_COMPUTE);
   if (!cs_nir) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: linked program lacks a CS NIR");
      goto fail;
   }

   if (!adreno_be_compile_compute(cs_nir, &be, res->log, sizeof(res->log)))
      goto fail;

   /* SSBO block-index -> binding remap: the kernel addresses UAV slot i ==
    * linked block i (gl_nir_lower_buffers), but the daemon binds buffers BY
    * DECLARED BINDING.  With a single SSBO (shadertoy) index==binding
    * coincides; with several (the geometry rasterizer) the linker orders
    * blocks by hash iteration, permuting the mapping, so record each slot's
    * declared binding before packing. */
   {
      uint32_t nblk = pglc_fe_num_ssbos(sh);
      for (uint32_t i = 0; i < be.st.num_ssbo; i++)
         be.st.ssbo_binding[i] = (i < nblk) ? pglc_fe_ssbo_binding(sh, i) : i;
   }

   packed = pack_stage(&be, &res->stage[PGLC_STAGE_COMPUTE]);
   adreno_be_result_free(&be);
   if (!packed) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: out of memory packing the a6xx kernel");
      goto fail;
   }

   /* Name -> dword-offset reflection of the uniforms the kernel reads (the
    * backend compiled a clone, so cs_nir still carries the frontend's byte
    * driver_locations; these are the UBO-0 byte offsets / 4). */
   if (!pglc_fe_build_uniform_bindings(cs_nir, &res->stage[PGLC_STAGE_COMPUTE])) {
      snprintf(res->log, sizeof(res->log),
               "pglc_compile_compute: out of memory building uniform reflection");
      goto fail;
   }

   pglc_fe_shaders_free(sh);
   res->success = true;
   return true;

fail:
   adreno_be_result_free(&be);
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
   const char *specialized = adreno_geom_source(cs_src);
   /* RenderWare's raw raster kernel also owns one pixel per invocation.
      The legacy triangle grid only covers a few rows of its dispatch box;
      kind 5 still needs the pixel-grid contract even with an xform pass. */
   bool pixel_owned = specialized != NULL ||
                      (cs_src && !strcmp(cs_src, rw_raster_cs));
   bool ok = compile_compute_source(specialized ? specialized : cs_src, res);
   if (ok && pixel_owned)
      ((struct adreno_pglc_stage *)res->stage[PGLC_STAGE_COMPUTE].backend)->geom_pixel_owned = true;
   return ok;
}
