/*
 * arch_g2d.c - raspberry-pi5 arch_g2d back end: V3D-accelerated offline
 * ARGB8888 drawing with a documented CPU compatibility path, ported from
 * the bare-metal g2d library (proven on real Pi 5 hardware).
 *
 * The V3D CSD kernels (see v3d_g2d.c) scan the destination in 16-pixel
 * groups; the kernels carry the destination rectangle in uniforms and
 * gate their TMU writes with a per-lane in-rect test.  A final partial
 * 16-pixel group is safe because lanes beyond the surface width are
 * redirected to the TMU scratch block:
 *
 *   arch_g2d_fill       any clipped rect        -> argb_fill kernel
 *   arch_g2d_blt        any clipped dst rect    -> argb_blit kernel
 *   arch_g2d_scale_to   whole dst               -> argb_blit kernel
 *   arch_g2d_rotate     any angle, any dst size -> argb_rotate kernel
 *                        (dst < box: clipped to dst top-left)
 *
 * arch_g2d_blt_alpha uses a dedicated source-over kernel.  Its two-read
 * pipeline rect-gates BOTH streams: the destination read of an
 * out-of-rect lane (tail lane of an unaligned row, lane past the rect
 * edge) is rerouted to the lane-private scratch word, so any destination
 * width is supported on the GPU.
 *
 * The rotate kernel runs the same affine engine as blit for every angle
 * (the canonical centres-based map degenerates exactly to the right-angle
 * coefficient sets) and writes every destination pixel: rotated content,
 * or transparent 0 for pixels whose pre-clamp source coordinate is out of
 * range (a five-instruction flag chain, then `mov ifna <data>, 0` right
 * after the LDTMU).  The clamp is kept so out-of-range TMU reads stay in
 * the source buffer.
 *
 * GPU eligibility (on top of the width/size checks):
 *   - the *_contig flag is set and the matching *_phy carries a valid
 *     physical base (the caller resolves it: contig shm slab / sys_dma
 *     memory); the physical address must fit the kernels' 32-bit TMU
 *     addresses.
 *
 * ZERO COPY: the GPU operates directly on the caller's buffers through
 * the caller-supplied physical bases; the kernels are preloaded once at
 * init and a dispatch only refreshes the small uniform block (see
 * v3d_g2d.c).
 */

#include <g2d_arch.h>

#include <string.h>

#include <ewoksys/klog.h>
#include "g2d_cpu.h"
#include "v3d_g2d.h"

#define G2D_MAX_COEF (1 << 23)  /* |map coefficient| must fit smul24 */

/* ------------------------------------------------------------------ */
/* GPU eligibility helpers                                             */
/* ------------------------------------------------------------------ */

/* The kernels' slice guard only tests the band START (H1 - qid*rows < 0
 * -> skip); a band that starts inside the surface but extends past H1
 * writes out of bounds.  Choosing nq so that H % nq == 0 makes every
 * band exactly H/nq rows (the guard then never fires), so dispatches can
 * never scribble past the surface.  Returns the largest divisor of H
 * that is <= nq (>= 1). */
static int g2d_fit_nq(int nq, int32_t H)
{
    if (nq > H)
        nq = H;
    while (nq > 1 && H % nq != 0)
        nq--;
    return nq;
}

static int gpu_ok(int32_t w, int32_t h)
{
    return v3d_g2d_ready() && w > 0 && h > 0;
}

static void log_cpu_fallback(const char *operation)
{
    slog("g2d: CPU fallback: %s\r\n", operation);
}

/* Validate a caller-provided physical base for the QPU's 32-bit TMU
 * addresses.  Returns the physical address usable by the kernels, or 0
 * when the canvas cannot run on the GPU (not physically contiguous, no
 * phy supplied, > 4 GB physical, or the address fails the RAM-range
 * validation gate). */
