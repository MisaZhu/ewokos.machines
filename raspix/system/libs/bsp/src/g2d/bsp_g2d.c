/*
 * bsp_g2d.c - raspix (Raspberry Pi 3 / BCM2837, Pi 4 / BCM2711) g2d
 * back end: V3D-accelerated offline ARGB8888 drawing, ported from the
 * raspi5 bsp_g2d.c (proven on real Pi 5 hardware).
 *
 * The V3D CSD kernels (see v3d_g2d.c) scan the destination in 16-pixel
 * groups; the kernels carry the destination rectangle in uniforms and
 * gate their TMU writes with a per-lane in-rect test.  A final partial
 * 16-pixel group is safe because lanes beyond the surface width are
 * redirected to the TMU scratch block:
 *
 *   bsp_g2d_fill       any clipped rect        -> argb_fill kernel
 *   bsp_g2d_blt        any clipped dst rect    -> argb_blit kernel
 *   bsp_g2d_scale_to   whole dst               -> argb_blit kernel
 *   bsp_g2d_rotate     any angle, any dst size -> argb_rotate kernel
 *                       (dst < box: clipped to dst top-left)
 *
 * bsp_g2d_blt_alpha uses a dedicated source-over kernel.  Its two-read
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
 * VC4 path (Pi3, V3D 2.1): the *_vc4 SRQ kernels cover the same
 * operation set with one thread per QPU (v3d_g2d_run_vc4).  The host
 * splits every surface into nq row bands (rows_q = h / nq, so nq must
 * divide h) and hands each QPU its own 32-word uniform slot with the
 * band's destination row base and the affine product bases (see the
 * g2d_qpu_kernels.h VC4 contract).
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
 *
 * NO CPU FALLBACK: an operation that cannot run on the GPU (not
 * eligible, or the dispatch failed) returns -1.  A failed dispatch is
 * never replayed on the CPU: a timed-out dispatch may still own or
 * have partially written the destination.
 */

#include <bsp/bsp_g2d.h>

#include <string.h>

#include "v3d_g2d.h"

#define G2D_MAX_COEF (1 << 23)  /* |map coefficient| must fit smul24 */

/* VC bus address alias for the VC4 (V3D 2.1) kernels: every memory
 * address a kernel touches (canvas band bases, source, scratch sink)
 * must carry this DIRECT/coherent VC bus alias - the same alias the
 * SRQ launcher applies to the code/uniform fetches, and the exact
 * addressing GPU_FFT release 3.0 uses on Pi2/3 (mem_alloc flag 0x4 =
 * MEM_FLAG_COHERENT, "ARM cannot see VC4 L2 on Pi 2").  The cached
 * 0x40000000 alias served stale V3D-L2T lines for freshly written
 * uniforms on real Pi3 hardware: the QPU read garbage uniform values
 * and every uniform-dependent dispatch spun forever.  The V3D >= 4.x
 * CSD paths take plain physical addresses and must NOT be aliased. */
#define VC4_BUS_ALIAS 0xC0000000u

/* ------------------------------------------------------------------ */
/* affine map coefficients (shared with the GPU kernels)               */
/* ------------------------------------------------------------------ */

/* Affine map coefficients (Q15 fixed point), identical to the GPU
 * kernels' uniform layout: u = (pu*X + qu*Y)>>15 + cu, v = (pv*X + qv*Y)
 * >>15 + cv for a destination pixel (X, Y). */
typedef struct {
    int32_t pu, qu, pv, qv;     /* Q15 scale factors folded with rotation */
    int32_t cu, cv;             /* constant offsets (source origin / flip) */
} g2d_map_t;

/* Rotation codes for g2d_map_params (90 = clockwise). */
enum {
    G2D_MAP_ROT_0 = 0,
    G2D_MAP_ROT_90 = 1,
    G2D_MAP_ROT_180 = 2,
    G2D_MAP_ROT_270 = 3
};

/* Build the map that samples src crop (sx,sy,sw,sh) into dst rect
 * (dx,dy,dw,dh), optionally rotated clockwise.  dx/dy only select the
 * rect; the map is origin-independent. */
