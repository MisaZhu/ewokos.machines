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
 * bsp_g2d_fill_alpha and bsp_g2d_blt_cpu are the explicit CPU paths: exact
 * per-pixel access with no alignment, contiguity, SIMD or GPU requirement,
 * for callers that cannot satisfy the GPU eligibility gate.
 */

#include <bsp/bsp_g2d.h>

#include <string.h>

#include <videocore/vc_g2d.h>

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
        g2d_map_params(0, 0, src_w, src_h, 0, 0, dst_w, dst_h,
                           G2D_BSP_MAP_ROT_0, &m);
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