static uint32_t gpu_phys(ewokos_addr_t phys, size_t bytes, uint8_t contig)
{
    if (!contig)
        return 0;
    if (phys == 0 || (phys >> 32) != 0)
        return 0;
    if ((ewokos_addr_t)(uint32_t)phys + bytes < (uint32_t)phys)
        return 0;                       /* wrap */
    if (!v3d_g2d_phy_valid((ewokos_addr_t)(uint32_t)phys, bytes))
        return 0;
    return (uint32_t)phys;
}

static int gpu_clip_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                         int32_t surf_w, int32_t surf_h)
{
    int64_t x0 = *x < 0 ? 0 : *x;
    int64_t y0 = *y < 0 ? 0 : *y;
    int64_t x1 = (int64_t)*x + *w;          /* 64-bit: rect sums may overflow */
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

/* The kernel evaluates the map with two signed 24x24 multiplies added
 * into a 32-bit register: the coefficients must fit 24 bits and the
 * largest |pu|*X + |qu|*Y product must not wrap 32 bits. */
static int gpu_map_fits(const g2d_cpu_map_t *m, int64_t w, int64_t h)
{
    int64_t pu = m->pu, qu = m->qu, pv = m->pv, qv = m->qv;
    int64_t a, b, c, d;

    if (pu >= G2D_MAX_COEF || pu <= -G2D_MAX_COEF ||
        qu >= G2D_MAX_COEF || qu <= -G2D_MAX_COEF ||
        pv >= G2D_MAX_COEF || pv <= -G2D_MAX_COEF ||
        qv >= G2D_MAX_COEF || qv <= -G2D_MAX_COEF)
        return 0;
    a = pu < 0 ? -pu : pu;
    b = qu < 0 ? -qu : qu;
    c = pv < 0 ? -pv : pv;
    d = qv < 0 ? -qv : qv;
    if (a * w + b * h >= ((int64_t)1 << 30))
        return 0;
    if (c * w + d * h >= ((int64_t)1 << 30))
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* GPU dispatch helpers                                                */
/* ------------------------------------------------------------------ */

/* constant-color fill of a clipped dst rect (argb_fill kernel);
 * phys = physical base of the surface, argb = its virtual address */
static int gpu_fill_surface(uint32_t phys, uint32_t *argb,
                            int32_t w, int32_t h,
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            uint32_t color)
{
    uint32_t u[16];
    int32_t L = (int32_t)(((uint32_t)w + 15u) >> 4);
    int full = ((w & 15) == 0 &&
                x0 == 0 && y0 == 0 && x1 == w && y1 == h);
    int nq, rows;

    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0)
        return 0;
    /* One dispatch covers the whole surface.  The legal V3D thread-end
     * sequence retires every QPU cleanly, so no 16 KiB batching or dummy
     * TMU transaction is needed. */
    nq = g2d_fit_nq(v3d_g2d_num_qpus(), h);
    rows = h / nq;
    u[0] = color;
    u[1] = (uint32_t)(L - 1);
    u[2] = (uint32_t)(w * 4u);
    u[3] = (uint32_t)(h - 1);
    u[4] = (uint32_t)(L * 64u - (uint32_t)w * 4u);
    u[5] = (uint32_t)(L * rows);
    u[6] = phys;
    u[7] = (uint32_t)x0;
    u[8] = (uint32_t)x1;
    u[9] = (uint32_t)y0;
    u[10] = (uint32_t)y1;
    u[11] = (uint32_t)full;
    u[12] = (uint32_t)rows;
    u[13] = (uint32_t)(rows * (int32_t)w * 4);
    u[14] = v3d_g2d_scratch_phys();
    u[15] = 0u;
    return v3d_g2d_run(g2d_qpu_argb_fill, g2d_qpu_argb_fill_n, u, 16,
                       nq, NULL, 0, argb,
                       (size_t)w * (size_t)h * 4u) == 0;
}

/* affine blit of a clipped dst rect (argb_blit / argb_rotate kernel) */
static int gpu_affine_surface(const uint64_t *kcode, int knwords,
                              const g2d_cpu_map_t *m,
                              uint32_t src_phys, uint32_t *argb_src,
                              int32_t src_w, int32_t src_h,
                              uint32_t dst_phys, uint32_t *argb_dst,
                              int32_t dst_w, int32_t dst_h,
                              int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t u[24];
    int32_t L = (int32_t)(((uint32_t)dst_w + 15u) >> 4);
    int nq = v3d_g2d_num_qpus();
    int32_t rows;
    int full = ((dst_w & 15) == 0 &&
                x0 == 0 && y0 == 0 && x1 == dst_w && y1 == dst_h);

    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0)
        return 0;
    if (!gpu_map_fits(m, (int64_t)L * 16, dst_h))
        return 0;
    /* nq must divide dst_h: the slice guard only tests the band start,
     * so a non-divisible rows count writes past the last row. */
    nq = g2d_fit_nq(nq, dst_h);
    rows = dst_h / nq;
    u[0] = (uint32_t)m->pu;
    u[1] = (uint32_t)m->qu;
    u[2] = (uint32_t)m->pv;
    u[3] = (uint32_t)m->qv;
    u[4] = (uint32_t)m->cu;
    u[5] = (uint32_t)m->cv;
    u[6] = (uint32_t)(src_w - 1);
    u[7] = (uint32_t)(src_h - 1);
    u[8] = (uint32_t)src_w;
    u[9] = dst_phys;        /* PHYSICAL addresses */
    u[10] = src_phys;
    u[11] = (uint32_t)(L - 1);
    u[12] = (uint32_t)dst_w * 4u;
    u[13] = (uint32_t)(dst_h - 1);
    u[14] = (uint32_t)(L * 64 - (uint32_t)dst_w * 4u);
    u[15] = (uint32_t)(L * rows);
    if (full) {
        /* whole-surface fast path: u16..u19 carry the incremental-map
         * constants (see the kernel header) */
        u[16] = (uint32_t)((int64_t)m->pu * dst_w - m->qu);  /* uxwrap */
        u[17] = (uint32_t)((int64_t)m->pv * dst_w - m->qv);  /* vxwrap */
        u[18] = (uint32_t)(m->pu << 4);                      /* pu16 */
        u[19] = (uint32_t)(m->pv << 4);                      /* pv16 */
    } else {
        u[16] = (uint32_t)x0;
        u[17] = (uint32_t)x1;
        u[18] = (uint32_t)y0;
        u[19] = (uint32_t)y1;
    }
    u[20] = (uint32_t)full;
    u[21] = (uint32_t)rows;
    u[22] = (uint32_t)(rows * (int32_t)dst_w * 4);  /* rows_stride */
    u[23] = v3d_g2d_scratch_phys();                 /* out-of-rect write target */
    return v3d_g2d_run(kcode, knwords, u, 24,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0;
}

/* clamped-edge blit (argb_blit kernel) */
static int gpu_blit_surface(const g2d_cpu_map_t *m,
                            uint32_t src_phys, uint32_t *argb_src,
                            int32_t src_w, int32_t src_h,
                            uint32_t dst_phys, uint32_t *argb_dst,
                            int32_t dst_w, int32_t dst_h,
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return gpu_affine_surface(g2d_qpu_argb_blit, g2d_qpu_argb_blit_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              x0, y0, x1, y1);
}

/* whole-surface rotation (argb_rotate kernel): every destination pixel is
 * written - rotated content or transparent 0 - so dst may be any size
 * holding the rotated content box */
static int gpu_rotate_surface(const g2d_cpu_map_t *m,
                              uint32_t src_phys, uint32_t *argb_src,
                              int32_t src_w, int32_t src_h,
                              uint32_t dst_phys, uint32_t *argb_dst,
                              int32_t dst_w, int32_t dst_h)
{
    return gpu_affine_surface(g2d_qpu_argb_rotate, g2d_qpu_argb_rotate_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              0, 0, dst_w, dst_h);
}

/* alpha blend of a clipped dst rect (argb_alpha kernel): the blend is
 * done entirely on the GPU - two TMU read streams (mapped src sample and
 * the real dst pixel), per-channel (s*sa + d*(255-sa)) >> 8 with the
 * over-alpha, written back through the TMU.  The ROT_0 map has qu = pv =
 * 0, so the kernel only loads (pu,cu,qv,cv). */
static int gpu_alpha_surface(const g2d_cpu_map_t *m, uint8_t alpha,
                             uint32_t src_phys, uint32_t *argb_src,
                             int32_t src_w, int32_t src_h,
                             uint32_t dst_phys, uint32_t *argb_dst,
                             int32_t dst_w, int32_t dst_h,
                             int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t u[23];
    int32_t L = (int32_t)(((uint32_t)dst_w + 15u) >> 4);
    int nq = v3d_g2d_num_qpus();
    int32_t rows;

    /* The kernel's dst-read stream is rect-gated (out-of-rect lanes read
     * their lane-private scratch word), so any dst width works - no
     * 16-pixel alignment requirement. */
    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || alpha == 0)
        return 0;
    if (!gpu_map_fits(m, dst_w, dst_h))
        return 0;
    /* nq must divide dst_h (slice guard tests only the band start) */
    nq = g2d_fit_nq(nq, dst_h);
    rows = dst_h / nq;
    u[0] = (uint32_t)m->pu;
    u[1] = (uint32_t)m->cu;
    u[2] = (uint32_t)m->qv;
    u[3] = (uint32_t)m->cv;
    u[4] = (uint32_t)(src_w - 1);
    u[5] = (uint32_t)(src_h - 1);
    u[6] = (uint32_t)src_w;
    u[7] = dst_phys;        /* PHYSICAL addresses */
    u[8] = src_phys;
    u[9] = (uint32_t)(L - 1);
    u[10] = (uint32_t)(dst_h - 1);
    u[11] = alpha;
    u[12] = (uint32_t)(L * (int)dst_h);   /* full-surface n; kernel clips per QPU */
    u[13] = (uint32_t)x0;
    u[14] = (uint32_t)x1;
    u[15] = (uint32_t)y0;
    u[16] = (uint32_t)y1;
    u[17] = 16u;
    u[18] = (uint32_t)(L * 64 - dst_w * 4);   /* jump: L*64 - dst stride */
    u[19] = 255u;
    u[20] = (uint32_t)rows;
    u[21] = (uint32_t)(rows * (int32_t)dst_w * 4); /* rows * stride (bytes) */
    u[22] = v3d_g2d_scratch_phys();                 /* out-of-rect write target */
    return v3d_g2d_run(g2d_qpu_argb_alpha, g2d_qpu_argb_alpha_n, u, 23,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0;
}

/* ------------------------------------------------------------------ */
/* arch_g2d API (called by the thin bsp_g2d dispatch layer)            */
/* ------------------------------------------------------------------ */

int32_t arch_g2d_init(void)
{
    return v3d_g2d_init();
}

int32_t arch_g2d_fill(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
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
    if (phys) {
        /* Never replay a submitted operation on the CPU: a timed-out
         * dispatch may still own or have partially written dst. */
        return gpu_fill_surface(phys, argb, argb_w, argb_h, rx, ry,
                                rx + rw, ry + rh, color) ? 0 : -1;
    }
    log_cpu_fallback("fill");
    g2d_cpu_fill(argb, argb_w, argb_h, x, y, w, h, color);
    return 0;
}

int32_t arch_g2d_blt(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                  int32_t src_w, int32_t src_h,
                  int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                  uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                  int32_t dst_w, int32_t dst_h,
                  int32_t dx, int32_t dy, int32_t dw, int32_t dh)
{
    g2d_cpu_map_t m;
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
        g2d_cpu_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            return gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                    dst_phys, argb_dst, dst_w, dst_h, rx, ry,
                                    rx + rw, ry + rh) ? 0 : -1;
        }
    }
    log_cpu_fallback("blt");
    g2d_cpu_blt(argb_src, src_w, src_h, sx, sy, sw, sh,
                argb_dst, dst_w, dst_h, dx, dy, dw, dh);
    return 0;
}

