/*
 * bsp_g2d.c - raspberry-pi5 g2d public API: argument validation and rect
 * clipping.  Every operation is delegated to the VideoCore back end in
 * libvideocore (<videocore/vc_g2d.h>), which owns the affine map
 * construction shared with the QPU kernels, the eligibility gate on
 * caller-supplied physical addresses, the large-surface batching policy
 * and every dispatch.
 *
 * The operations are GPU-only: a back-end call returns non-zero on
 * success and 0 when the operation was not eligible or the dispatch failed,
 * and both surface as -1 here.  There is no implicit CPU fallback for
 * fill/fill_alpha/blit/scale/rotate - a submitted dispatch is never
 * replayed, because a timed-out dispatch may still own or have partially
 * written the destination.
 */

#include <bsp/bsp_g2d.h>
#include <g2d_arch.h>
#include <g2dclient/g2dclient.h>

#include <videocore/vc_g2d.h>
#include <videocore/vc_pgl3d.h>
#include <ewoksys/klog.h>

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

int32_t bsp_g2d_cmd(int argc, char **argv, char *buf, uint32_t len)
{
    return vc_g2d_cmd(argc, argv, buf, len);
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

/* Right-angle rotation of a src rect straight into the scan-out: the
 * rotated rect's top-left lands at (dx,dy) of the pitched dst surface.
 * 90/270 take the argb_rot90 kernel with explicit strides (the src rect
 * is addressed inside its canvas, the dst inside the scan-out by pitch);
 * geometries the fast path refuses (rect height not splittable into
 * 4-row strips over >= 4 QPUs, 270 with width % 16 != 0) and 180 take
 * the affine blit through a rotate map, which can only model a packed
 * source - so those need the rect to span full canvas rows (sx == 0,
 * sw == src_w).  Anything else is declined before any write (-1), the
 * caller keeps its own path.  Never replay a submitted dispatch. */
int32_t bsp_g2d_rotate_phy(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                         int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         ewokos_addr_t dst_phy, uint32_t dst_size,
                         int32_t dst_w, int32_t dst_h, uint32_t dst_pitch,
                         int32_t dx, int32_t dy, int32_t degree)
{
    g2d_map_t m;
    int32_t rot = g2d_norm_degree(degree);
    int32_t rw, rh;
    int32_t surf_w;
    uint32_t src_phys = 0, dst_phys = 0;
    size_t src_off, dst_off, dst_bytes;

    if (rot != 90 && rot != 180 && rot != 270)
        return -1;
    if (!argb_src || src_w <= 0 || src_h <= 0 || sw <= 0 || sh <= 0 ||
        sx < 0 || sy < 0 || sx > src_w - sw || sy > src_h - sh)
        return -1;
    if (dst_pitch < (uint32_t)dst_w * 4u || (dst_pitch & 3u) != 0 ||
        dst_size == 0 || !gpu_ok(dst_w, dst_h))
        return -1;
    g2d_rotated_size(sw, sh, rot, &rw, &rh);
    if (rw <= 0 || rh <= 0 || dx < 0 || dy < 0 ||
        dx > dst_w - rw || dy > dst_h - rh)
        return -1;
    /* the touched rows must fit the declared physical segment */
    dst_off = (size_t)dy * dst_pitch + (size_t)dx * 4u;
    dst_bytes = (size_t)(rh - 1) * dst_pitch + (size_t)rw * 4u;
    if (dst_off + dst_bytes > dst_size)
        return -1;
    src_off = ((size_t)sy * (size_t)src_w + (size_t)sx) * 4u;

    src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
    dst_phys = gpu_phys(dst_phy, dst_off + dst_bytes, 1);
    if (!src_phys || !dst_phys)
        return -1;
    src_phys += (uint32_t)src_off;
    dst_phys += (uint32_t)dst_off;

    if ((rot == 90 || rot == 270) &&
        gpu_rot90_rect(src_phys, (uint32_t *)((uint8_t *)argb_src + src_off),
                       sw, sh, src_w * 4,
                       dst_phys, NULL, rw, rh, (int32_t)dst_pitch, rot))
        return 0;

    /* affine fallback: packed source rows only */
    if (sx != 0 || sw != src_w)
        return -1;
    surf_w = (int32_t)(dst_pitch / 4u);
    g2d_map_rotate(sw, sh, rot, rw, rh, &m);
    if (!gpu_map_fits(&m, ((int64_t)surf_w + 15) / 16 * 16, rh))
        return -1;
    /* the dst is modelled pitch/4 pixels wide from the rect's top-left
     * so each row strides by dst_pitch; the in-rect lane gate keeps
     * every write inside [0,rw) x [0,rh).  Exact right-angle maps stay
     * inside the content box, so argb_blit's clamp never engages. */
    return gpu_blit_op(&m, src_phys, (uint32_t *)((uint8_t *)argb_src + src_off),
                       sw, sh, dst_phys, NULL, surf_w, rh,
                       0, 0, rw, rh, dst_bytes) ? 0 : -1;
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

/* Translucent colour fill of a sub-rect: GPU-only like the other
 * operations (the argb_alpha kernel over a constant-colour source, see
 * gpu_fill_alpha_op), same eligibility gate as bsp_g2d_fill.  alpha == 0
 * is a no-op.  Blends are not idempotent, so a failed dispatch is never
 * replayed. */
int32_t bsp_g2d_fill_alpha(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                         int32_t argb_w, int32_t argb_h,
                         int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t color)
{
    int32_t rx = x, ry = y, rw = w, rh = h;
    uint32_t phys = 0;

    if (((color >> 24) & 0xff) == 0)
        return 0;
    if (argb &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, argb_w, argb_h) &&
        gpu_ok(argb_w, argb_h)) {
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
    }
    if (!phys)
        return -1;
    return gpu_fill_alpha_op(phys, argb, argb_w, argb_h, rx, ry,
                             rx + rw, ry + rh, color) ? 0 : -1;
}

/* Filled triangle: GPU-only like the other operations (the dedicated
 * argb_triangle CSD kernel rasterizes the three edge half-planes with the
 * doubled + asymmetric half-pixel bias, see gpu_fill_triangle_op), same
 * eligibility gate as bsp_g2d_fill_alpha.  alpha == 0 is a no-op and the
 * vertices may lie off-surface (the bbox is clipped and the kernel clamps
 * every write inside).  Blends are not idempotent, so a failed dispatch is
 * reported, never replayed on the CPU. */
int32_t bsp_g2d_fill_triangle(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
                         int32_t argb_w, int32_t argb_h,
                         int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         int32_t x3, int32_t y3, uint32_t color)
{
    uint32_t phys = 0;

    if (((color >> 24) & 0xff) == 0)
        return 0;
    if (argb && gpu_ok(argb_w, argb_h))
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
    if (!phys)
        return -1;
    return gpu_fill_triangle_op(phys, argb, argb_w, argb_h,
                                x1, y1, x2, y2, x3, y3, color) ? 0 : -1;
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
         * g2dtest scale_tl/scale_br/scale_to_data checks): the contract
         * is the Q15 form the kernel evaluates, u = (X*pu)>>15 with
         * pu = ceil(((sw-1)<<15)/(dw-1)), so X = 0 samples column 0 and
         * X = dw-1 samples column sw-1 - unlike the blt rect map
         * u = X*sw/dw, whose floor walk stops short of the last source
         * column (800->320 sampled (797,597) instead of (799,599)).
         * The coefficient is rounded UP: truncation could leave the
         * corner product one below (sw-1)<<15, while ceil overshoots it
         * by at most dw-2 < 2^15, so (pu*(dw-1))>>15 lands exactly on
         * sw-1 and no sample ever leaves the source - the no_clamp fast
         * path stays safe for destinations up to 32769 pixels wide/tall.
         * Note the quantised map is NOT floor(X*(sw-1)/(dw-1)) at every
         * X (800->320 differs at X=212, 531 vs 530.997); no integer Q15
         * slope can reproduce that rational's floor for all X (the
         * admissible interval is [82074.08, 82074.57) there), so the
         * Q15 form is what the test mirrors. */
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