static void g2d_map_params(int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    int rotate, g2d_map_t *m)
{
    int32_t ku, kv;
    int32_t kudx, kvdy;

    (void)dx;
    (void)dy;
    if (dw <= 0)
        dw = 1;
    if (dh <= 0)
        dh = 1;
    if (sw <= 0)
        sw = 1;
    if (sh <= 0)
        sh = 1;
    /* Q15 scale factors: source pixel per destination pixel.  Computed in
     * 64 bits so wide crops cannot overflow the coefficient. */
    ku = (int32_t)(((int64_t)sw << 15) / dw);
    kv = (int32_t)(((int64_t)sh << 15) / dh);
    /* The map is evaluated at absolute destination pixels (X, Y), so the
     * constants carry the -(dx,dy) offset, shifted by 15 like the
     * multiply: at (X,Y)=(dx,dy) the sample is exactly (sx,sy). */
    kudx = (int32_t)(((int64_t)ku * dx) >> 15);
    kvdy = (int32_t)(((int64_t)kv * dy) >> 15);
    switch (rotate) {
    case G2D_MAP_ROT_90:                       /* clockwise */
        /* dst(x,y) = src(sy + sh-1 - (x-dx)*sh/dw,  sx + (y-dy)*sw/dh):
         * the dst Y axis maps to the src X axis (scale sw/dh) and the dst
         * X axis maps to the src Y axis reversed (scale sh/dw).  (The old
         * code swapped the scales - ku for the v-axis and kv for the
         * u-axis - which only worked for square sources.) */
        m->pu = 0;   m->qu = (int32_t)(((int64_t)sw << 15) / dh);
        m->cu = sx - (int32_t)(((int64_t)m->qu * dy) >> 15);
        m->pv = -(int32_t)(((int64_t)sh << 15) / dw);
        m->qv = 0;   m->cv = sy + sh - 1 +
                     (int32_t)(((int64_t)m->pv * dx) >> 15);
        break;
    case G2D_MAP_ROT_180:
        m->pu = -ku; m->qu = 0;   m->cu = sx + sw - 1 + kudx;
        m->pv = 0;   m->qv = -kv; m->cv = sy + sh - 1 + kvdy;
        break;
    case G2D_MAP_ROT_270:                      /* counter-clockwise */
        /* dst(x,y) = src(sy + (x-dx)*sh/dw,  sx + sw-1 - (y-dy)*sw/dh):
         * the dst X axis maps to the src Y axis (scale sh/dw) and the
         * dst Y axis maps to the src X axis reversed (scale sw/dh). */
        m->pu = 0;   m->qu = -(int32_t)(((int64_t)sw << 15) / dh);
        m->cu = sx + sw - 1 +
                (int32_t)(((int64_t)m->qu * dy) >> 15);
        m->pv = (int32_t)(((int64_t)sh << 15) / dw);
        m->qv = 0;   m->cv = sy - (int32_t)(((int64_t)m->pv * dx) >> 15);
        break;
    default:                                   /* G2D_MAP_ROT_0 */
        m->pu = ku;  m->qu = 0;   m->cu = sx - kudx;
        m->pv = 0;   m->qv = kv;  m->cv = sy - kvdy;
        break;
    }
}

/* 14-bit fixed point trig (table[i] = round(sin(i deg) * 16384)),
 * identical to the EwokOS arch_g2d table. */
#define G2D_FP_BITS 14
#define G2D_FP_ONE  (1 << G2D_FP_BITS)

static const int16_t g2d_sin_table[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997,
    2280, 2563, 2845, 3126, 3406, 3686, 3964, 4240,
    4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402,
    6664, 6924, 7182, 7438, 7692, 7943, 8192, 8438,
    8682, 8923, 9162, 9397, 9630, 9860, 10087, 10311,
    10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982,
    12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421,
    13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598,
    14726, 14849, 14968, 15082, 15191, 15296, 15396, 15491,
    15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083,
    16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362,
    16374, 16382, 16384
};

static int32_t g2d_norm_degree(int32_t degree)
{
    return ((degree % 360) + 360) % 360;
}

static int32_t g2d_sin_fp(int32_t degree)
{
    degree = g2d_norm_degree(degree);
    if (degree <= 90)
        return g2d_sin_table[degree];
    if (degree <= 180)
        return g2d_sin_table[180 - degree];
    if (degree <= 270)
        return -g2d_sin_table[degree - 180];
    return -g2d_sin_table[360 - degree];
}

static int32_t g2d_cos_fp(int32_t degree)
{
    return g2d_sin_fp(degree + 90);
}

/* Smallest size able to hold src_w x src_h rotated clockwise by degree
 * (any angle): exact swap/keep for multiples of 90, rotated bounding box
 * otherwise (ceiling-rounded, 14-bit fixed-point trig). */
