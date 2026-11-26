/*
 * nv_texlayout.c - GA10B texture memory layout for the pglcompiler.h contract.
 * Plain linear rows: the geometry kernels index the image as a linear word
 * array, and the compiled fullscreen samplers sample the same linear layout
 * in software (nv_tex.c: SSBO loads + bilinear / cube face selection), so a
 * raster slot backs a compiled sampler and a cube stages as 6 linear faces.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <pglcompiler.h>

bool pglc_tex_layout_2d(uint32_t w, uint32_t h, uint32_t cpp,
                        struct pglc_tex_layout *out)
{
   if (!out || w == 0 || h == 0 || cpp == 0)
      return false;
   memset(out, 0, sizeof(*out));
   out->tiling = PGLC_TEX_TILING_RASTER;
   out->padded_w = w;
   out->padded_h = h;
   out->stride = w * cpp;
   out->size = out->stride * h;
   return true;
}

bool pglc_tex_layout_mip_chain(uint32_t w, uint32_t h, uint32_t cpp,
                               uint32_t last_level,
                               struct pglc_tex_layout *out)
{
   if (last_level != 0)
      return false;
   return pglc_tex_layout_2d(w, h, cpp, out);
}

bool pglc_tex_sampler_cube_supported(void)
{
   return true;
}

void pglc_tex_store_2d(void *dst, const void *src, uint32_t src_stride,
                       uint32_t w, uint32_t h, uint32_t cpp,
                       const struct pglc_tex_layout *l)
{
   uint32_t row = w * cpp;
   uint8_t *d = (uint8_t *)dst;
   const uint8_t *s = (const uint8_t *)src;

   if (!dst || !src || !l)
      return;
   for (uint32_t y = 0; y < h; y++) {
      memcpy(d, s, row);
      d += l->stride;
      s += src_stride;
   }
}

bool pglc_tex_sampler_reads_raster(void)
{
   return true;
}