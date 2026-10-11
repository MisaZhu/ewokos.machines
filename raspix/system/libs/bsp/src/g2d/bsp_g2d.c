/*
 * bsp_g2d.c - raspix (Raspberry Pi 3 / BCM2837, Pi 4 / BCM2711) g2d
 * public API: argument validation, rect clipping and the scalar CPU paths.
 * Every hardware operation is delegated to the VideoCore back end in
 * libvideocore (<videocore/vc_g2d.h>), which owns the affine map
 * construction shared with the QPU kernels, the eligibility gate on
 * caller-supplied physical addresses and the dispatches themselves.
 *
 * The hardware operations are GPU-only: a back-end call reports DONE,
 * UNSUPPORTED (declined before any submission) or FAILED (after a
 * submission), and both non-success states surface as -1 here.  There is
 * no implicit CPU fallback for fill/blit/scale/rotate - a submitted
 * dispatch is never replayed, because a timed-out dispatch may still own
 * or have partially written the destination.
 *
 * bsp_g2d_blt_cpu is the explicit CPU path: exact per-pixel access with no
 * alignment, contiguity, SIMD or GPU requirement, for callers that cannot
 * satisfy the GPU eligibility gate.  bsp_g2d_fill_alpha is GPU-only like
 * the other hardware operations (the alpha pipeline over a constant
 * source); a declined canvas reports -1 and the caller blends itself.
 */

#include <bsp/bsp_g2d.h>

#include <string.h>

#include <g2dclient/g2dclient.h>
#include <videocore/vc_g2d.h>

/* the gaussian blur is delegated to the platform arch_g2d_* NEON engine
   (resolved at link time through libgraph in the g2dd), the same policy
   machine.virt/miyoo use; the VideoCore back end has no blur kernel. */
#include <g2d_arch.h>

/* ------------------------------------------------------------------ */
/* bsp_g2d API                                                         */
/* ------------------------------------------------------------------ */

int32_t bsp_g2d_init(void)
{
    return vc_g2d_init();
}

uint32_t bsp_g2d_clock_hz(void)
{
    return vc_g2d_clock_hz();
}

/* devcmd /dev/g2d <args>: g2dd answers its own "stat" line and then hands
 * the arguments down here for the backend's share (the V3D per-dispatch
 * phase counters, see vc_g2d_cmd). */
int32_t bsp_g2d_cmd(int argc, char **argv, char *buf, uint32_t len)
{
    return vc_g2d_cmd(argc, argv, buf, (size_t)len);
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

/* rotate straight into a raw physical destination: no engine path on
   this platform, decline as a missing capability (sticky) so displayd
   stops asking and keeps its own rotate + flush path */
int32_t bsp_g2d_rotate_phy(uint32_t* argb_src, ewokos_addr_t src_phy, uint8_t src_contig, int32_t src_w, int32_t src_h,
        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
        ewokos_addr_t dst_phy, uint32_t dst_size, int32_t dst_w, int32_t dst_h,
        uint32_t dst_pitch,
        int32_t dx, int32_t dy, int32_t degree) {
    (void)argb_src; (void)src_phy; (void)src_contig; (void)src_w; (void)src_h;
    (void)sx; (void)sy; (void)sw; (void)sh;
    (void)dst_phy; (void)dst_size; (void)dst_w; (void)dst_h;
    (void)dst_pitch;
    (void)dx; (void)dy; (void)degree;
    return G2D_ERR_NOT_SUPPORTED;
}


int32_t bsp_g2d_fill(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                   int32_t argb_w, int32_t argb_h,
                   int32_t x, int32_t y, int32_t w, int32_t h,
                   uint32_t color)
{
    int32_t rx = x, ry = y, rw = w, rh = h;
    uint32_t phys = 0;
    int gr = GPU_UNSUPPORTED;

    if (!argb || argb_w <= 0 || argb_h <= 0 || w <= 0 || h <= 0)
        return -1;
    if (!gpu_clip_rect(&rx, &ry, &rw, &rh, argb_w, argb_h))
        return -1;
    if (gpu_ok(argb_w, argb_h))
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
    if (phys) {
        gr = gpu_fill(phys, argb, argb_w, argb_h, rx, ry, rx + rw, ry + rh,
                      color);
        if (gr == GPU_DONE)
            return 0;
        if (gr == GPU_FAILED)
            return -1;
    }
    return -1;
}

int32_t bsp_g2d_blt(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                  int32_t src_w, int32_t src_h,
                  int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                  uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                  int32_t dst_w, int32_t dst_h,
                  int32_t dx, int32_t dy, int32_t dw, int32_t dh)
{
    g2d_map_t m;
    int32_t rx = dx, ry = dy, rw = dw, rh = dh;
    uint32_t src_phys = 0, dst_phys = 0;
    int gr = GPU_UNSUPPORTED;

    if (!argb_src || !argb_dst || argb_src == argb_dst ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return -1;
    if (!gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h))
        return -1;
    if (gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_BSP_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            gr = gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                  dst_phys, argb_dst, dst_w, dst_h, rx, ry,
                                  rx + rw, ry + rh, 0);
            if (gr == GPU_DONE)
                return 0;
            if (gr == GPU_FAILED)
                return -1;
        }
    }
    return -1;
}