static void g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                          int32_t *dst_w, int32_t *dst_h)
{
    int32_t c, s, w, h;

    if (dst_w == NULL || dst_h == NULL)
        return;
    *dst_w = 0;
    *dst_h = 0;
    if (src_w <= 0 || src_h <= 0)
        return;

    degree = g2d_norm_degree(degree);
    if (degree % 90 == 0) {
        if (degree == 90 || degree == 270) {
            *dst_w = src_h;
            *dst_h = src_w;
        } else {
            *dst_w = src_w;
            *dst_h = src_h;
        }
        return;
    }

    c = g2d_cos_fp(degree);
    s = g2d_sin_fp(degree);
    if (c < 0)
        c = -c;
    if (s < 0)
        s = -s;
    /* round up so the rotated corners always fit */
    w = (int32_t)(((int64_t)src_w * c + (int64_t)src_h * s + G2D_FP_ONE - 1)
                  >> G2D_FP_BITS);
    h = (int32_t)(((int64_t)src_w * s + (int64_t)src_h * c + G2D_FP_ONE - 1)
                  >> G2D_FP_BITS);
    *dst_w = (w < 1) ? 1 : w;
    *dst_h = (h < 1) ? 1 : h;
}

/* Whole-surface clockwise rotation map (canonical rotate semantics shared
 * with the argb_rotate GPU kernel).  Rotation around the surface centres,
 * written into the top-left bw x bh box of the destination:
 *
 *   u = floor((x*c14 + y*s14)/2^14) + cu,   c14/s14 = 14-bit cos/sin
 *   v = floor((-x*s14 + y*c14)/2^14) + cv
 *
 * where cu/cv place the rotated src centre on the content-box centre:
 *
 *   cu = round((sw-1)/2 - (dcx*c14 + dcy*s14)/2^28)
 *   cv = round((sh-1)/2 - (dcy*c14 - dcx*s14)/2^28)
 *
 * with dcx/dcy the 14-bit half-pixel centres of the content box.  Q15
 * coefficients: pu = 2*c14 etc., so the kernel's (pu*x + qu*y)>>15
 * reproduces the fixed-point formula exactly (arithmetic shift, no
 * truncation drift), and for 0/90/180/270 the map degenerates exactly to
 * the swap/keep right-angle coefficient sets. */
static void g2d_map_rotate(int32_t src_w, int32_t src_h, int32_t degree,
                        int32_t bw, int32_t bh, g2d_map_t *m)
{
    int32_t c = g2d_cos_fp(degree);
    int32_t s = g2d_sin_fp(degree);
    int64_t dcx = ((int64_t)bw - 1) << (G2D_FP_BITS - 1);
    int64_t dcy = ((int64_t)bh - 1) << (G2D_FP_BITS - 1);

    m->pu = c << 1;
    m->qu = s << 1;
    m->pv = -(s << 1);
    m->qv = c << 1;
    /* (sw-1)/2 in 2^28 units is (sw-1) << 27 */
    m->cu = (int32_t)((((int64_t)(src_w - 1) << 27) - (dcx * c + dcy * s)
                       + (1 << 27)) >> 28);
    m->cv = (int32_t)((((int64_t)(src_h - 1) << 27) - (dcy * c - dcx * s)
                       + (1 << 27)) >> 28);
}

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

/* Validate a caller-provided physical base for the QPU's 32-bit TMU
 * addresses.  Returns the physical address usable by the kernels, or 0
 * when the canvas cannot run on the GPU (not physically contiguous, no
 * phy supplied, > 4 GB physical, or the address fails the RAM-range
 * validation gate). */
