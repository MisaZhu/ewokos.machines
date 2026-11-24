/*
 * nv_pglc.cpp - the GA10B (Orin Nano / sm_87) implementation of the
 * backend-neutral pglcompiler.h contract (compile half).
 *
 * Two routes fill pglc_stage[PGLC_STAGE_COMPUTE]:
 *   - the fixed geom_raster_cs.h geometry texts map onto kernels precompiled
 *     with nvcc and embedded in the driver (libs/ga10b/kernels): an exact text
 *     match returns the twin's GA10B_PGL_K_* id plus its uniform layout (the
 *     same substitute-by-text scheme the Adreno back end uses).  This is the
 *     fast, verified path the tile rasterizer runs.
 *   - everything else is compiled on device: the shared GLSL->NIR frontend
 *     (pglcompiler_frontend.h) produces linked compute NIR, nv_backend.c lowers
 *     it to sm_87 SASS, and nv_cubin.c wraps the SASS + launch metadata into a
 *     cubin the ga10b_dev module loader consumes.  The SASS lives in
 *     pglc_stage.code; the cubin + metadata in the nv_pglc_stage snapshot.
 *
 * ROUTES: GA10B has no graphics-pipeline port here, so pglc_compile_glsl()
 * declines (portablegl keeps that program on its CPU rasterizer or routes it to
 * a compute kernel); both compute entries compile.
 *
 * WHY C++: this TU is C++ only because the frontend header pulls nir.h and we
 * build it with the same flags as the frontend; the SASS encoder / cubin
 * synthesizer are plain C behind nv_backend.h / nv_cubin.h.  Everything
 * exported is extern "C".
 *
 * Machine library: machines/nvidia/orin.nano/system/libs/nvcompiler, built by
 * gles/Makefile through PGL_G3D_COMPILER_DIR into libpglcompiler_be.a.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pglcompiler.h"
#include "pglcompiler_frontend.h"
#include "nv_backend.h"
#include "nv_cubin.h"
#include "nv_pglc.h"
#include <geom_raster_cs.h>
#include <ga10b/ga10b_pgl.h>

/* ---------------------------------------------------------------------------
 * Version gate
 * ------------------------------------------------------------------------- */
extern "C" void
pglc_set_version(int ver)
{
   (void)ver;
}

extern "C" bool
pglc_version_supported(uint32_t ver)
{
   return ver == GA10B_PGL_VERSION;
}

/* ---------------------------------------------------------------------------
 * Precompiled geometry twins (fast path)
 * ------------------------------------------------------------------------- */
struct nv_unif {
   const char* name;
   uint32_t dword_offset;
   uint32_t num_components;
};

struct nv_kernel {
   const char* glsl;
   uint32_t id;
   uint32_t local_size[3];
   uint32_t unif_dwords;
   const struct nv_unif* unif;
   uint32_t num_unif;
};