int32_t bsp_g2d_gaussian_blur(uint32_t* argb, ewokos_addr_t argb_phy, uint8_t contig,
			uint32_t* tmp, ewokos_addr_t tmp_phy, uint8_t tmp_contig,
			int32_t argb_w, int32_t argb_h,
			int32_t rect_x, int32_t rect_y,
			int32_t rect_w, int32_t rect_h,
			int32_t radius)
{
   	//return G2D_ERR_NOT_SUPPORTED; //TODO
    uint32_t phys = 0;
    uint32_t scratch_phys = 0;
    /* Scratch row pitch = canvas pitch, padded by one 64 B cache line when
     * that pitch is 4 KiB-aligned, so the V pass's (2r+1)-row gather lands
     * in distinct L2T sets instead of all aliasing onto one (measured
     * 1024^2 r4 = 29.6 ms vs NEON 16.2 ms).  This sizing MUST match what
     * gpu_gaussian_blur_op writes in vc_g2d.c, and graph_g2d.c's
     * blur_tmp_get allocation MUST be at least this large. */
    uint32_t argb_pitch = (uint32_t)argb_w * 4u;
    uint32_t tmp_pitch = ((argb_pitch & 4095u) == 0u) ? (argb_pitch + 64u)
                                                      : argb_pitch;
    size_t tmp_need = (size_t)(rect_h - 1) * tmp_pitch + (size_t)rect_w * 4u;

    if (argb && tmp &&
        radius >= 1 && radius <= 64 &&
        gpu_ok(argb_w, argb_h) &&
        rect_x >= 0 && rect_y >= 0 && rect_w > 0 && rect_h > 0 &&
        rect_x <= argb_w - rect_w && rect_y <= argb_h - rect_h) {
        phys = gpu_phys(argb_phy, (size_t)argb_w * argb_h * 4, contig);
        scratch_phys = gpu_phys(tmp_phy, tmp_need, tmp_contig);
    }
    if (!phys || !scratch_phys) {
        klog("g2d blur: surface not GPU-visible (phys=%08x scratch=%08x "
             "contig=%u/%u w=%d h=%d r=%d)\n",
             phys, scratch_phys, contig, tmp_contig, argb_w, argb_h,
             radius);
        return -1;
    }
    /* Half-res fast path (quality-for-speed).  The caller's radius is passed
     * through UNCHANGED - it is never halved - so the only quality knob the
     * caller set still governs the blur.  The speedup comes purely from
     * processing 1/4 the pixels: downsample the canvas 2x, blur the half-res
     * image at that same radius, then upsample 2x back.  The accepted
     * trade-off (the caller opted into "rougher but faster"): a radius-R blur
     * on a half-res image spreads over ~2R full-res pixels, and the pow2
     * down / nearest up are point-sampled, so the result is softer and not
     * bit-exact.  That is fine behind a translucent frost, so this path is
     * gated to whole-canvas blurs (gpu_scale_op is whole-surface) large
     * enough that the blur dominates the two extra scale dispatches.
     *
     * Buffer reuse, no extra allocation: the half-res image lives at tmp[0]
     * (hw*hh*4 bytes) and the half-res blur's scratch at tmp[off_b]; each
     * region is ~1/4 of the full-res tmp_need, so off_b + scratch_need fits.
     * The half-res blur is handed tmp[0] as its canvas and tmp[off_b] as its
     * scratch - the REAL canvas is written only by the final upsample.  So a
     * down or blur failure (even a submitted-dispatch timeout) leaves the
     * canvas pristine and falls through to the exact blur below; only once the
     * upsample is submitted is the canvas committed (never replayed). */
    if (radius >= 2 &&
        rect_x == 0 && rect_y == 0 &&
        rect_w == argb_w && rect_h == argb_h &&
        argb_w >= 64 && argb_h >= 64 &&
        (int64_t)argb_w * (int64_t)argb_h >= 65536) {
        int32_t hw = argb_w / 2, hh = argb_h / 2;
        uint32_t half_pitch = (uint32_t)hw * 4u;
        uint32_t half_tmp_pitch = ((half_pitch & 4095u) == 0u)
                                      ? (half_pitch + 64u) : half_pitch;
        size_t off_b = ((size_t)hw * (size_t)hh * 4u + 63u) & ~(size_t)63u;
        size_t scratch_need = (size_t)(hh - 1) * half_tmp_pitch +
                              (size_t)hw * 4u;
        g2d_map_t m;
        if (off_b + scratch_need <= tmp_need) {
            uint32_t *tmp_b = (uint32_t *)((uint8_t *)tmp + off_b);
            uint32_t scratch_b_phys = scratch_phys + (uint32_t)off_b;
            /* down: canvas -> tmp[0] as a packed hw x hh surface */
            g2d_map_params(0, 0, argb_w, argb_h, 0, 0, hw, hh,
                           G2D_MAP_ROT_0, &m);
            if (gpu_map_fits(&m, ((int64_t)hw + 15) / 16 * 16, hh) &&
                gpu_scale_op(&m, phys, argb, argb_w, argb_h,
                             scratch_phys, tmp, hw, hh)) {
                /* blur tmp[0] at the FULL radius; scratch = tmp[off_b] */
                if (gpu_gaussian_blur_op(scratch_phys, tmp,
                                         scratch_b_phys, tmp_b,
                                         hw, hh, 0, 0, hw, hh, radius) == 0) {
                    /* up: tmp[0] (hw x hh) -> canvas */
                    g2d_map_params(0, 0, hw, hh, 0, 0, argb_w, argb_h,
                                   G2D_MAP_ROT_0, &m);
                    if (gpu_map_fits(&m, ((int64_t)argb_w + 15) / 16 * 16,
                                     argb_h) &&
                        gpu_scale_op(&m, scratch_phys, tmp, hw, hh,
                                     phys, argb, argb_w, argb_h))
                        return 0;
                    klog("g2d blur: half-res upsample failed (w=%d h=%d r=%d)\n",
                         argb_w, argb_h, radius);
                    return -1;
                }
            }
        }
        /* gate not met, scratch too small, or down/blur failed before the
         * canvas was touched: fall through to the exact full-res blur. */
    }
    //Never replay a submitted operation on the CPU: a timed-out
    //dispatch may still own or have partially written dst.
    {
        int rc = gpu_gaussian_blur_op(phys, argb, scratch_phys, tmp,
                                      argb_w, argb_h,
                                      rect_x, rect_y, rect_w, rect_h,
                                      radius);
        if (rc == 0)
            return 0;
        // rc: 1 = CSD poll timeout, -1 = fault/gate inside the op
        klog("g2d blur: dispatch failed rc=%d (w=%d h=%d rect %d,%d %dx%d "
             "r=%d)\n", rc, argb_w, argb_h,
             rect_x, rect_y, rect_w, rect_h, radius);
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* portablegl GPU (V3D 7.1 3D-pipeline) offload                        */
/* ------------------------------------------------------------------ */

/* The 3D pipeline is brought up lazily on the first pgl op (idempotent,
 * on top of the CSD path's V3D init that bsp_g2d_init already ran) and
 * latched: once vc_pgl3d_init() declines, we never retry, so a portablegl
 * client sees one sticky G2D_ERR_NOT_SUPPORTED and stays on its CPU
 * rasterizer.  These two flags are only touched from g2dd's serialized
 * dev_cntl path for a given requester; the vc_pgl3d layer owns the real
 * GPU-dispatch lock. */
static int _pgl3d_ready;
static int _pgl3d_declined;

static int pgl3d_ensure(void)
{
    if (_pgl3d_ready)
        return 1;
    if (_pgl3d_declined)
        return 0;
    if (vc_pgl3d_init() == VC_PGL3D_DONE) {
        _pgl3d_ready = 1;
        return 1;
    }
    _pgl3d_declined = 1;
    return 0;
}

/* Map the vc_pgl3d tri-state onto the g2d result codes g2dd passes back
 * verbatim: DONE -> G2D_OK, FAILED -> G2D_ERR_FAILED (transient), and
 * UNSUPPORTED -> G2D_ERR_NOT_SUPPORTED (sticky, the client falls back). */
static int32_t pgl3d_rc(int r)
{
    switch (r) {
    case VC_PGL3D_DONE:
        return G2D_OK;
    case VC_PGL3D_FAILED:
        return G2D_ERR_FAILED;
    default: /* VC_PGL3D_UNSUPPORTED */
        return G2D_ERR_NOT_SUPPORTED;
    }
}

int32_t bsp_g2d_pgl_clear(uint32_t *fb, ewokos_addr_t fb_phy, uint8_t contig,
                          int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
                          uint32_t color, int32_t rb_swap)
{
    /* the GPU clears through the physical base only; fb (the render-target
     * VA) is not touched here. */
    (void)fb;
    if (!pgl3d_ensure())
        return G2D_ERR_NOT_SUPPORTED;
    return pgl3d_rc(vc_pgl3d_clear(fb_phy, contig, stride,
                                   (uint32_t)w, (uint32_t)h, fmt, color,
                                   rb_swap));
}

int32_t bsp_g2d_pgl_draw(ewokos_addr_t fb_phy, uint8_t fb_contig,
                         int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
                         uint32_t mode, uint32_t nverts,
                         ewokos_addr_t pos_phy, uint8_t pos_contig,
                         ewokos_addr_t col_phy, uint8_t col_contig,
                         uint32_t fs_id, uint32_t color, int32_t rb_swap,
                         uint32_t depth_func, int32_t depth_mask,
                         int32_t blend_en, int32_t cull_en,
                         ewokos_addr_t z_phy, uint8_t z_contig,
                         uint32_t z_stride, uint8_t first_frame,
                         uint32_t clear_rgba, float clear_z,
                         const void* ext, const void* tex)
{
    vc_pgl3d_draw_req_t req;

    if (!pgl3d_ensure())
        return G2D_ERR_NOT_SUPPORTED;

    memset(&req, 0, sizeof(req));
    req.mode       = mode;
    req.nverts     = nverts;
    req.pos_phy    = pos_phy;
    req.pos_contig = pos_contig;
    req.col_phy    = col_phy;
    req.col_contig = col_contig;
    req.fb_phy     = fb_phy;
    req.fb_contig  = fb_contig;
    req.stride     = stride;
    req.w          = (uint32_t)w;
    req.h          = (uint32_t)h;
    req.fmt        = fmt;
    req.fs_id      = fs_id;
    req.rgba       = color;
    req.rb_swap    = rb_swap;
    req.depth_func = depth_func;
    req.depth_mask = (uint8_t)depth_mask;
    req.blend_en   = (uint8_t)blend_en;
    req.cull_en    = (uint8_t)cull_en;
    /* persistent Z32F depth surface + frame boundary (Phase A2): z_phy == 0
     * falls back to tile-local Z; first_frame marks the frame's first
     * dispatch (clears colour to clear_rgba + Z to clear_z), later dispatches
     * LOAD the persisted colour+Z so a multi-draw frame accumulates. */
    req.z_phy       = z_phy;
    req.z_contig    = z_contig;
    req.z_stride    = z_stride;
    req.first_frame = first_frame;
    req.clear_rgba  = clear_rgba;
    req.clear_z     = clear_z;
    /* Phase B2c extended render state (scissor / polygon offset / colour write
     * mask).  ext is an opaque pointer to the caller's g2d_pgl_ext_t (the g2dd
     * handler passes &req.ext); a NULL ext from an older caller leaves every
     * field zero = scissor-off / no-offset / write-all, matching the memset. */
    if (ext) {
        const g2d_pgl_ext_t* e = (const g2d_pgl_ext_t*)ext;
        req.scissor_x   = e->scissor_x;
        req.scissor_y   = e->scissor_y;
        req.scissor_w   = e->scissor_w;
        req.scissor_h   = e->scissor_h;
        req.poly_enable = e->poly_enable;
        req.poly_factor = e->poly_factor;
        req.poly_units  = e->poly_units;
        req.color_mask  = e->color_mask;
    }
    /* Phase D texture state (fs_id == G2D_PGL_FS_TEX_MOD).  tex is an opaque
     * pointer to the caller's g2d_pgl_tex_t (the g2dd handler passes a filled
     * struct when it could resolve tex + uv canvases); a NULL tex from an
     * older caller leaves every field zero = the back end declines TEX_MOD
     * back to UNSUPPORTED, matching the memset. */
    if (tex) {
        const g2d_pgl_tex_t* t = (const g2d_pgl_tex_t*)tex;
        req.tex_phy    = t->tex_phy;
        req.tex_contig = t->tex_contig;
        req.tex_stride = t->tex_stride;
        req.tex_w      = t->tex_w;
        req.tex_h      = t->tex_h;
        req.uv_phy     = t->uv_phy;
        req.uv_contig  = t->uv_contig;
    }
    return pgl3d_rc(vc_pgl3d_draw(&req));
}