static uint32_t gpu_phys(ewokos_addr_t phys, size_t bytes, uint8_t contig)
{
    if (!contig)
        return 0;
    if (phys == 0)
        return 0;
    /* ewokos_addr_t is 32 bits on the arm build: always sub-4 GB */
    if (sizeof(phys) > 4 && (phys >> 32) != 0)
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
static int gpu_map_fits(const g2d_map_t *m, int64_t w, int64_t h)
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

/* VC4 (V3D 2.1) fill: one SRQ thread per QPU, each processing its
 * rows_q-row band.  Per-QPU contract: u0=color u1=qid u2=L-1
 * u3=dst_row0 (band row 0, x=0) u4,5=x0,x1 u6=rowjump u7=rows_q
 * u8=gx0. */
static int gpu_fill_surface_vc4(uint32_t phys, uint32_t *argb,
                                int32_t w, int32_t h,
                                int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1,
                                uint32_t color)
{
    uint32_t u[16 * VC4_UNIF_QWORDS];
    int32_t gx0 = x0 & ~15;
    int32_t L = (int32_t)(((uint32_t)(x1 - gx0) + 15u) >> 4);
    int nq, rows, q;

    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0)
        return 0;
    nq = g2d_fit_nq(v3d_g2d_num_qpus(), h);
    rows = h / nq;
    for (q = 0; q < nq; q++) {
        uint32_t *s = u + q * VC4_UNIF_QWORDS;
        int32_t y_start = q * rows;

        s[0] = color;
        s[1] = (uint32_t)q;
        s[2] = (uint32_t)(L - 1);
        s[3] = (phys + (uint32_t)y_start * (uint32_t)w * 4u)
               | VC4_BUS_ALIAS;
        s[4] = (uint32_t)x0;
        s[5] = (uint32_t)x1;
        s[6] = (uint32_t)L * 64u - (uint32_t)w * 4u;
        s[7] = (uint32_t)rows;
        s[8] = (uint32_t)gx0;
    }
    return v3d_g2d_run_vc4(g2d_qpu_argb_fill_vc4, g2d_qpu_argb_fill_vc4_n,
                           u, nq, NULL, 0, argb,
                           (size_t)w * (size_t)h * 4u) == 0;
}

/* affine blit of a clipped dst rect (argb_blit / argb_rotate kernel) */
static int gpu_affine_surface(const uint64_t *kcode, int knwords,
                              const g2d_map_t *m,
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

/* VC4 (V3D 2.1) affine blit/rotate: per-QPU 32-word slots carrying the
 * band's dst row base and the affine product bases.  VC4's mul24 is
 * unsigned, so the coefficients arrive as |pu|/|pv| plus a packed sign
 * word; the UN-shifted product base at the band's first pixel is
 * u_base = pu*gx0 + qu*y_start, v_base = pv*gx0 + qv*y_start. */
static int gpu_affine_surface_vc4(const uint64_t *kcode, int knwords,
                                  const g2d_map_t *m,
                                  uint32_t src_phys, uint32_t *argb_src,
                                  int32_t src_w, int32_t src_h,
                                  uint32_t dst_phys, uint32_t *argb_dst,
                                  int32_t dst_w, int32_t dst_h,
                                  int32_t x0, int32_t y0,
                                  int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS];
    int32_t gx0 = x0 & ~15;
    int32_t L = (int32_t)(((uint32_t)(x1 - gx0) + 15u) >> 4);
    int nq, rows, q;

    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0)
        return 0;
    if (!gpu_map_fits(m, (int64_t)L * 16, dst_h))
        return 0;
    nq = g2d_fit_nq(v3d_g2d_num_qpus(), dst_h);
    rows = dst_h / nq;
    for (q = 0; q < nq; q++) {
        uint32_t *s = u + q * VC4_UNIF_QWORDS;
        int32_t y_start = q * rows;
        int64_t u_base = (int64_t)m->pu * gx0 + (int64_t)m->qu * y_start;
        int64_t v_base = (int64_t)m->pv * gx0 + (int64_t)m->qv * y_start;

        s[0] = (uint32_t)m->cu;
        s[1] = (uint32_t)m->cv;
        s[2] = (uint32_t)(src_w - 1);
        s[3] = (uint32_t)(src_h - 1);
        s[4] = (uint32_t)src_w;
        s[5] = src_phys | VC4_BUS_ALIAS;
        s[6] = (dst_phys + (uint32_t)y_start * (uint32_t)dst_w * 4u)
               | VC4_BUS_ALIAS;
        s[7] = (uint32_t)(L - 1);
        s[8] = (uint32_t)rows;
        s[9] = (uint32_t)x0;
        s[10] = (uint32_t)x1;
        s[11] = (uint32_t)gx0;
        s[12] = (uint32_t)q;
        s[13] = (uint32_t)L * 64u - (uint32_t)dst_w * 4u;
        s[14] = (uint32_t)u_base;       /* UN-shifted product base */
        s[15] = (uint32_t)v_base;
        s[16] = (uint32_t)(m->pu < 0 ? -m->pu : m->pu);
        s[17] = (uint32_t)(m->pv < 0 ? -m->pv : m->pv);
        s[18] = (uint32_t)((m->pu < 0 ? 1u : 0u) |
                           (m->pv < 0 ? 2u : 0u));
        s[19] = (uint32_t)((int64_t)m->pu << 4);    /* pu16 */
        s[20] = (uint32_t)((int64_t)m->pv << 4);    /* pv16 */
        s[21] = (uint32_t)((int64_t)m->pu * 16 * L - m->qu); /* uxwrap */
        s[22] = (uint32_t)((int64_t)m->pv * 16 * L - m->qv); /* vxwrap */
        s[23] = 0u;                                 /* trailing, unused */
    }
    return v3d_g2d_run_vc4(kcode, knwords, u, nq,
                           argb_src, (size_t)src_w * src_h * 4,
                           argb_dst, (size_t)dst_w * dst_h * 4) == 0;
}

