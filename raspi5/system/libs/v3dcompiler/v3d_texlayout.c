/*
 * v3d_texlayout.c - V3D 2D texture memory layout + CPU tiling: the V3D
 * backend's implementation of pglc_tex_layout_2d / pglc_tex_store_2d.
 *
 * The V3D TMU cannot sample a raster (row-major) 2D image: level 0 of a 2D
 * texture must be in one of the hardware tiling layouts (LT / UBLINEAR / UIF),
 * chosen from the image size by a fixed rule.  The gallium v3d state tracker
 * (v3d_resource.c v3d_setup_slices + v3d_get_ub_pad) is NOT vendored, so the
 * single-level 2D case of that rule is transcribed here, and the pixel
 * shuffling itself is delegated to the vendored broadcom/common/v3d_tiling.c
 * (v3d_store_tiled_image), which is the same code gallium uses for
 * transfer_map uploads.
 *
 * pglc_tex_layout.tiling carries enum v3d_tiling_mode value-for-value
 * (RASTER = 0 = PGLC_TEX_TILING_RASTER, then LINEARTILE, UBLINEAR_1_COLUMN,
 * UBLINEAR_2_COLUMN, UIF_NO_XOR, UIF_XOR), which is also the V3D_3D_TEX_TILING_*
 * numbering the g2dd daemon (videocore v3d_3d.c) decodes from the ctex request.
 */
#include <string.h>
#include "util/box.h"
#include "util/macros.h"
#include "broadcom/common/v3d_tiling.h"
#include "pglcompiler.h"

/* UIF cache geometry (gallium v3d_context.h / v3d_resource.c). */
#define V3D_UIFCFG_BANKS            8u
#define V3D_UIFCFG_PAGE_SIZE        4096u
#define V3D_PAGE_CACHE_SIZE         (V3D_UIFCFG_PAGE_SIZE * V3D_UIFCFG_BANKS)
#define V3D_UIFBLOCK_SIZE           256u
#define V3D_UIFBLOCK_ROW_SIZE       (4u * V3D_UIFBLOCK_SIZE)

#define PAGE_UB_ROWS                (V3D_UIFCFG_PAGE_SIZE / V3D_UIFBLOCK_ROW_SIZE)
#define PAGE_UB_ROWS_TIMES_1_5      ((PAGE_UB_ROWS * 3u) >> 1)
#define PAGE_CACHE_UB_ROWS          (V3D_PAGE_CACHE_SIZE / V3D_UIFBLOCK_ROW_SIZE)
#define PAGE_CACHE_MINUS_1_5_UB_ROWS (PAGE_CACHE_UB_ROWS - PAGE_UB_ROWS_TIMES_1_5)

/* v3d_resource.c v3d_get_ub_pad(): extra UIF-block rows appended below a UIF
 * level so that consecutive columns do not all land in the same page-cache
 * bank. */
static uint32_t get_ub_pad(uint32_t cpp, uint32_t height)
{
   uint32_t utile_h = v3d_utile_height(cpp);
   uint32_t uif_block_h = utile_h * 2;
   uint32_t height_ub = height / uif_block_h;
   uint32_t height_offset_in_pc = height_ub % PAGE_CACHE_UB_ROWS;

   /* For the perfectly-aligned-for-UIF-XOR case, don't add any pad. */
   if (height_offset_in_pc == 0)
      return 0;

   /* Try padding up to where we're offset by at least half a page. */
   if (height_offset_in_pc < PAGE_UB_ROWS_TIMES_1_5) {
      /* If we fit entirely in the page cache, don't pad. */
      if (height_ub < PAGE_CACHE_UB_ROWS)
         return 0;
      else
         return PAGE_UB_ROWS_TIMES_1_5 - height_offset_in_pc;
   }

   /* If we're close to being aligned to page cache size, then round up and
    * rely on XOR. */
   if (height_offset_in_pc > PAGE_CACHE_MINUS_1_5_UB_ROWS)
      return PAGE_CACHE_UB_ROWS - height_offset_in_pc;

   /* Otherwise, we're far enough away (top and bottom) to not need any
    * padding. */
   return 0;
}

