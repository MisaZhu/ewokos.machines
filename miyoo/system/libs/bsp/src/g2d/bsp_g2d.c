/*
 * bsp_g2d.c - miyoo (SigmaStar SSD202D) g2d back end: GE-accelerated
 * offline ARGB8888 drawing, following the raspi5 dispatch-layer design
 * (bsp_g2d.c policy + a hardware back end file, ge_g2d.c).
 *
 * Operation routing:
 *
 *   bsp_g2d_fill        any clipped rect, opaque color -> GE rectfill
 *   bsp_g2d_blt         1:1 copy (sw==dw, sh==dh)      -> GE bitblt
 *   bsp_g2d_blt_alpha   1:1 copy, global const alpha   -> GE bitblt + DFB
 *   bsp_g2d_fill_alpha  any                            -> scalar cpu
 *   bsp_g2d_blt_cpu     any                            -> scalar cpu
 *   bsp_g2d_scale_to    any                            -> -1
 *   bsp_g2d_rotate      any                            -> -1
 *
 * Everything the recovered GE register interface can do runs on the
 * hardware.  What remains on the cpu is there by API design, not as a
 * fallback: bsp_g2d_fill_alpha and bsp_g2d_blt_cpu carry no physical
 * base, so no hardware path can ever take them (fill_alpha is the
 * translucent-color fill the g2dd routes away from the opaque
 * bsp_g2d_fill; blt_cpu serves g2dd's sub-8px pitch-alignment tails).
 *
 * Scale and rotate are NOT implementable: the vendor library exposes
 * neither on this chip (MI_GFX_Rotate_e knows only ROTATE_0, no
 * StretchBlit ioctl was recovered), so they return -1.  There is no
 * software fallback here - when the device path fails, the client side
 * (libgraph graph_*_arch/cpu) renders on the cpu.
 *
 * GE eligibility (on top of the size checks):
 *   - the *_contig flag is set and the matching *_phy carries a valid
 *     physical base inside the DRAM window (the caller resolves it:
 *     contig shm slab / sys_dma memory).  Such canvases are mapped
 *     NOCACHE in every process, so the engine's accesses need no ARM
 *     cache maintenance.
 *
 * ZERO COPY: the GE operates directly on the caller's buffers through
 * the caller-supplied physical bases, translated to MIU bus addresses.
 *
 * NO CPU REPLAY: an operation that was submitted to the GE and failed
 * (wait-idle timeout) returns -1 and is never replayed here - a wedged
 * engine may still own or have partially written the destination.
 */

#include <bsp/bsp_g2d.h>

#include <string.h>

#include "ge_g2d.h"

/* ------------------------------------------------------------------ */
/* GE eligibility helpers                                              */
/* ------------------------------------------------------------------ */

static int ge_ok(int32_t w, int32_t h) {
	return ge_g2d_ready() && w > 0 && h > 0;
}

/* Validate a caller-provided physical base and return the GE-visible
 * MIU bus address, or 0 when the canvas cannot run on the GE (not
 * physically contiguous, no phy supplied, or the address fails the
 * RAM-range validation gate). */
static uint32_t ge_phys(ewokos_addr_t phys, size_t bytes, uint8_t contig) {
	if (!contig || phys == 0)
		return 0;
	if (!ge_g2d_phy_valid(phys, bytes))
		return 0;
	return ge_g2d_miu(phys);
}

/* clip a rect to a w x h surface; returns 0 when nothing is left */
static int ge_clip_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                        int32_t surf_w, int32_t surf_h) {
	int64_t x0 = *x < 0 ? 0 : *x;
	int64_t y0 = *y < 0 ? 0 : *y;
	int64_t x1 = (int64_t)*x + *w;      /* 64-bit: rect sums may overflow */
	int64_t y1 = (int64_t)*y + *h;

	if (x1 > surf_w)
		x1 = surf_w;
	if (y1 > surf_h)
		y1 = surf_h;
	if (x1 <= x0 || y1 <= y0)
		return 0;
	*x = (int32_t)x0;
	*y = (int32_t)y0;
	*w = (int32_t)(x1 - x0);
	*h = (int32_t)(y1 - y0);
	return 1;
}

/* ------------------------------------------------------------------ */
/* scalar blend helper for the cpu-only entry points                   */
/* ------------------------------------------------------------------ */

/* Scalar source-over blend (same math as the raspi5 back end):
 * out_a = dst_a + ((255 - dst_a) * a) / 255,
 * out_c = (src_c * a + dst_c * (255 - a)) / 255. */