/* clamped-edge blit (argb_blit kernel) */
static int gpu_blit_surface(const g2d_map_t *m,
                            uint32_t src_phys, uint32_t *argb_src,
                            int32_t src_w, int32_t src_h,
                            uint32_t dst_phys, uint32_t *argb_dst,
                            int32_t dst_w, int32_t dst_h,
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (v3d_g2d_ver() == 21)
        return gpu_affine_surface_vc4(g2d_qpu_argb_blit_vc4,
                                      g2d_qpu_argb_blit_vc4_n, m,
                                      src_phys, argb_src, src_w, src_h,
                                      dst_phys, argb_dst, dst_w, dst_h,
                                      x0, y0, x1, y1);
    return gpu_affine_surface(g2d_qpu_argb_blit, g2d_qpu_argb_blit_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              x0, y0, x1, y1);
}

/* whole-surface rotation (argb_rotate kernel): every destination pixel is
 * written - rotated content or transparent 0 - so dst may be any size
 * holding the rotated content box */
static int gpu_rotate_surface(const g2d_map_t *m,
                              uint32_t src_phys, uint32_t *argb_src,
                              int32_t src_w, int32_t src_h,
                              uint32_t dst_phys, uint32_t *argb_dst,
                              int32_t dst_w, int32_t dst_h)
{
    if (v3d_g2d_ver() == 21)
        return gpu_affine_surface_vc4(g2d_qpu_argb_rotate_vc4,
                                      g2d_qpu_argb_rotate_vc4_n, m,
                                      src_phys, argb_src, src_w, src_h,
                                      dst_phys, argb_dst, dst_w, dst_h,
                                      0, 0, dst_w, dst_h);
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
static int gpu_alpha_surface_vc4(const g2d_map_t *m, uint8_t alpha,
                                 uint32_t src_phys, uint32_t *argb_src,
                                 int32_t src_w, int32_t src_h,
                                 uint32_t dst_phys, uint32_t *argb_dst,
                                 int32_t dst_w, int32_t dst_h,
                                 int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1);

static int gpu_alpha_surface(const g2d_map_t *m, uint8_t alpha,
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
    if (v3d_g2d_ver() == 21)
        return gpu_alpha_surface_vc4(m, alpha, src_phys, argb_src,
                                     src_w, src_h, dst_phys, argb_dst,
                                     dst_w, dst_h, x0, y0, x1, y1);
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

/* VC4 (V3D 2.1) alpha blend: the alpha op is always a ROT_0 map
 * (qu = pv = 0), so u depends only on x and v only on y: the per-QPU
 * v broadcast base is v_base = qv*y_start and the row-end v advance
 * arrives as vxwrap = -qv (the kernel does v_prod -= vxwrap).
 * pu/qv are non-negative here, so no sign machinery is needed. */
static int gpu_alpha_surface_vc4(const g2d_map_t *m, uint8_t alpha,
                                 uint32_t src_phys, uint32_t *argb_src,
                                 int32_t src_w, int32_t src_h,
                                 uint32_t dst_phys, uint32_t *argb_dst,
                                 int32_t dst_w, int32_t dst_h,
                                 int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS];
    int32_t gx0 = x0 & ~15;
    int32_t L = (int32_t)(((uint32_t)(x1 - gx0) + 15u) >> 4);
    int nq, rows, q;

    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || alpha == 0)
        return 0;
    if (m->pu < 0 || m->qv < 0 || m->qu != 0 || m->pv != 0)
        return 0;                   /* ROT_0 maps only */
    if (!gpu_map_fits(m, dst_w, dst_h))
        return 0;
    nq = g2d_fit_nq(v3d_g2d_num_qpus(), dst_h);
    rows = dst_h / nq;
    for (q = 0; q < nq; q++) {
        uint32_t *s = u + q * VC4_UNIF_QWORDS;
        int32_t y_start = q * rows;

        s[0] = (uint32_t)m->cu;
        s[1] = (uint32_t)m->cv;
        s[2] = (uint32_t)(src_w - 1);
        s[3] = (uint32_t)(src_h - 1);
        s[4] = (uint32_t)src_w;
        s[5] = src_phys | VC4_BUS_ALIAS;
        s[6] = v3d_g2d_scratch_phys() | VC4_BUS_ALIAS;
        s[7] = (dst_phys + (uint32_t)y_start * (uint32_t)dst_w * 4u)
               | VC4_BUS_ALIAS;
        s[8] = (uint32_t)(L - 1);
        s[9] = (uint32_t)rows;
        s[10] = (uint32_t)x0;
        s[11] = (uint32_t)x1;
        s[12] = (uint32_t)gx0;
        s[13] = (uint32_t)q;
        s[14] = alpha;
        s[15] = (uint32_t)m->pu;
        s[16] = (uint32_t)m->qv;
        s[17] = (uint32_t)((int64_t)m->pu * gx0);         /* u_base */
        s[18] = (uint32_t)((int64_t)m->qv * y_start);     /* v_base */
        s[19] = (uint32_t)((int64_t)m->pu << 4);          /* pu16 */
        s[20] = (uint32_t)((int64_t)m->pu * 16 * L);      /* uxwrap */
        s[21] = (uint32_t)(-(int64_t)m->qv);              /* vxwrap */
        s[22] = (uint32_t)L * 64u - (uint32_t)dst_w * 4u; /* rowjump */
    }
    return v3d_g2d_run_vc4(g2d_qpu_argb_alpha_vc4, g2d_qpu_argb_alpha_vc4_n,
                           u, nq, argb_src, (size_t)src_w * src_h * 4,
                           argb_dst, (size_t)dst_w * dst_h * 4) == 0;
}

