/*
 * adreno_texlayout.c - Adreno implementation of pglc_tex_layout_2d /
 * pglc_tex_store_2d.
 *
 * The q8b 3D path is compute-only: textured kernels (geom_raster_cs.h
 * textured / colour-modulated variants) read their texture through an SSBO
 * as plain row-major ARGB8888 words, not through the a6xx texture unit, so
 * there is no tiling (no UBWC, no a6xx tile modes) to apply.  Level 0 is
 * stored as linear raster rows with stride == w * cpp; tiling is always
 * PGLC_TEX_TILING_RASTER, which the client already treats as "pick the
 * linear-texture kernels" on every backend.
 */
#include <string.h>
#include "pglcompiler.h"

bool pglc_tex_layout_2d(uint32_t w, uint32_t h, uint32_t cpp,
                        struct pglc_tex_layout *out)
{
   if (!out || w == 0 || h == 0 || cpp == 0)
      return false;
   out->tiling   = PGLC_TEX_TILING_RASTER;
   out->ub_pad   = 0;
   out->padded_w = w;
   out->padded_h = h;
   out->stride   = w * cpp;
   out->size     = out->stride * h;
   return true;
}

void pglc_tex_store_2d(void *dst, const void *src, uint32_t src_stride,
                       uint32_t w, uint32_t h, uint32_t cpp,
                       const struct pglc_tex_layout *l)
{
   uint32_t row_bytes = w * cpp;
   uint8_t *d = (uint8_t *)dst;
   const uint8_t *s = (const uint8_t *)src;

   if (!dst || !src || !l)
      return;
   if (src_stride == l->stride && row_bytes == l->stride) {
      memcpy(d, s, (size_t)l->stride * h);
      return;
   }
   for (uint32_t y = 0; y < h; y++) {
      memcpy(d, s, row_bytes);
      d += l->stride;
      s += src_stride;
   }
}