int32_t bsp_g2d_blt_alpha(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                        int32_t src_w, int32_t src_h,
                        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                        uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                        int32_t dst_w, int32_t dst_h,
                        int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                        uint8_t alpha)
{
    g2d_map_t m;
    int32_t rx = dx, ry = dy, rw = dw, rh = dh;
    uint32_t src_phys = 0, dst_phys = 0;
    int gr = GPU_UNSUPPORTED;

    if (alpha == 0)
        return 0;

    if (!argb_src || !argb_dst || argb_src == argb_dst ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return -1;
    if (!gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h))
        return -1;
    if (gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
        if (src_phys && dst_phys) {
            g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh,
                               G2D_BSP_MAP_ROT_0, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                gr = gpu_alpha_surface(&m, alpha, src_phys, argb_src,
                                       src_w, src_h, dst_phys, argb_dst,
                                       dst_w, dst_h, rx, ry,
                                       rx + rw, ry + rh);
                if (gr == GPU_DONE)
                    return 0;
                if (gr == GPU_FAILED)
                    return -1;
            }
        }
    }
    return -1;
}

/* Scalar source-over blend:
 * out_a = dst_a + ((255 - dst_a) * a) / 255,
 * out_c = (src_c * a + dst_c * (255 - a)) / 255. */
static uint32_t blend_argb_scalar(uint32_t dst_color, uint8_t a,
                                  uint8_t r, uint8_t g, uint8_t b)
{
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

/* Alpha fill of a sub-rect, clipped to the buffer bounds, on the GPU:
 * the VideoCore alpha pipeline over a constant-colour source
 * (gpu_fill_alpha_surface), effective alpha the colour's alpha byte.
 * Same GPU-only contract as bsp_g2d_blt_alpha: a canvas that fails the
 * eligibility gate, or a VC4 decline (rect width not a 16-pixel multiple),
 * reports -1 before anything is written and the caller blends on the cpu;
 * a submitted dispatch is never replayed.  alpha == 0 is a no-op. */
int32_t bsp_g2d_fill_alpha(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                         int32_t argb_w, int32_t argb_h,
                         int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color)
{
    int32_t rx = x, ry = y, rw = w, rh = h;
    uint32_t phys = 0;

    if (((color >> 24) & 0xff) == 0)
        return 0;
    if (!argb || argb_w <= 0 || argb_h <= 0 || w <= 0 || h <= 0)
        return -1;
    if (!gpu_clip_rect(&rx, &ry, &rw, &rh, argb_w, argb_h))
        return 0;
    if (gpu_ok(argb_w, argb_h))
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
    if (!phys)
        return -1;
    return gpu_fill_alpha_surface(phys, argb, argb_w, argb_h,
                                  rx, ry, rx + rw, ry + rh,
                                  color) == GPU_DONE ? 0 : -1;
}

/* CPU back end for sub-alignment tails and narrow copies: scalar 1:1
 * copy or blend with exact per-pixel access, no alignment, contiguity
 * or GPU requirements.  The 1:1 rect is clipped against both buffer
 * bounds first (callers hand in unclipped window rects: a row that
 * overruns the right edge would otherwise wrap into the left edge of
 * the next row).  use_alpha == 0 is a plain copy; otherwise the same
 * math as bsp_g2d_blt_alpha (effective alpha (src_a * alpha) >> 8,
 * then the /255 blend). */
int32_t bsp_g2d_blt_cpu(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                      int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                      uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                      int32_t dx, int32_t dy, uint8_t use_alpha,
                      uint8_t alpha)
{
    if (argb_src == NULL || argb_dst == NULL || sw <= 0 || sh <= 0)
        return 0;
    if (use_alpha != 0 && alpha == 0)
        return 0;

    /* 1:1 mapping: cutting one side shifts the other surface's origin
     * by the same delta, cutting right/bottom just shrinks the size */
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

int32_t bsp_g2d_scale_to(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                       int32_t src_w, int32_t src_h,
                       uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                       int32_t dst_w, int32_t dst_h)
{
    g2d_map_t m;
    uint32_t src_phys = 0, dst_phys = 0;
    int gr = GPU_UNSUPPORTED;

    if (!argb_src || !argb_dst || argb_src == argb_dst ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return -1;
    if (gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        /* Corner-preserving nearest map (scale_to semantics, same Q15
         * contract as raspi5 and the g2dtest scale_tl/scale_br/
         * scale_to_data oracle): u = (X*pu)>>15 with
         * pu = ceil(((sw-1)<<15)/(dw-1)), so X = 0 samples column 0 and
         * X = dw-1 samples column sw-1.  Rounded UP so the corner product
         * never lands below (sw-1)<<15 and overshoots it by < 2^15, hence
         * no sample leaves the source and no_clamp stays safe.  This is
         * deliberately NOT g2d_map_params: that is the blt rect map
         * u = X*sw/dw, whose floor walk stops short of the last source
         * column but is exact for integer ratios. */
        m.pu = (dst_w > 1) ? (int32_t)((((int64_t)(src_w - 1) << 15) +
                                       dst_w - 2) / (dst_w - 1)) : 0;
        m.qu = 0;
        m.cu = 0;
        m.pv = 0;
        m.qv = (dst_h > 1) ? (int32_t)((((int64_t)(src_h - 1) << 15) +
                                       dst_h - 2) / (dst_h - 1)) : 0;
        m.cv = 0;
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            gr = gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                  dst_phys, argb_dst, dst_w, dst_h,
                                  0, 0, dst_w, dst_h, 1);
            if (gr == GPU_DONE)
                return 0;
            if (gr == GPU_FAILED)
                return -1;
        }
    }
    return -1;
}

int32_t bsp_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                           int32_t *dst_w, int32_t *dst_h)
{
    g2d_rotated_size(src_w, src_h, degree, dst_w, dst_h);
    return 0;
}

