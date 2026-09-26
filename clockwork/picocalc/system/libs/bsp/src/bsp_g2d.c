#include <bsp/bsp_g2d.h>
#include <g2d_arch.h>

/* thin dispatch layer: every operation is implemented by the platform's
   arch_g2d_* back end (NEON software engine on virt, a hardware 2D engine
   on platforms that provide one). the *_contig flags tell the back end
   when a buffer is physically contiguous, and *_phy carries the resolved
   physical base so a hardware engine can work on physical addresses. */

int32_t bsp_g2d_init(void) {
	return arch_g2d_init();
}

/* software back end has no engine clock to report */
uint32_t bsp_g2d_clock_hz(void) {
	return 0;
}

/* blit into a raw physical destination (scan-out push): the hardware engine could write the physical range directly, but that fast path is not wired up on this platform yet, so decline and let the caller (displayd flush_g2d) fall back to its cpu flush path */
int32_t bsp_g2d_blt_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
				int32_t sx, int32_t sy, int32_t sw, int32_t sh,
				ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
				uint32_t dst_pitch,
				int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	(void)argb_src; (void)src_phy; (void)src_contig; (void)src_w; (void)src_h;
	(void)sx; (void)sy; (void)sw; (void)sh;
	(void)dst_phy; (void)dst_size; (void)dst_w; (void)dst_h;
	(void)dst_pitch;
	(void)dx; (void)dy; (void)dw; (void)dh;
	return -1;
	}


int32_t bsp_g2d_fill(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig, int32_t argb_w, int32_t argb_h,
		int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	return arch_g2d_fill(argb, argb_phy, contig, argb_w, argb_h, x, y, w, h, color);
}

int32_t bsp_g2d_fill_alpha(uint32_t* argb, int32_t argb_w, int32_t argb_h,
		int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
	return arch_g2d_fill_alpha(argb, argb_w, argb_h, x, y, w, h, color);
}

int32_t bsp_g2d_blt(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
		int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
	return arch_g2d_blt(argb_src, src_phy, src_contig, src_w, src_h, sx, sy, sw, sh,
		argb_dst, dst_phy, dst_contig, dst_w, dst_h, dx, dy, dw, dh);
}

int32_t bsp_g2d_blt_alpha(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h,
		int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint8_t alpha) {
	return arch_g2d_blt_alpha(argb_src, src_phy, src_contig, src_w, src_h, sx, sy, sw, sh,
		argb_dst, dst_phy, dst_contig, dst_w, dst_h, dx, dy, dw, dh, alpha);
}

/* cpu path for sub-alignment tails and narrow copies: handed to the back
   end's 1:1 blit, which works purely on the virtual pointers and ignores
   phy/contig (passed 0 here). its simd blocks plus scalar/padded tail
   handle any width, and it clips the rect against both buffer bounds.
   use_alpha == 0 is a plain copy; otherwise the same blend math as
   bsp_g2d_blt_alpha, and alpha == 0 is a no-op. */
int32_t bsp_g2d_blt_cpu(uint32_t* argb_src, int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		uint32_t* argb_dst, int32_t dst_w, int32_t dst_h,
		int32_t dx, int32_t dy, uint8_t use_alpha, uint8_t alpha) {
	if(use_alpha != 0)
		return arch_g2d_blt_alpha(argb_src, 0, 0, src_w, src_h, sx, sy, sw, sh,
			argb_dst, 0, 0, dst_w, dst_h, dx, dy, sw, sh, alpha);
	return arch_g2d_blt(argb_src, 0, 0, src_w, src_h, sx, sy, sw, sh,
		argb_dst, 0, 0, dst_w, dst_h, dx, dy, sw, sh);
}

int32_t bsp_g2d_scale_to(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h) {
	return arch_g2d_scale_to(argb_src, src_phy, src_contig, src_w, src_h,
		argb_dst, dst_phy, dst_contig, dst_w, dst_h);
}

int32_t bsp_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
		int32_t* dst_w, int32_t* dst_h) {
	return arch_g2d_rotated_size(src_w, src_h, degree, dst_w, dst_h);
}