static uint32_t blend_argb_scalar(uint32_t dst_color, uint8_t a,
                                  uint8_t r, uint8_t g, uint8_t b) {
	uint32_t oa = (dst_color >> 24) & 0xff;
	uint32_t dr = (dst_color >> 16) & 0xff;
	uint32_t dg = (dst_color >> 8) & 0xff;
	uint32_t db = dst_color & 0xff;
	uint32_t inv_a = 255 - a;

	oa = oa + (255 - oa) * a / 255;
	dr = (r * a + dr * inv_a) / 255;
	dg = (g * a + dg * inv_a) / 255;
	db = (b * a + db * inv_a) / 255;
	return (oa << 24) | (dr << 16) | (dg << 8) | db;
}

/* ------------------------------------------------------------------ */
/* bsp_g2d API                                                         */
/* ------------------------------------------------------------------ */

int32_t bsp_g2d_init(void) {
	/* GE-only back end: without the engine there is no g2d service
	 * at all and clients render on the cpu from the start */
	return ge_g2d_init();
}

int32_t bsp_g2d_fill(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                   int32_t argb_w, int32_t argb_h,
                   int32_t x, int32_t y, int32_t w, int32_t h,
                   uint32_t color) {
	int32_t rx = x, ry = y, rw = w, rh = h;

	if (argb != NULL && ge_ok(argb_w, argb_h) &&
	    ge_clip_rect(&rx, &ry, &rw, &rh, argb_w, argb_h)) {
		uint32_t miu = ge_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
		if (miu != 0)
			return ge_g2d_fill(miu, argb_w, argb_h, rx, ry,
			                   rx + rw, ry + rh, color);
	}
	return -1;
}

/* 1:1 dst clip shared by the blit entry points: cutting the left/top
 * edge shifts the src origin by the same delta, cutting right/bottom
 * just shrinks the size.  returns 0 when nothing is left. */
static int ge_clip_blit(int32_t *csx, int32_t *csy,
                        int32_t *cdx, int32_t *cdy,
                        int32_t *cw, int32_t *ch,
                        int32_t src_w, int32_t src_h,
                        int32_t dst_w, int32_t dst_h) {
	if (*cdx < 0) { *csx -= *cdx; *cw += *cdx; *cdx = 0; }
	if (*cdy < 0) { *csy -= *cdy; *ch += *cdy; *cdy = 0; }
	if (*cdx + *cw > dst_w) *cw = dst_w - *cdx;
	if (*cdy + *ch > dst_h) *ch = dst_h - *cdy;

	return *cw > 0 && *ch > 0 &&
	       *csx >= 0 && *csy >= 0 &&
	       *csx + *cw <= src_w && *csy + *ch <= src_h;
}

int32_t bsp_g2d_blt(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                  int32_t src_w, int32_t src_h,
                  int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                  uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                  int32_t dst_w, int32_t dst_h,
                  int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	/* GE does 1:1 copies only: the vendor library exposes no stretch
	 * on this chip, so scaled blits fail */
	if (argb_src != NULL && argb_dst != NULL && argb_src != argb_dst &&
	    sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
	    sw == dw && sh == dh &&
	    ge_ok(dst_w, dst_h)) {
		int32_t csx = sx, csy = sy, cw = dw, ch = dh;
		int32_t cdx = dx, cdy = dy;

		if (ge_clip_blit(&csx, &csy, &cdx, &cdy, &cw, &ch,
		                 src_w, src_h, dst_w, dst_h)) {
			uint32_t src_miu = ge_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
			uint32_t dst_miu = ge_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
			if (src_miu != 0 && dst_miu != 0)
				return ge_g2d_blit(src_miu, src_w, src_h, csx, csy,
				                   dst_miu, dst_w, dst_h, cdx, cdy,
				                   cw, ch);
		}
	}
	return -1;
}

/* GE constant-alpha BitBlt through the DFB blend stage
 * (CTRL ABL|DFB, ABL_COEF = 1, ABL_CONST = alpha): same 1:1-only
 * constraint as bsp_g2d_blt.  alpha == 0xff degrades to the plain
 * copy path, alpha == 0 is a no-op. */
int32_t bsp_g2d_blt_alpha(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                        int32_t src_w, int32_t src_h,
                        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                        uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                        int32_t dst_w, int32_t dst_h,
                        int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                        uint8_t alpha) {
	if (alpha == 0)
		return 0;

	if (argb_src != NULL && argb_dst != NULL && argb_src != argb_dst &&
	    sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
	    sw == dw && sh == dh &&
	    ge_ok(dst_w, dst_h)) {
		int32_t csx = sx, csy = sy, cw = dw, ch = dh;
		int32_t cdx = dx, cdy = dy;

		if (ge_clip_blit(&csx, &csy, &cdx, &cdy, &cw, &ch,
		                 src_w, src_h, dst_w, dst_h)) {
			uint32_t src_miu = ge_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
			uint32_t dst_miu = ge_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
			if (src_miu != 0 && dst_miu != 0) {
				if (alpha == 0xff)
					return ge_g2d_blit(src_miu, src_w, src_h, csx, csy,
					                   dst_miu, dst_w, dst_h, cdx, cdy,
					                   cw, ch);
				return ge_g2d_blit_alpha(src_miu, src_w, src_h, csx, csy,
				                         dst_miu, dst_w, dst_h, cdx, cdy,
				                         cw, ch, alpha);
			}
		}
	}
	return -1;
}