int32_t bsp_g2d_rotate(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                     int32_t src_w, int32_t src_h,
                     uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                     int32_t dst_w, int32_t dst_h, int32_t degree)
{
    g2d_map_t m;
    int32_t rot = g2d_norm_degree(degree);
    int32_t bw, bh;
    uint32_t src_phys = 0, dst_phys = 0;
    int gr = GPU_UNSUPPORTED;

    /* GPU: any angle, with a destination exactly matching the rotated
     * bounding box.  The exact-size rule keeps the public API deterministic
     * and matches g2dtest's bad-size rejection contract.
     * In-place is NOT supported on the GPU: the affine walk covers the
     * destination in ascending row-major order, so the map's reads would
     * hit pixels the walk has already overwritten; in-place remains an
     * explicit failure. */
    if (!argb_src || !argb_dst || argb_src == argb_dst ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return -1;
    if (gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        g2d_rotated_size(src_w, src_h, rot, &bw, &bh);
        if (bw > 0 && bh > 0 && dst_w == bw && dst_h == bh) {
            /* the right-angle fast path runs first and declines
             * (GPU_UNSUPPORTED) on VC4 and for every other angle, so the
             * generic affine map below remains the general case */
            gr = gpu_rot90_surface(src_phys, argb_src, src_w, src_h,
                                   dst_phys, argb_dst, dst_w, dst_h, rot);
            if (gr == GPU_DONE)
                return 0;
            if (gr == GPU_FAILED)
                return -1;
            g2d_map_rotate(src_w, src_h, rot, bw, bh, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                gr = gpu_rotate_surface(&m, src_phys, argb_src, src_w, src_h,
                                        dst_phys, argb_dst, dst_w, dst_h);
                if (gr == GPU_DONE)
                    return 0;
                if (gr == GPU_FAILED)
                    return -1;
            }
        }
    }
    return -1;
}
/* in-place separable gaussian blur of a sub-rect, as a 3-tier degrade:
   1. the V3D 4.2 GPU blur (gpu_gaussian_blur_op) - real acceleration on
      Pi4, bit-exact with the client's reference for r1..r4 and for the
      composed r6 (= r4-r4-r2).  A large whole-canvas blur (area >= 256^2,
      radius >= 2, Pi4 only) first tries a half-res chain - downsample 2x,
      blur at the SAME radius, upsample 2x (~4x fewer pixels) - that trades
      bit-exactness for speed behind a frost; it commits the canvas only at
      the final upsample, so any earlier failure falls through to the exact
      kernels with the canvas pristine.  g2dtest's r1-r6 correctness canvas
      (320x200 = 64000) sits below the gate and stays bit-exact;
   2. else the arch NEON engine (arch_g2d_gaussian, ARCH_BOOST), which
      blurs in place, clips the rect to the buffer bounds and allocates its
      own scratch - this covers Pi3 (VC4 has no gauss kernel), a surface
      that is not GPU-visible, and any rect the GPU declines as ineligible;
   3. else decline as a missing capability (sticky G2D_ERR_NOT_SUPPORTED)
      so a build with no NEON engine linked into the driver lets the client
      keep its own software blur, rather than the non-BOOST
      arch_g2d_gaussian stub claiming success on an unblurred canvas.
   A GPU_FAILED (a dispatch was submitted and then errored/timed out) is
   returned as-is and NEVER replayed on NEON here: the GPU may still own or
   have partially written the canvas.  G2D_ERR_FAILED does not latch the
   client's capability, so the GPU path is retried on the next blur. */
int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h,
			int32_t rect_x, int32_t rect_y,
			int32_t rect_w, int32_t rect_h,
			int32_t radius) {
	uint32_t phys = 0;
	uint32_t scratch_phys = 0;
	/* scratch row pitch = canvas pitch, padded by one 64 B cache line when
	   that pitch is 4 KiB-aligned, so the GPU V pass's (2r+1)-row gather
	   lands in distinct L2T sets instead of aliasing onto one. this sizing
	   MUST match gpu_gaussian_blur_op in vc_g2d.c and the client's
	   graph_g2d.c blur_tmp_get allocation. */
	uint32_t argb_pitch = (uint32_t)argb_w * 4u;
	uint32_t tmp_pitch = ((argb_pitch & 4095u) == 0u) ? (argb_pitch + 64u)
	                                                  : argb_pitch;
	size_t tmp_need = (size_t)(rect_h - 1) * tmp_pitch + (size_t)rect_w * 4u;

	if(argb != NULL && tmp != NULL &&
			radius >= 1 && radius <= 64 &&
			gpu_ok(argb_w, argb_h) &&
			rect_x >= 0 && rect_y >= 0 && rect_w > 0 && rect_h > 0 &&
			rect_x <= argb_w - rect_w && rect_y <= argb_h - rect_h) {
		phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
		scratch_phys = gpu_phys(tmp_phy, tmp_need, tmp_contig);
	}
	if(phys != 0 && scratch_phys != 0) {
		/* Half-res fast path (quality-for-speed), mirroring raspi5: for a
		   whole-canvas blur big enough that the blur dominates the two extra
		   scale dispatches, downsample 2x, blur the half-res image at the
		   SAME radius (never halved - the caller's quality knob still governs
		   the spread), then upsample 2x back.  Processing 1/4 the pixels is
		   the entire speedup; the accepted trade-off is a softer, point-sampled
		   (decimate down / nearest up) result that is NOT bit-exact, which is
		   fine behind a translucent frost.  Gated to V3D 4.2 (Pi4), a
		   whole-canvas rect, area >= 65536 (>= 256^2) and radius >= 2, so the
		   g2dtest r1-r6 correctness canvas (320x200 = 64000) stays on the
		   exact kernels and only the PERF/bench groups (640x480 and up) take
		   this path.

		   Buffer reuse, no extra allocation: the half-res image lives at tmp[0]
		   (hw*hh*4 bytes) and the half-res blur's scratch at tmp[off_b]; each
		   region is ~1/4 of tmp_need, so off_b + scratch_need fits inside the
		   scratch the client already allocated.  The REAL canvas is written
		   ONLY by the final upsample, so a down or blur failure (even a
		   submitted-dispatch timeout, which only ever touches tmp) leaves the
		   canvas pristine and falls through to the exact full-res blur below;
		   only once the upsample is submitted is the canvas committed, and a
		   GPU_FAILED there is returned as-is and never replayed. */
		if(gpu_gauss_available() &&
				radius >= 2 &&
				rect_x == 0 && rect_y == 0 &&
				rect_w == argb_w && rect_h == argb_h &&
				argb_w >= 64 && argb_h >= 64 &&
				(int64_t)argb_w * (int64_t)argb_h >= 65536) {
			int32_t hw = argb_w / 2, hh = argb_h / 2;
			uint32_t half_pitch = (uint32_t)hw * 4u;
			uint32_t half_tmp_pitch = ((half_pitch & 4095u) == 0u)
			                              ? (half_pitch + 64u) : half_pitch;
			size_t off_b = ((size_t)hw * (size_t)hh * 4u + 63u) &
			               ~(size_t)63u;
			size_t scratch_need = (size_t)(hh - 1) * half_tmp_pitch +
			                      (size_t)hw * 4u;
			g2d_map_t m;
			if(off_b + scratch_need <= tmp_need) {
				uint32_t* tmp_b = (uint32_t*)((uint8_t*)tmp + off_b);
				uint32_t scratch_b_phys = scratch_phys + (uint32_t)off_b;
				/* down: canvas -> tmp[0] as a packed hw x hh surface */
				g2d_map_params(0, 0, argb_w, argb_h, 0, 0, hw, hh,
				               G2D_BSP_MAP_ROT_0, &m);
				if(gpu_map_fits(&m, ((int64_t)hw + 15) / 16 * 16, hh) &&
						gpu_blit_surface(&m, phys, argb, argb_w, argb_h,
						                 scratch_phys, tmp, hw, hh,
						                 0, 0, hw, hh, 1) == GPU_DONE) {
					/* blur tmp[0] at the FULL radius; scratch = tmp[off_b] */
					if(gpu_gaussian_blur_op(scratch_phys, tmp,
					                        scratch_b_phys, tmp_b,
					                        hw, hh, 0, 0, hw, hh,
					                        radius) == GPU_DONE) {
						/* up: tmp[0] (hw x hh) -> canvas */
						int ur = GPU_UNSUPPORTED;
						g2d_map_params(0, 0, hw, hh, 0, 0, argb_w, argb_h,
						               G2D_BSP_MAP_ROT_0, &m);
						if(gpu_map_fits(&m,
						                ((int64_t)argb_w + 15) / 16 * 16,
						                argb_h))
							ur = gpu_blit_surface(&m, scratch_phys, tmp,
							                    hw, hh, phys, argb,
							                    argb_w, argb_h,
							                    0, 0, argb_w, argb_h, 1);
						if(ur == GPU_DONE)
							return 0;
						if(ur == GPU_FAILED)
							return G2D_ERR_FAILED;  /* canvas committed */
						/* GPU_UNSUPPORTED / map did not fit: nothing was
						   submitted, the canvas is still pristine, so fall
						   through to the exact full-res blur. */
					}
				}
			}
			/* gate not met, scratch too small, or the down/blur failed before
			   the canvas was touched: fall through to the exact full-res GPU
			   blur (and then NEON). */
		}
		{
			int gr = gpu_gaussian_blur_op(phys, argb, scratch_phys, tmp,
					argb_w, argb_h,
					rect_x, rect_y, rect_w, rect_h, radius);
			if(gr == GPU_DONE)
				return 0;
			if(gr == GPU_FAILED)
				return G2D_ERR_FAILED;   /* never replay a submitted dispatch */
			/* GPU_UNSUPPORTED: declined before submitting anything, so the
			   NEON engine below runs on a still-untouched canvas. */
		}
	}
#ifdef ARCH_BOOST
	/* VC4 (Pi3) has no GPU gauss kernel, so the NEON engine runs.  Mirror the
	   GPU's radius > 4 decomposition (greedy {16,9,4,1}, so r6 = r4-r4-r2) by
	   blurring each stage in place: every arch_g2d_gaussian stage is bit-exact
	   with the g2dtest reference for its radius, so the composition is bit-exact
	   with the reference's composed r6, and Pi3 matches Pi4's GPU output for any
	   radius.  radius <= 4 stays a single pass (already bit-exact).  The 264-entry
	   stages bound covers the validated radius <= 64 (max 262 stages); a larger
	   radius is left to the single pass, which gaussian_blur_neon ignores above
	   its own radius-32 cap. */
	if(radius > 4 && radius <= 64) {
		int stages[264];
		int ns = gpu_gauss_decompose(radius, stages);
		int si;
		for(si = 0; si < ns; si++)
			arch_g2d_gaussian(argb, argb_phy, contig, argb_w, argb_h,
					rect_x, rect_y, rect_w, rect_h, stages[si]);
		return 0;
	}
	return arch_g2d_gaussian(argb, argb_phy, contig, argb_w, argb_h,
			rect_x, rect_y, rect_w, rect_h, radius);
#else
	return G2D_ERR_NOT_SUPPORTED;
#endif
}