/* ------------------------------------------------------------------ */
/* bsp_g2d API                                                         */
/* ------------------------------------------------------------------ */

int32_t bsp_g2d_init(void)
{
    return v3d_g2d_init();
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
    if (v3d_g2d_ver() == 21)
        return gpu_fill_surface_vc4(phys, argb, argb_w, argb_h, rx, ry,
                                    rx + rw, ry + rh, color) ? 0 : -1;
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
        g2d_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            return gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                    dst_phys, argb_dst, dst_w, dst_h, rx, ry,
                                    rx + rw, ry + rh) ? 0 : -1;
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
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                return gpu_alpha_surface(&m, alpha, src_phys, argb_src,
                                         src_w, src_h, dst_phys, argb_dst,
                                         dst_w, dst_h, rx, ry,
                                         rx + rw, ry + rh) ? 0 : -1;
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
        g2d_map_params(0, 0, src_w, src_h, 0, 0, dst_w, dst_h,
                           G2D_MAP_ROT_0, &m);
        if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
            return gpu_blit_surface(&m, src_phys, argb_src, src_w, src_h,
                                    dst_phys, argb_dst, dst_w, dst_h,
                                    0, 0, dst_w, dst_h) ? 0 : -1;
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

    /* GPU: any angle, any destination size (the argb_rotate kernel writes
     * every destination pixel - content or transparent 0 - so a dst
     * larger than the rotated content box needs no pre-clear, and a dst
     * smaller than the box is simply clipped: the map is built against
     * the bw x bh content box and the walk stops at the dst edge, so
     * dst only ever shows the top-left corner of the rotated content).
     * In-place is NOT supported on the GPU: the affine walk covers the
     * destination in ascending row-major order, so the map's reads would
     * hit pixels the walk has already overwritten; with no CPU fallback
     * an in-place call fails with -1. */
    if (argb_src && argb_dst && argb_src != argb_dst &&
        src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
        gpu_ok(dst_w, dst_h)) {
        src_phys = gpu_phys(src_phy, (size_t)src_w * src_h * 4, src_contig);
        dst_phys = gpu_phys(dst_phy, (size_t)dst_w * dst_h * 4, dst_contig);
    }
    if (src_phys && dst_phys) {
        g2d_rotated_size(src_w, src_h, rot, &bw, &bh);
        if (bw > 0 && bh > 0) {
            g2d_map_rotate(src_w, src_h, rot, bw, bh, &m);
            if (gpu_map_fits(&m, ((int64_t)dst_w + 15) / 16 * 16, dst_h)) {
                return gpu_rotate_surface(&m, src_phys, argb_src, src_w, src_h,
                                          dst_phys, argb_dst, dst_w, dst_h) ? 0 : -1;
            }
        }
    }
    return -1;
}
