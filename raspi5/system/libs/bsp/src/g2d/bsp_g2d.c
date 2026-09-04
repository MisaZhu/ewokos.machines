/*
 * bsp_g2d.c - raspberry-pi5 g2d public API: argument validation, rect
 * clipping and the scalar CPU path.  Every hardware operation is delegated
 * to the VideoCore back end in libvideocore (<videocore/vc_g2d.h>), which
 * owns the affine map construction shared with the QPU kernels, the
 * eligibility gate on caller-supplied physical addresses, the
 * large-surface batching policy and every dispatch.
 *
 * The hardware operations are GPU-only: a back-end call returns non-zero on
 * success and 0 when the operation was not eligible or the dispatch failed,
 * and both surface as -1 here.  There is no implicit CPU fallback for
 * fill/blit/scale/rotate - a submitted dispatch is never replayed, because
 * a timed-out dispatch may still own or have partially written the
 * destination.
 *
 * bsp_g2d_fill_alpha is the explicit CPU path: exact per-pixel access with
 * no alignment, contiguity, SIMD or GPU requirement, for callers that
 * cannot satisfy the GPU eligibility gate.
 */

#include <bsp/bsp_g2d.h>

#include <videocore/vc_g2d.h>

/* ------------------------------------------------------------------ */
/* bsp_g2d API                                                         */
/* ------------------------------------------------------------------ */

int32_t bsp_g2d_init(void)
{
    return vc_g2d_init();
}

/* V3D clock rate in Hz confirmed at init; 0 when unknown/unsupported */
uint32_t bsp_g2d_clock_hz(void)
{
    return vc_g2d_clock_hz();
}

int32_t bsp_g2d_fill(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                   int32_t argb_w, int32_t argb_h,
                   int32_t x, int32_t y, int32_t w, int32_t h,
                   uint32_t color)
{
    int32_t rx = x, ry = y, rw = w, rh = h;
    uint32_t phys = 0;

    if (argb &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, argb_w, argb_h) &&
        gpu_ok(argb_w, argb_h)) {
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
    }
    if (!phys)
        return -1;
    /* Never replay a submitted operation on the CPU: a timed-out
     * dispatch may still own or have partially written dst. */
    return gpu_fill_surface(phys, argb, argb_w, argb_h, rx, ry,
                            rx + rw, ry + rh, color) ? 0 : -1;
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

    if (argb_src && argb_dst && argb_src != argb_dst &&
        sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h) &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        /* identity 1:1 rect: the vec4 copy kernel moves 4x the bytes per
         * TMU request; ineligible geometry falls through to argb_blit */
        if (sw == dw && sh == dh) {
            int32_t csx = sx + (rx - dx), csy = sy + (ry - dy);
            if (gpu_copy_eligible(src_phys, src_w, src_h, csx, csy,
                                  dst_phys, dst_w, rx, rw, rh))
                return gpu_copy_op(src_phys, argb_src, src_w, src_h,
                                   dst_phys, argb_dst, dst_w,
                                   csx, csy, rx, ry, rx + rw, ry + rh,
                                   (size_t)dst_w * dst_h * 4) ? 0 : -1;
        }
        g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h))
            return gpu_blit_op(&m, src_phys, argb_src, src_w, src_h,
                               dst_phys, argb_dst, dst_w, dst_h,
                               rx, ry, rx + rw, ry + rh,
                               (size_t)dst_w * dst_h * 4) ? 0 : -1;
    }
    return -1;
}

int32_t bsp_g2d_blt_phy(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                      int32_t src_w, int32_t src_h,
                      int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                      ewokos_addr_t dst_phy, uint32_t dst_size,
                      int32_t dst_w, int32_t dst_h, uint32_t dst_pitch,
                      int32_t dx, int32_t dy, int32_t dw, int32_t dh)
{
    g2d_map_t m;
    int32_t rx = dx, ry = dy, rw = dw, rh = dh;
    uint32_t src_phys = 0, dst_phys = 0;
    int32_t surf_w;
    size_t dst_bytes;

    if (dst_pitch < (uint32_t)dst_w * 4u || (dst_pitch & 3u) != 0 ||
        dst_size == 0)
        return -1;
    /* stride-unaware geometry would let the kernel run past the rows:
     * the last touched byte is the rect's bottom row end */
    if ((uint64_t)(dst_h - 1) * dst_pitch +
        (uint64_t)dst_w * 4u > dst_size)
        return -1;
    surf_w = (int32_t)(dst_pitch / 4u);
    dst_bytes = (size_t)(dst_h - 1) * dst_pitch + (size_t)dst_w * 4u;

    if (argb_src &&
        sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h) &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, dst_bytes, 1);
    }
    if (src_phys && dst_phys) {
        /* identity 1:1 rect straight to scanout: the vec4 copy kernel
         * (displayd's full-frame flush is exactly this shape) */
        if (sw == dw && sh == dh) {
            int32_t csx = sx + (rx - dx), csy = sy + (ry - dy);
            if (gpu_copy_eligible(src_phys, src_w, src_h, csx, csy,
                                  dst_phys, surf_w, rx, rw, rh))
                return gpu_copy_op(src_phys, argb_src, src_w, src_h,
                                   dst_phys, NULL, surf_w,
                                   csx, csy, rx, ry, rx + rw, ry + rh,
                                   (size_t)dst_h * dst_pitch) ? 0 : -1;
        }
        g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)surf_w + 15) / 16 * 16, dst_h))
            /* the surface is modelled pitch/4 pixels wide so each row
             * strides by dst_pitch; the rect sits in the visible
             * left-hand part and the in-rect lane gate keeps every
             * write inside it.  dst has no kernel-visible VA here (raw
             * physical range): the dispatch's flush covers visibility.
             * Never replay a submitted operation on the CPU. */
            return gpu_blit_op(&m, src_phys, argb_src, src_w, src_h,
                               dst_phys, NULL, surf_w, dst_h,
                               rx, ry, rx + rw, ry + rh,
                               (size_t)dst_h * dst_pitch) ? 0 : -1;
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

    if (alpha == 0)
        return 0;

    if (argb_src && argb_dst && argb_src != argb_dst &&
        sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h) &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
        if (src_phys && dst_phys) {
            g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh,
                               G2D_MAP_ROT_0, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h))
                return gpu_alpha_op(&m, alpha, src_phys, argb_src,
                                    src_w, src_h, dst_phys, argb_dst,
                                    dst_w, dst_h, rx, ry, rx + rw, ry + rh,
                                    (size_t)dst_w * dst_h * 4) ? 0 : -1;
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