int32_t bsp_g2d_rotate(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
		uint32_t* argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig, int32_t dst_w, int32_t dst_h, int32_t degree) {
	return arch_g2d_rotate(argb_src, src_phy, src_contig, src_w, src_h,
		argb_dst, dst_phy, dst_contig, dst_w, dst_h, degree);
}
/* fixed Q16 weights of the gaussian blur (sigma = radius/2, normalized to
   exactly 65536 with the center weight absorbing the rounding) - the same
   tables the hardware blur kernels use, so software and GPU outputs are
   bit-exact against each other and against g2dtest's scalar reference. */
static const uint16_t gauss_wk2[5] = { 25386, 5664, 3436, 5664, 25386 };
static const uint16_t gauss_wk4[9] = { 17608, 7340, 3929, 2700, 2382,
        2700, 3929, 7340, 17608 };

/* whole-surface separable gaussian blur, software two-pass on the CPU
   (this machine has no blur-capable GPU back end, so the bsp implements
   the op itself - same fixed Q16 weights, edge replication and round
   half-up as the GPU kernels).  tmp is the caller's scratch surface
   (>= w*h*4 bytes); phy/contig are ignored by the software path. */
int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h, int32_t radius) {
    const uint16_t* wk;
    uint32_t x, y, t, ks;
    (void)argb_phy; (void)contig; (void)tmp_phy; (void)tmp_contig;

    /* the G2D_* result codes are the g2dclient wire values: -1 FAILED,
       -2 NOT_SUPPORTED (g2dd passes the return through verbatim) */
    if(argb == NULL || tmp == NULL || argb_w <= 0 || argb_h <= 0)
        return -1;
    if(radius == 2) {
        wk = gauss_wk2;
        ks = 5;
    }
    else if(radius == 4) {
        wk = gauss_wk4;
        ks = 9;
    }
    else {
        return -2; /* G2D_ERR_NOT_SUPPORTED */
    }

    /* H pass: argb -> tmp */
    for(y = 0; y < (uint32_t)argb_h; y++) {
        const uint32_t* srow = argb + (size_t)y * (uint32_t)argb_w;
        for(x = 0; x < (uint32_t)argb_w; x++) {
            uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
            for(t = 0; t < ks; t++) {
                int32_t xx = (int32_t)x + (int32_t)t - radius;
                uint32_t p;
                if(xx < 0) xx = 0;
                if(xx >= argb_w) xx = argb_w - 1;
                p = srow[xx];
                sb += (p & 0xff) * wk[t];
                sg += ((p >> 8) & 0xff) * wk[t];
                sr += ((p >> 16) & 0xff) * wk[t];
                sa += ((p >> 24) & 0xff) * wk[t];
            }
            tmp[y * (uint32_t)argb_w + x] =
                (((sa + 32768) >> 16) << 24) |
                (((sr + 32768) >> 16) << 16) |
                (((sg + 32768) >> 16) << 8) |
                ((sb + 32768) >> 16);
        }
    }
    /* V pass: tmp -> argb (in place) */
    for(y = 0; y < (uint32_t)argb_h; y++) {
        for(x = 0; x < (uint32_t)argb_w; x++) {
            uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
            for(t = 0; t < ks; t++) {
                int32_t yy = (int32_t)y + (int32_t)t - radius;
                uint32_t p;
                if(yy < 0) yy = 0;
                if(yy >= argb_h) yy = argb_h - 1;
                p = tmp[(uint32_t)yy * (uint32_t)argb_w + x];
                sb += (p & 0xff) * wk[t];
                sg += ((p >> 8) & 0xff) * wk[t];
                sr += ((p >> 16) & 0xff) * wk[t];
                sa += ((p >> 24) & 0xff) * wk[t];
            }
            argb[y * (uint32_t)argb_w + x] =
                (((sa + 32768) >> 16) << 24) |
                (((sr + 32768) >> 16) << 16) |
                (((sg + 32768) >> 16) << 8) |
                ((sb + 32768) >> 16);
        }
    }
    return 0; /* G2D_OK */
}
