/*
 * nv_blob.c - GA10B wire-blob serializer (pglc_program_serialize).
 *
 * Two program shapes:
 *   - geom_kind != 0: the geometry tile rasterizer's precompiled twins, a pair
 *     of kernel ids + uniform-region spans (ga10b_prog_blob_t).
 *   - geom_kind == 0: a fullscreen compute program compiled on device
 *     (nv_pglc_stage.cubin); emits a ga10b_compute_blob_t header + the cubin
 *     ELF, which the daemon loads via ga10b_dev_module and dispatches one
 *     thread per pixel.
 * Both are padded to at least sizeof(g2d_pgl_program_blob_t) (g2dd's minimum).
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include <pglcompiler.h>
#include <g2dclient/g2dclient.h>   /* sizeof(g2d_pgl_program_blob_t) */
#include <ga10b/ga10b_pgl.h>
#include "nv_pglc.h"

static uint32_t pad_blob(uint32_t n)
{
   if (n < sizeof(g2d_pgl_program_blob_t))
      n = sizeof(g2d_pgl_program_blob_t);
   return (n + 3u) & ~3u;
}

uint32_t pglc_program_serialize(const struct pglc_program *main_prog,
                                const struct pglc_program *xform_prog,
                                const struct pglc_blob_opts *opts,
                                void *out, uint32_t cap)
{
   const struct pglc_stage *rs;

   if (!main_prog || !opts)
      return 0;
   rs = &main_prog->stage[PGLC_STAGE_COMPUTE];

   if (opts->geom_kind == 0) {
      /* fullscreen compute: a compiler-generated cubin */
      const struct nv_pglc_stage *ab =
         (const struct nv_pglc_stage *)rs->backend;
      ga10b_compute_blob_t cb;
      uint32_t total;
      if (!ab || !ab->cubin || ab->cubin_size == 0)
         return 0;
      total = pad_blob(sizeof(cb) + ab->cubin_size);
      if (!out)
         return total;
      if (cap < total)
         return 0;
      memset(out, 0, total);
      memset(&cb, 0, sizeof(cb));
      cb.magic = GA10B_PROG_MAGIC;
      cb.version = GA10B_PROG_VERSION;
      cb.geom_kind = 0;
      memcpy(cb.local_size, rs->local_size, sizeof(cb.local_size));
      cb.unif_dwords = rs->uniform_blob_dwords;
      cb.cubin_size = ab->cubin_size;
      memcpy(out, &cb, sizeof(cb));
      memcpy((uint8_t *)out + sizeof(cb), ab->cubin, ab->cubin_size);
      return total;
   }

   /* geometry tile rasterizer: two precompiled twins */
   {
      const struct pglc_stage *xs;
      ga10b_prog_blob_t b;
      uint32_t total = pad_blob(sizeof(b));
      if (!xform_prog)
         return 0;
      xs = &xform_prog->stage[PGLC_STAGE_COMPUTE];
      if (rs->code_words != 1 || xs->code_words != 1)
         return 0;
      if (!out)
         return total;
      if (cap < total)
         return 0;
      memset(out, 0, total);
      memset(&b, 0, sizeof(b));
      b.magic = GA10B_PROG_MAGIC;
      b.version = GA10B_PROG_VERSION;
      b.geom_kind = opts->geom_kind;
      b.geom_tpw = opts->geom_tpw;
      b.raster_kernel = rs->code[0];
      b.xform_kernel = xs->code[0];
      b.raster_unif_dwords = rs->uniform_blob_dwords;
      b.xform_unif_dwords = xs->uniform_blob_dwords;
      memcpy(out, &b, sizeof(b));
      return total;
   }
}

uint32_t pglc_blob_format_version(void)
{
   return GA10B_PROG_VERSION;
}