/* CPU-only by API design (no physical base): alpha fill of a sub-rect,
 * clipped to the buffer bounds, exact per-pixel access; same blend math
 * as the GE constant-alpha path.  alpha == 0 is a no-op. */
int32_t bsp_g2d_fill_alpha(uint32_t *argb, int32_t argb_w, int32_t argb_h,
                         int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color) {
	uint8_t a;

	if (argb == NULL)
		return 0;
	a = (uint8_t)((color >> 24) & 0xff);
	if (a == 0)
		return 0;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (w <= 0 || h <= 0 || x >= argb_w || y >= argb_h)
		return 0;
	if (x + w > argb_w) w = argb_w - x;
	if (y + h > argb_h) h = argb_h - y;

	for (int32_t row = y; row < y + h; row++) {
		uint32_t *dp = argb + row * argb_w + x;
		for (int32_t col = 0; col < w; col++) {
			dp[col] = blend_argb_scalar(dp[col], a,
					(uint8_t)((color >> 16) & 0xff),
					(uint8_t)((color >> 8) & 0xff),
					(uint8_t)(color & 0xff));
		}
	}
	return 0;
}

/* CPU-only by API design (no physical base): serves g2dd's sub-8px
 * pitch-alignment tails and narrow 1:1 copies.  1:1 clip: cutting one
 * side shifts the other surface's origin by the same delta. */
int32_t bsp_g2d_blt_cpu(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                      int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                      uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                      int32_t dx, int32_t dy, uint8_t use_alpha,
                      uint8_t alpha) {
	if (argb_src == NULL || argb_dst == NULL || sw <= 0 || sh <= 0)
		return 0;
	if (use_alpha != 0 && alpha == 0)
		return 0;

	if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
	if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
	if (sx < 0) { dx -= sx; sw += sx; sx = 0; }
	if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
	if (sx + sw > src_w) sw = src_w - sx;
	if (sy + sh > src_h) sh = src_h - sy;
	if (dx + sw > dst_w) sw = dst_w - dx;
	if (dy + sh > dst_h) sh = dst_h - dy;
	if (sw <= 0 || sh <= 0)
		return 0;

	for (int32_t row = 0; row < sh; row++) {
		const uint32_t *sp = argb_src + (sy + row) * src_w + sx;
		uint32_t *dp = argb_dst + (dy + row) * dst_w + dx;

		if (use_alpha == 0) {
			memcpy(dp, sp, (size_t)sw * sizeof(uint32_t));
			continue;
		}
		for (int32_t col = 0; col < sw; col++) {
			uint32_t color = sp[col];
			uint32_t src_a = (color >> 24) & 0xff;
			uint8_t sa;

			if (src_a == 0)
				continue;
			sa = (uint8_t)((alpha == 0xff) ? src_a : (src_a * alpha) >> 8);
			if (sa == 0)
				continue;
			if (sa == 0xff) {
				dp[col] = color;
				continue;
			}
			dp[col] = blend_argb_scalar(dp[col], sa,
					(uint8_t)((color >> 16) & 0xff),
					(uint8_t)((color >> 8) & 0xff),
					(uint8_t)(color & 0xff));
		}
	}
	return 0;
}

/* GE stretch is not implementable: the vendor library exposes no
 * StretchBlit on this chip and the recovered register set programs a
 * single block size for both surfaces */
int32_t bsp_g2d_scale_to(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                       int32_t src_w, int32_t src_h,
                       uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                       int32_t dst_w, int32_t dst_h) {
	return -1;
}

/* GE rotation is not implementable: the vendor MI_GFX_Rotate_e knows
 * only ROTATE_0 on this chip.  report a zero result size so the caller
 * fails the operation cleanly */
int32_t bsp_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                           int32_t *dst_w, int32_t *dst_h) {
	if (dst_w != NULL)
		*dst_w = 0;
	if (dst_h != NULL)
		*dst_h = 0;
	return -1;
}

int32_t bsp_g2d_rotate(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                     int32_t src_w, int32_t src_h,
                     uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                     int32_t dst_w, int32_t dst_h, int32_t degree) {
	return -1;
}