/* CPU-only alpha fill of a sub-rect, clipped to the buffer bounds:
 * exact per-pixel access, no alignment/contiguity requirements;
 * same blend math as bsp_g2d_blt_alpha.  alpha == 0 is a no-op. */
int32_t bsp_g2d_fill_alpha(uint32_t *argb, int32_t argb_w, int32_t argb_h,
                         int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color)
{
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

int32_t bsp_g2d_scale_to(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                       int32_t src_w, int32_t src_h,
                       uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                       int32_t dst_w, int32_t dst_h)
{
    g2d_map_t m;
    uint32_t src_phys = 0, dst_phys = 0;

    if (argb_src && argb_dst && argb_src != argb_dst &&
        src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        /* Corner-preserving nearest map (scale_to semantics, see the
         * g2dtest scale_tl/scale_br checks): u = X*(sw-1)/(dw-1), so
         * X = 0 samples column 0 and X = dw-1 samples column sw-1 -
         * unlike the blt rect map u = X*sw/dw, whose floor walk stops
         * short of the last source column (800->320 sampled (797,597)
         * instead of (799,599)).  The Q15 coefficient is rounded UP:
         * truncation could leave the corner product one below
         * (sw-1)<<15, while ceil overshoots it by at most dw-2 < 2^15,
         * so (pu*(dw-1))>>15 lands exactly on sw-1 and no sample ever
         * leaves the source - the no_clamp fast path stays safe for
         * destinations up to 32769 pixels wide/tall. */
        m.pu = (dst_w > 1) ? (int32_t)((((int64_t)(src_w - 1) << 15) +
                                       dst_w - 2) / (dst_w - 1)) : 0;
        m.qu = 0;
        m.cu = 0;
        m.pv = 0;
        m.qv = (dst_h > 1) ? (int32_t)((((int64_t)(src_h - 1) << 15) +
                                       dst_h - 2) / (dst_h - 1)) : 0;
        m.cv = 0;
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h))
            return gpu_scale_op(&m, src_phys, argb_src, src_w, src_h,
                                dst_phys, argb_dst, dst_w, dst_h) ? 0 : -1;
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

    /* GPU: any angle, any destination size (the argb_rotate kernel writes
     * every destination pixel - content or transparent 0 - so a dst
     * larger than the rotated content box needs no pre-clear, and a dst
     * smaller than the box is simply clipped: the map is built against
     * the bw x bh content box and the walk stops at the dst edge, so
     * dst only ever shows the top-left corner of the rotated content).
     * In-place is NOT supported on the GPU: the affine walk covers the
     * destination in ascending row-major order, so the map's reads would
     * hit pixels the walk has already overwritten (proven on the Pi 5
     * for 180); with no CPU fallback an in-place call fails with -1. */
    if (argb_src && argb_dst && argb_src != argb_dst &&
        src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        /* Dedicated 90/270 fast path: contiguous reads, strided writes.
         * Exact-size right-angle rotation with src width % 16 == 0 (e.g.
         * 1280x720 and, unlike the affine path, 1920x1080 whose dst width
         * 1080 % 16 != 0) - falls through to the generic paths when not
         * eligible. */
        if ((rot == 90 || rot == 270) &&
            gpu_rot90_surface(src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h, rot))
            return 0;
        g2d_rotated_size(src_w, src_h, rot, &bw, &bh);
        if (bw > 0 && bh > 0) {
            g2d_map_rotate(src_w, src_h, rot, bw, bh, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h))
                return gpu_rotate_op(&m, rot, bw, bh,
                                     src_phys, argb_src, src_w, src_h,
                                     dst_phys, argb_dst, dst_w, dst_h) ? 0 : -1;
        }
    }
    return -1;
}