bool pglc_tex_layout_2d(uint32_t w, uint32_t h, uint32_t cpp,
                        struct pglc_tex_layout *out)
{
   uint32_t utile_w, utile_h, uif_block_w, uif_block_h;
   uint32_t level_width = w, level_height = h;

   if (out == NULL || w == 0 || h == 0)
      return false;
   if (cpp != 1 && cpp != 2 && cpp != 4 && cpp != 8 && cpp != 16)
      return false;
   memset(out, 0, sizeof(*out));

   utile_w = v3d_utile_width((int)cpp);
   utile_h = v3d_utile_height((int)cpp);
   uif_block_w = utile_w * 2;
   uif_block_h = utile_h * 2;

   /* v3d_setup_slices(), level 0 of a non-MSAA tiled 2D texture (uif_top
    * false, so every branch is reachable). */
   if (level_width <= utile_w || level_height <= utile_h) {
      out->tiling = (uint32_t)V3D_TILING_LINEARTILE;
      level_width = align(level_width, utile_w);
      level_height = align(level_height, utile_h);
   } else if (level_width <= uif_block_w) {
      out->tiling = (uint32_t)V3D_TILING_UBLINEAR_1_COLUMN;
      level_width = align(level_width, uif_block_w);
      level_height = align(level_height, uif_block_h);
   } else if (level_width <= 2 * uif_block_w) {
      out->tiling = (uint32_t)V3D_TILING_UBLINEAR_2_COLUMN;
      level_width = align(level_width, 2 * uif_block_w);
      level_height = align(level_height, uif_block_h);
   } else {
      /* We align the width to a 4-block column of UIF blocks, but we only
       * align height to UIF blocks. */
      level_width = align(level_width, 4 * uif_block_w);
      level_height = align(level_height, uif_block_h);

      out->ub_pad = get_ub_pad(cpp, level_height);
      level_height += out->ub_pad * uif_block_h;

      /* If the padding set us to be aligned to the page cache size, then
       * the HW will use the XOR bit on odd columns to get us perfectly
       * misaligned. */
      if ((level_height / uif_block_h) %
          (V3D_PAGE_CACHE_SIZE / V3D_UIFBLOCK_ROW_SIZE) == 0)
         out->tiling = (uint32_t)V3D_TILING_UIF_XOR;
      else
         out->tiling = (uint32_t)V3D_TILING_UIF_NO_XOR;
   }

   out->padded_w = level_width;
   out->padded_h = level_height;
   out->stride   = level_width * cpp;
   out->size     = out->stride * level_height;
   return true;
}

void pglc_tex_store_2d(void *dst, const void *src, uint32_t src_stride,
                       uint32_t w, uint32_t h, uint32_t cpp,
                       const struct pglc_tex_layout *layout)
{
   struct pipe_box box;
   enum v3d_tiling_mode mode;

   if (dst == NULL || src == NULL || layout == NULL || w == 0 || h == 0)
      return;

   /* The neutral tiling word mirrors enum v3d_tiling_mode value-for-value. */
   mode = (enum v3d_tiling_mode)layout->tiling;
   if (mode == V3D_TILING_RASTER) {
      uint32_t y;
      for (y = 0; y < h; y++)
         memcpy((uint8_t *)dst + (size_t)y * layout->stride,
                (const uint8_t *)src + (size_t)y * src_stride,
                (size_t)w * cpp);
      return;
   }

   /* Padding rows/columns are never sampled (REPEAT/CLAMP wrap against the
    * real w/h), but zero them so the buffer holds no stale data. */
   memset(dst, 0, layout->size);

   box.x = 0; box.y = 0; box.z = 0;
   box.width = (int16_t)w; box.height = (int16_t)h; box.depth = 1;
   v3d_store_tiled_image(dst, layout->stride, (void *)src, src_stride,
                         mode, (int)cpp, layout->padded_h, &box);
}

/* The V3D TMU cannot sample a raster (row-major) image - level 0 must be in a
   tiled layout (see the file header).  Returning false tells the shared client
   (pgl_g3d.c) to leave a raster slot unbound, so a draw that would sample it
   declines to the CPU path instead of reading a scrambled picture. */
bool pglc_tex_sampler_reads_raster(void)
{
   return false;
}