static const struct nv_unif xform_color_unif[] = {
   { "uMVP",        GA10B_PGL_XF_MVP,         16 },
   { "uViewportWH", GA10B_PGL_XF_VIEWPORT_WH, 2 },
   { "uVertCount",  GA10B_PGL_XF_VERT_COUNT,  1 },
   { "uColAlpha",   GA10B_PGL_XF_COL_ALPHA,   1 },
};
static const struct nv_unif xform_tex_unif[] = {
   { "uMVP",        GA10B_PGL_XF_MVP,         16 },
   { "uViewportWH", GA10B_PGL_XF_VIEWPORT_WH, 2 },
   { "uVertCount",  GA10B_PGL_XF_VERT_COUNT,  1 },
};
static const struct nv_unif xform_lit_unif[] = {
   { "uMVP",        GA10B_PGL_XL_MVP,         16 },
   { "uModelView",  GA10B_PGL_XL_MODELVIEW,   16 },
   { "uNormalMat",  GA10B_PGL_XL_NORMALMAT,   16 },
   { "uMaterial",   GA10B_PGL_XL_MATERIAL,    4 },
   { "uLightEye",   GA10B_PGL_XL_LIGHT_EYE,   4 },
   { "uShade",      GA10B_PGL_XL_SHADE,       4 },
   { "uViewportWH", GA10B_PGL_XL_VIEWPORT_WH, 2 },
   { "uVertCount",  GA10B_PGL_XL_VERT_COUNT,  1 },
};
static const struct nv_unif xform_inst_unif[] = {
   { "uLightEye",   GA10B_PGL_XI_LIGHT_EYE,   4 },
   { "uShade",      GA10B_PGL_XI_SHADE,       4 },
   { "uViewportWH", GA10B_PGL_XI_VIEWPORT_WH, 2 },
   { "uVertCount",  GA10B_PGL_XI_VERT_COUNT,  1 },
};
static const struct nv_unif raster_unif[] = {
   { "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
   { "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
   { "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
   { "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
   { "uBlendMode",  GA10B_PGL_RS_BLEND_MODE,  1 },
   { "uDepthWrite", GA10B_PGL_RS_DEPTH_WRITE, 1 },
   { "uCullSign",   GA10B_PGL_RS_CULL_SIGN,   1 },
};
static const struct nv_unif raster_tex_unif[] = {
   { "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
   { "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
   { "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
   { "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
   { "uTexW",       GA10B_PGL_RT_TEX_W,       1 },
   { "uTexH",       GA10B_PGL_RT_TEX_H,       1 },
};
static const struct nv_unif raster_texcol_unif[] = {
   { "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
   { "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
   { "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
   { "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
   { "uTexW",       GA10B_PGL_RT_TEX_W,       1 },
   { "uTexH",       GA10B_PGL_RT_TEX_H,       1 },
   { "uScissor",    GA10B_PGL_RT_SCISSOR,     4 },
};

#define NV_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define NV_UNIF(t)  t, NV_COUNT(t)

static const struct nv_kernel nv_kernels[] = {
   { geom_xform_color_cs_glsl, GA10B_PGL_K_XFORM_COLOR, { 8, 8, 1 },
     GA10B_PGL_XF_DWORDS, NV_UNIF(xform_color_unif) },
   { geom_xform_cs_glsl, GA10B_PGL_K_XFORM_LIT, { 8, 8, 1 },
     GA10B_PGL_XL_DWORDS, NV_UNIF(xform_lit_unif) },
   { geom_xform_tex_cs_glsl, GA10B_PGL_K_XFORM_TEX, { 8, 8, 1 },
     GA10B_PGL_XF_DWORDS, NV_UNIF(xform_tex_unif) },
   { geom_xform_texcol_cs_glsl, GA10B_PGL_K_XFORM_TEXCOL, { 8, 8, 1 },
     GA10B_PGL_XF_DWORDS, NV_UNIF(xform_tex_unif) },
   { geom_xform_inst_cs_glsl, GA10B_PGL_K_XFORM_INST, { 8, 8, 1 },
     GA10B_PGL_XI_DWORDS, NV_UNIF(xform_inst_unif) },
   { geom_xform_instlit_cs_glsl, GA10B_PGL_K_XFORM_INSTLIT, { 8, 8, 1 },
     GA10B_PGL_XI_DWORDS, NV_UNIF(xform_inst_unif) },
   { geom_raster_cs_glsl, GA10B_PGL_K_RASTER, { 16, 16, 1 },
     GA10B_PGL_RS_DWORDS, NV_UNIF(raster_unif) },
   { geom_raster_tex_cs_glsl, GA10B_PGL_K_RASTER_TEX, { 8, 8, 1 },
     GA10B_PGL_RT_DWORDS, NV_UNIF(raster_tex_unif) },
   { geom_raster_texcol_cs_glsl, GA10B_PGL_K_RASTER_TEXCOL, { 8, 8, 1 },
     GA10B_PGL_RTC_DWORDS, NV_UNIF(raster_texcol_unif) },
};

/* ---------------------------------------------------------------------------
 * Stage packing / release
 * ------------------------------------------------------------------------- */
static void
stage_free(struct pglc_stage *st)
{
   struct nv_pglc_stage *ab = (struct nv_pglc_stage *)st->backend;
   if (ab) {
      free(ab->cubin);
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

static bool
nv_decline(struct pglc_program *res, const char *why)
{
   if (res) {
      memset(res, 0, sizeof(*res));
      snprintf(res->log, sizeof(res->log), "ga10b: %s", why);
   }
   return false;
}

/* ---------------------------------------------------------------------------
 * Graphics route: not ported
 * ------------------------------------------------------------------------- */
extern "C" bool
pglc_compile_glsl(const char *vert_src, const char *frag_src,
                  struct pglc_program *res)
{
   (void)vert_src;
   (void)frag_src;
   return nv_decline(res, "no graphics pipeline port (compute kernels only)");
}

/* ---------------------------------------------------------------------------
 * Real on-device compute compile
 * ------------------------------------------------------------------------- */
static bool
compile_compute_source(const char *cs_src, struct pglc_program *res)
{
   struct pglc_fe_shaders *sh = NULL;
   struct nv_be_result be;
   const struct nir_shader_compiler_options *nir_opts;
   nir_shader *cs_nir;
   struct pglc_stage *st;
   struct nv_pglc_stage *ab;

   if (!res)
      return false;
   memset(res, 0, sizeof(*res));
   memset(&be, 0, sizeof(be));
   if (!cs_src)
      return nv_decline(res, "compute source is required");

   nir_opts = nv_be_nir_options();
   if (!pglc_fe_compile_compute(cs_src, nir_opts, &sh, res->log, sizeof(res->log)))
      goto fail;
   cs_nir = pglc_fe_nir(sh, PGLC_STAGE_COMPUTE);
   if (!cs_nir) {
      snprintf(res->log, sizeof(res->log),
               "ga10b: linked program lacks a CS NIR");
      goto fail;
   }
   if (!nv_be_compile_compute(cs_nir, &be, res->log, sizeof(res->log)))
      goto fail;

   st = &res->stage[PGLC_STAGE_COMPUTE];
   ab = (struct nv_pglc_stage *)calloc(1, sizeof(*ab));
   if (!ab) {
      snprintf(res->log, sizeof(res->log), "ga10b: out of memory packing stage");
      goto fail;
   }
   ab->kernel_id = 0;
   ab->meta = be.meta;
   /* Wrap the SASS into a cubin so the ga10b_dev module loader consumes it
    * unchanged.  Uniforms reach the kernel through constant bank 0
    * (load_uniform c[0][NV_UNIFORM_BASE_OFF+off]); SSBO bases at
    * NV_SSBO_BASE_OFF.  Emit a zeroed cb0 of NV_CB0_SIZE so the loader
    * allocates the bank the daemon fills at launch. */
   {
      uint8_t cb0[NV_CB0_SIZE];
      memset(cb0, 0, sizeof(cb0));
      uint32_t csz = nv_cubin_build(be.bin, be.bin_words, &be.meta, "pgl_cs",
                                    NULL, 0, cb0, sizeof(cb0), NULL, 0);
      if (csz) {
         ab->cubin = (uint8_t *)malloc(csz);
         if (ab->cubin) {
            ab->cubin_size = nv_cubin_build(be.bin, be.bin_words, &be.meta,
                                            "pgl_cs", NULL, 0, cb0, sizeof(cb0),
                                            ab->cubin, csz);
            if (ab->cubin_size == 0) {
               free(ab->cubin);
               ab->cubin = NULL;
            }
         }
      }
   }
   st->backend = ab;
   st->code = be.bin;            /* ownership transferred */
   st->code_words = be.bin_words;
   be.bin = NULL;
   be.bin_words = 0;
   memcpy(st->local_size, be.meta.local_size, sizeof(st->local_size));
   nv_be_result_free(&be);

   /* Name -> dword-offset reflection of the uniforms the kernel reads. */
   if (!pglc_fe_build_uniform_bindings(cs_nir, st)) {
      snprintf(res->log, sizeof(res->log),
               "ga10b: out of memory building uniform reflection");
      goto fail;
   }
   pglc_fe_shaders_free(sh);
   res->success = true;
   return true;

fail:
   nv_be_result_free(&be);
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

   if (!frag_src)
      return nv_decline(res, "fragment source is required");
   cs_src = pglc_fe_fragment_to_compute_source(frag_src);
   if (!cs_src)
      return nv_decline(res, "out of memory building compute source");
   ok = compile_compute_source(cs_src, res);
   free(cs_src);
   return ok;
}

/* Verbatim compute entry: a fixed geometry text returns its precompiled twin;
 * any other complete GLSL ES 3.10 compute shader is compiled on device. */
extern "C" bool
pglc_compile_compute_raw(const char *cs_src, struct pglc_program *res)
{
   if (!res)
      return false;
   memset(res, 0, sizeof(*res));
   if (!cs_src)
      return nv_decline(res, "no kernel source");

   for (uint32_t i = 0; i < NV_COUNT(nv_kernels); i++) {
      const struct nv_kernel *k = &nv_kernels[i];
      struct pglc_stage *st = &res->stage[PGLC_STAGE_COMPUTE];
      struct nv_pglc_stage *ab;
      if (strcmp(cs_src, k->glsl) != 0)
         continue;
      st->code = (uint32_t *)malloc(sizeof(uint32_t));
      st->uniform_bindings = (struct pglc_uniform_binding *)
         calloc(k->num_unif, sizeof(*st->uniform_bindings));
      ab = (struct nv_pglc_stage *)calloc(1, sizeof(*ab));
      if (!st->code || !st->uniform_bindings || !ab) {
         free(st->code);
         free(st->uniform_bindings);
         free(ab);
         return nv_decline(res, "out of memory");
      }
      st->code[0] = k->id;
      st->code_words = 1;
      ab->kernel_id = k->id;
      st->backend = ab;
      for (uint32_t u = 0; u < k->num_unif; u++) {
         struct pglc_uniform_binding *b = &st->uniform_bindings[u];
         strncpy(b->name, k->unif[u].name, PGLC_MAX_UNIF_NAME - 1);
         b->dword_offset = k->unif[u].dword_offset;
         b->num_components = k->unif[u].num_components;
      }
      st->num_uniform_bindings = k->num_unif;
      st->uniform_blob_dwords = k->unif_dwords;
      memcpy(st->local_size, k->local_size, sizeof(st->local_size));
      res->success = true;
      return true;
   }
   return compile_compute_source(cs_src, res);
}