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
 *   arch_g2d_rotate     0/90/180/270 exact-size -> argb_blit kernel
 *
 * arch_g2d_blt_alpha uses a dedicated source-over kernel.  Its two-read
 * pipeline currently requires a destination width divisible by 16;
 * incompatible alpha operations and arbitrary-angle rotation use the CPU.
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
    //klog("g2d: CPU fallback: %s\r\n", operation);
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

/* affine blit of a clipped dst rect (argb_blit kernel) */
static int gpu_blit_surface(const g2d_cpu_map_t *m,
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
    return v3d_g2d_run(g2d_qpu_argb_blit, g2d_qpu_argb_blit_n, u, 24,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0;
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
    int32_t L = dst_w >> 4;
    int nq = v3d_g2d_num_qpus();
    int32_t rows;

    if ((dst_w & 15) != 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || alpha == 0)
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
    u[18] = 64u;
    u[19] = 255u;
    u[20] = (uint32_t)rows;
    u[21] = (uint32_t)(rows * L * 64);              /* rows_stride */
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

void arch_g2d_fill(uint32_t *argb, ewokos_addr_t argb_phy, uint8_t contig,
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
        (void)gpu_fill_surface(phys, argb, argb_w, argb_h, rx, ry,
                               rx + rw, ry + rh, color);
        return;
    }
    log_cpu_fallback("fill");
    g2d_cpu_fill(argb, argb_w, argb_h, x, y, w, h, color);
}

void arch_g2d_blt(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
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
            (void)gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                   dst_phys, argb_dst, dst_w, dst_h, rx, ry,
                                   rx + rw, ry + rh);
            return;
        }
    }
    log_cpu_fallback("blt");
    g2d_cpu_blt(argb_src, src_w, src_h, sx, sy, sw, sh,
                argb_dst, dst_w, dst_h, dx, dy, dw, dh);
}

void arch_g2d_blt_alpha(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
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
        return;

    if (argb_src && argb_dst && argb_src != argb_dst &&
        (dst_w & 15) == 0 &&
        sw > 0 && sh > 0 && dw > 0 && dh > 0 &&
        gpu_clip_rect(&rx, &ry, &rw, &rh, dst_w, dst_h) &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
        if (src_phys && dst_phys) {
            g2d_cpu_map_params(sx, sy, sw, sh, dx, dy, dw, dh,
                               G2D_MAP_ROT_0, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                (void)gpu_alpha_surface(&m, alpha, src_phys, argb_src,
                                        src_w, src_h, dst_phys, argb_dst,
                                        dst_w, dst_h, rx, ry,
                                        rx + rw, ry + rh);
                return;
            }
        }
    }
    log_cpu_fallback("blt_alpha");
    g2d_cpu_blt_alpha(argb_src, src_w, src_h, sx, sy, sw, sh,
                      argb_dst, dst_w, dst_h, dx, dy, dw, dh, alpha);
}

void arch_g2d_scale_to(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
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
            (void)gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                   dst_phys, argb_dst, dst_w, dst_h,
                                   0, 0, dst_w, dst_h);
            return;
        }
    }
    log_cpu_fallback("scale_to");
    g2d_cpu_scale_to(argb_src, src_w, src_h, argb_dst, dst_w, dst_h);
}

void arch_g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                           int32_t *dst_w, int32_t *dst_h)
{
    g2d_cpu_rotated_size(src_w, src_h, degree, dst_w, dst_h);
}

void arch_g2d_rotate(uint32_t *argb_src, ewokos_addr_t src_phy, uint8_t src_contig,
                     int32_t src_w, int32_t src_h,
                     uint32_t *argb_dst, ewokos_addr_t dst_phy, uint8_t dst_contig,
                     int32_t dst_w, int32_t dst_h, int32_t degree)
{
    g2d_cpu_map_t m;
    int32_t rot = ((degree % 360) + 360) % 360;
    int rot_code;
    uint32_t src_phys = 0, dst_phys = 0;

    /* GPU: 0/90/180/270 with an exactly-sized destination (whole-surface
     * write).  In-place is NOT supported on the GPU: the blit kernel
     * walks the destination in ascending row-major order, so for
     * ROT_180 an in-place copy reads src pixels (W-1-x, H-1-y) that the
     * walk has already overwritten (proven on the Pi 5). */
    if (argb_src && argb_dst && rot % 90 == 0 &&
        argb_src != argb_dst && src_w > 0 && src_h > 0 &&
        gpu_ok(dst_w, dst_h) &&
        ((rot == 0 && dst_w == src_w && dst_h == src_h) ||
         (rot == 90 && dst_w == src_h && dst_h == src_w) ||
         (rot == 270 && dst_w == src_h && dst_h == src_w) ||
         (rot == 180 && dst_w == src_w && dst_h == src_h))) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        rot_code = (rot == 0) ? G2D_MAP_ROT_0 :
                   (rot == 90) ? G2D_MAP_ROT_90 :
                   (rot == 270) ? G2D_MAP_ROT_270 : G2D_MAP_ROT_180;
        g2d_cpu_map_params(0, 0, src_w, src_h, 0, 0, dst_w, dst_h,
                           rot_code, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            (void)gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                   dst_phys, argb_dst, dst_w, dst_h,
                                   0, 0, dst_w, dst_h);
            return;
        }
    }
    log_cpu_fallback("rotate");
    g2d_cpu_rotate(argb_src, src_w, src_h, argb_dst, dst_w, dst_h, degree);
}