int32_t arch_g2d_blt_alpha(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                        int32_t src_w, int32_t src_h,
                        int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                        uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                        int32_t dst_w, int32_t dst_h,
                        int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                        uint8_t alpha)
{
    g2d_cpu_map_t m;
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
            g2d_cpu_map_params(sx, sy, sw, sh, dx, dy, dw, dh,
                               G2D_MAP_ROT_0, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                return gpu_alpha_surface(&m, alpha, src_phys, argb_src,
                                         src_w, src_h, dst_phys, argb_dst,
                                         dst_w, dst_h, rx, ry,
                                         rx + rw, ry + rh) ? 0 : -1;
            }
        }
    }
    log_cpu_fallback("blt_alpha");
    g2d_cpu_blt_alpha(argb_src, src_w, src_h, sx, sy, sw, sh,
                      argb_dst, dst_w, dst_h, dx, dy, dw, dh, alpha);
    return 0;
}

/* Scalar source-over blend, identical math to g2d_cpu_blt_alpha:
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
 * same blend math as arch_g2d_blt_alpha.  alpha == 0 is a no-op. */
int32_t arch_g2d_fill_alpha(uint32_t *argb, int32_t argb_w, int32_t argb_h,
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
 * math as arch_g2d_blt_alpha (effective alpha (src_a * alpha) >> 8,
 * then the /255 blend). */
int32_t arch_g2d_blt_cpu(uint32_t *argb_src, int32_t src_w, int32_t src_h,
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

int32_t arch_g2d_scale_to(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                       int32_t src_w, int32_t src_h,
                       uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                       int32_t dst_w, int32_t dst_h)
{
    g2d_cpu_map_t m;
    uint32_t src_phys = 0, dst_phys = 0;

    if (argb_src && argb_dst && argb_src != argb_dst &&
        src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        g2d_cpu_map_params(0, 0, src_w, src_h, 0, 0, dst_w, dst_h,
                           G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            return gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                    dst_phys, argb_dst, dst_w, dst_h,
                                    0, 0, dst_w, dst_h) ? 0 : -1;
        }
    }
    log_cpu_fallback("scale_to");
    g2d_cpu_scale_to(argb_src, src_w, src_h, argb_dst, dst_w, dst_h);
    return 0;
}

int32_t arch_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                           int32_t *dst_w, int32_t *dst_h)
{
    g2d_cpu_rotated_size(src_w, src_h, degree, dst_w, dst_h);
    return 0;
}

int32_t arch_g2d_rotate(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                     int32_t src_w, int32_t src_h,
                     uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                     int32_t dst_w, int32_t dst_h, int32_t degree)
{
    g2d_cpu_map_t m;
    int32_t rot = ((degree % 360) + 360) % 360;
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
     * for 180).  The CPU rasterizer handles in-place via a direct
     * reversal. */
    if (argb_src && argb_dst && argb_src != argb_dst &&
        src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        g2d_cpu_rotated_size(src_w, src_h, rot, &bw, &bh);
        if (bw > 0 && bh > 0) {
            g2d_cpu_map_rotate(src_w, src_h, rot, bw, bh, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                return gpu_rotate_surface(&m, src_phys, argb_src, src_w, src_h,
                                          dst_phys, argb_dst, dst_w, dst_h) ? 0 : -1;
            }
        }
    }
    log_cpu_fallback("rotate");
    g2d_cpu_rotate(argb_src, src_w, src_h, argb_dst, dst_w, dst_h, degree);
    return 0;
}
