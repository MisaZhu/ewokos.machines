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
 *   bsp_g2d_rotate     any angle, exact bbox   -> argb_rotate kernel
 *
 * bsp_g2d_blt_alpha uses a dedicated source-over pipeline.  On VC4 the
 * mapped source is gathered through TMU, the contiguous destination span
 * is staged through VDR, and one SRQ launch blends up to twelve spans in
 * parallel through the direct bus alias.  Sixteen immutable span variants
 * provide exact 1..16 pixel VDW writes without a CPU fallback.
 *
 * The rotate kernel runs the same affine engine as blit for every angle
 * (the canonical centres-based map degenerates exactly to the right-angle
 * coefficient sets) and writes every destination pixel: rotated content,
 * or transparent 0 for pixels whose pre-clamp source coordinate is out of
 * range (a five-instruction flag chain, then `mov ifna <data>, 0` right
 * after the LDTMU).  The clamp is kept so out-of-range TMU reads stay in
 * the source buffer.
 *
 * VC4 path (Pi3, V3D 2.1): fill, blit, scale and rotate flatten work into
 * exact 1..16-pixel spans and submit fully retired 12-QPU SRQ batches.
 * Alpha batches its two staging steps, then serializes the long blend kernel
 * to avoid the VC4 VPM DMA deadlock.  Public hardware APIs are GPU-only.
 *
 * GPU eligibility (on top of the width/size checks):
 *   - the *_contig flag is set and the matching *_phy carries a valid
 *     physical base (the caller resolves it: contig shm slab / sys_dma
 *     memory); the physical address must fit the kernels' 32-bit TMU
 *     addresses.
 *
 * The GPU operates on caller buffers through caller-supplied physical bases.
 * Alpha's intermediate staging is also GPU-produced; the ARM only builds
 * address vectors and uniforms (see v3d_g2d.c).
 *
 * There is no implicit CPU fallback.  A submitted dispatch is never replayed:
 * a timed-out dispatch may still own or have partially written destination.
 */

#include <bsp/bsp_g2d.h>

#include <string.h>

#include "ewoksys/dma.h"
#include "v3d_g2d.h"

#define G2D_MAX_COEF (1 << 23)  /* |map coefficient| must fit smul24 */

/* VC bus aliases for VC4 (V3D 2.1). Reads, code and uniforms use the
 * L2-cached alias. VDW destinations use the direct alias: a cached GPU
 * write followed by a Normal-NC ARM rewrite can otherwise be resurrected
 * by a later L2 clear and overwrite the newer ARM data. Raw ARM physical
 * addresses wedge the QPU memory pipe on real Pi3 hardware. V3D >= 4.x
 * CSD paths take plain physical addresses and must not be aliased. */
#define VC4_BUS_ALIAS 0x40000000u
#define VC4_BUS_ALIAS_DIRECT 0xc0000000u

/* A long VC4 loop reduces SRQ overhead, but an excessively long VDW loop
 * can delay thread retirement on BCM2837.  Sixteen rows/QPU is the
 * validated default; keep this tunable for real-silicon characterization. */
#ifndef VC4_FILL_ROWS_PER_QPU
#define VC4_FILL_ROWS_PER_QPU 16
#endif
#if VC4_FILL_ROWS_PER_QPU < 1 || VC4_FILL_ROWS_PER_QPU > 64
#error "VC4_FILL_ROWS_PER_QPU must be in the range 1..64"
#endif

static int gpu_vc4_run_once(const uint64_t *code, int nwords,
                            const uint32_t *unifs, int nq,
                            const void *src, size_t src_len,
                            void *dst, size_t dst_len)
{
    return v3d_g2d_run_vc4(code, nwords, unifs, nq,
                           src, src_len, dst, dst_len) == 0 ? 0 : -1;
}

/* GPU helpers use a three-state result: success, unsupported before any
 * submission, or failure after submission.  Both non-success states surface
 * as -1; keeping them distinct prevents a failed dispatch being mistaken for
 * a capability rejection. */
enum {
    GPU_FAILED = -1,
    GPU_UNSUPPORTED = 0,
    GPU_DONE = 1
};

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

static int32_t g2d_scale_coef(int32_t src_len, int32_t dst_len)
{
    int64_t numerator;

    if (src_len <= 1 || dst_len <= 1)
        return 0;
    numerator = (int64_t)(src_len - 1) << 15;
    return (int32_t)((numerator + dst_len - 2) / (dst_len - 1));
}

/* Rotation codes for g2d_map_params (90 = clockwise). */
enum {
    G2D_BSP_MAP_ROT_0 = 0,
    G2D_BSP_MAP_ROT_90 = 1,
    G2D_BSP_MAP_ROT_180 = 2,
    G2D_BSP_MAP_ROT_270 = 3
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
    ku = g2d_scale_coef(sw, dw);
    kv = g2d_scale_coef(sh, dh);
    /* The map is evaluated at absolute destination pixels (X, Y), so the
     * constants carry the -(dx,dy) offset, shifted by 15 like the
     * multiply: at (X,Y)=(dx,dy) the sample is exactly (sx,sy). */
    kudx = (int32_t)(((int64_t)ku * dx) >> 15);
    kvdy = (int32_t)(((int64_t)kv * dy) >> 15);
    switch (rotate) {
    case G2D_BSP_MAP_ROT_90:                   /* clockwise */
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
    case G2D_BSP_MAP_ROT_180:
        m->pu = -ku; m->qu = 0;   m->cu = sx + sw - 1 + kudx;
        m->pv = 0;   m->qv = -kv; m->cv = sy + sh - 1 + kvdy;
        break;
    case G2D_BSP_MAP_ROT_270:                  /* counter-clockwise */
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

    m->pu = c * 2;
    m->qu = s * 2;
    m->pv = -(s * 2);
    m->qv = c * 2;
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
    if (bytes > (size_t)UINT32_MAX - (uint32_t)phys)
        return 0;                       /* 32-bit QPU address range overflow */
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

/* VC4 staging is addressed by 32-bit bus values on the target.  The host
 * unit test still needs real CPU pointers for the staging copies, so keep a
 * private aligned pool there instead of round-tripping a truncated pointer. */
static void *gpu_dma_alloc(size_t bytes, uint32_t *phys_out)
{
    if (phys_out == NULL)
        return NULL;
#ifdef VC4_HOST_TEST
    static uint8_t pool[4u * 1024u * 1024u];
    static size_t next;
    size_t aligned = (bytes + 15u) & ~(size_t)15u;
    void *p;

    if (aligned > sizeof(pool) - next)
        return NULL;
    p = pool + next;
    next += aligned;
    *phys_out = (uint32_t)(uintptr_t)p;
    return p;
#else
    return v3d_g2d_vc4_staging_alloc(bytes, phys_out);
#endif
}

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
                       (size_t)w * (size_t)h * 4u) == 0 ?
           GPU_DONE : GPU_FAILED;
}

/* VC4 exact fill, one SRQ/QPU thread per 1..16-pixel span.  Each thread
 * performs one VDW and the ARM batches spans at the physical QPU count. */
static int gpu_fill_surface_vc4(uint32_t phys, uint32_t *argb,
                                int32_t w, int32_t h,
                                int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1,
                                uint32_t color)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t groups, maxq, rows, total, q;

    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0)
        return 0;
    rows = y1 - y0;
    maxq = v3d_g2d_num_qpus();
    if (x0 < 0 || x1 <= x0 || x1 > w || rows <= 0 || maxq <= 0)
        return 0;
    groups = (x1 - x0 + 15) >> 4;
    if (groups <= 0 || maxq > 16)
        return 0;
    total = rows * groups;
    /* Full 16-pixel groups can be emitted by one QPU loop per row.  This
     * cuts a 320x240 fill from 400 SRQ launches to two with the default
     * 16-row batches while preserving the
     * exact-span kernel for clipped or unaligned rectangles. */
    if ((x0 & 15) == 0 && ((x1 - x0) & 15) == 0) {
        int32_t row_batch;
        /* When the rectangle spans the complete surface width, rows are
         * physically contiguous.  Let each QPU walk a small bounded row
         * block.  Very long VDW loops (thousands of groups in one thread)
         * can leave BCM2837's SRQ state wedged after the thread retires;
         * the bounded row count keeps the loop short while reducing
         * dispatches versus the original one-row launch scheme. */
        if (x0 == 0 && x1 == w) {
            const int32_t rows_per_qpu = VC4_FILL_ROWS_PER_QPU;
            int32_t row_batch;
            for (row_batch = 0; row_batch < rows;
                 row_batch += maxq * rows_per_qpu) {
                int32_t nqb = 0;
                for (q = 0; q < maxq; q++) {
                    int32_t row = row_batch + q * rows_per_qpu;
                    int32_t nrows;
                    uint32_t *s;
                    if (row >= rows)
                        break;
                    nrows = rows - row;
                    if (nrows > rows_per_qpu)
                        nrows = rows_per_qpu;
                    s = u + nqb * VC4_UNIF_QWORDS;
                    s[0] = (phys + (((uint32_t)y0 + (uint32_t)row) *
                                    (uint32_t)w) * 4u) |
                           VC4_BUS_ALIAS_DIRECT;
                    s[1] = color;
                    s[2] = (uint32_t)groups * (uint32_t)nrows;
                    nqb++;
                }
                q = gpu_vc4_run_once(g2d_qpu_argb_fill_loop_vc4,
                                     g2d_qpu_argb_fill_loop_vc4_n, u, nqb,
                                     NULL, 0, argb,
                                     (size_t)w * (size_t)h * 4u);
                if (q != 0)
                    return q > 0 && row_batch == 0 ? GPU_UNSUPPORTED :
                           GPU_FAILED;
            }
            return GPU_DONE;
        }
        for (row_batch = 0; row_batch < rows; row_batch += maxq) {
            int32_t nqb = rows - row_batch;
            if (nqb > maxq)
                nqb = maxq;
            for (q = 0; q < nqb; q++) {
                uint32_t *s = u + q * VC4_UNIF_QWORDS;
                int32_t row = row_batch + q;
                s[0] = (phys + (((uint32_t)y0 + (uint32_t)row) *
                                (uint32_t)w + (uint32_t)x0) * 4u) |
                       VC4_BUS_ALIAS_DIRECT;
                s[1] = color;
                s[2] = (uint32_t)groups;
            }
            q = gpu_vc4_run_once(g2d_qpu_argb_fill_loop_vc4,
                                 g2d_qpu_argb_fill_loop_vc4_n, u, nqb,
                                 NULL, 0, argb,
                                 (size_t)w * (size_t)h * 4u);
            if (q != 0)
                return q > 0 && row_batch == 0 ? GPU_UNSUPPORTED : GPU_FAILED;
        }
        return GPU_DONE;
    }
    /* Each QPU performs one VDW.  Work beyond the physical QPU count is
     * split into fully retired SRQ batches. */
    for (int32_t batch = 0; batch < total; batch += maxq) {
        int32_t nqb = total - batch;
        if (nqb > maxq)
            nqb = maxq;
        for (q = 0; q < nqb; q++) {
            uint32_t *s = u + q * VC4_UNIF_QWORDS;
            int32_t gid = batch + q;
            int32_t row = gid / groups;
            int32_t group = gid % groups;
            int32_t span = x1 - (x0 + group * 16);
            if (span > 16)
                span = 16;
            s[0] = (phys + (((uint32_t)y0 + (uint32_t)row) *
                             (uint32_t)w + (uint32_t)x0 +
                             (uint32_t)group * 16u) * 4u) |
                   VC4_BUS_ALIAS_DIRECT;
            s[1] = color;
            s[2] = 0x80804000u | ((uint32_t)span << 16);
        }
        q = gpu_vc4_run_once(g2d_qpu_argb_fill_vc4,
                             g2d_qpu_argb_fill_vc4_n, u, nqb,
                             NULL, 0, argb,
                             (size_t)w * (size_t)h * 4u);
        if (q > 0)
            return batch == 0 ? GPU_UNSUPPORTED : GPU_FAILED;
        if (q < 0)
            return GPU_FAILED;
    }
    return GPU_DONE;
}

/* Fast 1:1 row-copy blit on VC4, rebuilt around the proven VDR/VDW kernel
 * (simulator/vc4kernels/vc4_blit.qpu).  One QPU performs one exact-width
 * 1..16-word VDR/VDW span.  ARM flattens all rectangle (row, group) pairs
 * and submits fully retired batches; non-identity maps use the affine TMU
 * gather path below.  Per-QPU contract: u0=source span bus address,
 * u1=destination span bus address, u2/u3=dynamic VDR/VDW setups. */
static int gpu_blit_surface_vc4(const g2d_map_t *m,
                                uint32_t src_phys, uint32_t *argb_src,
                                int32_t src_w, int32_t src_h,
                                uint32_t dst_phys, uint32_t *argb_dst,
                                int32_t dst_w, int32_t dst_h,
                                int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t groups, maxq, rows, total, batch, nqb, q;
    int32_t sx0, sy0;

    if (!argb_src || !argb_dst || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 ||
        y1 <= y0)
        return 0;
    /* identity 1:1 map: u = X + cu, v = Y + cv */
    if (m->pu != (1 << 15) || m->qu != 0 || m->pv != 0 ||
        m->qv != (1 << 15))
        return 0;
    rows = y1 - y0;
    if (x1 > dst_w || y1 > dst_h || rows <= 0)
        return 0;
    groups = (x1 - x0 + 15) >> 4;
    maxq = v3d_g2d_num_qpus();
    if (groups <= 0 || maxq <= 0 || maxq > 16)
        return 0;
    sx0 = x0 + m->cu;
    sy0 = y0 + m->cv;
    if (sx0 < 0 || sy0 < 0 ||
        sx0 + (x1 - x0) > src_w || sy0 + rows > src_h)
        return 0;
    /* Aligned full-width identity copies are physically contiguous across
     * rows.  Use the loop kernel so each QPU retires a bounded row block
     * instead of one SRQ launch per 16-pixel span. */
    total = rows * groups;
    /* A short launch must use its actual QPU count; forcing maxq can wedge
     * SRQ before the first VDW on BCM2837. */
    for (batch = 0; batch < total; batch += maxq) {
        nqb = total - batch;
        if (nqb > maxq)
            nqb = maxq;
        for (q = 0; q < nqb; q++) {
            uint32_t *s = u + q * VC4_UNIF_QWORDS;
            if (q < nqb) {
                int32_t gid = batch + q;
                /* Group-major ordering keeps concurrent QPUs on distinct
                 * rows.  The only multi-QPU/multi-group probe verified on
                 * BCM2837 had exactly this row-separated shape. */
                int32_t group = gid / rows;
                int32_t row = gid % rows;
                int32_t span = x1 - (x0 + group * 16);
                if (span > 16)
                    span = 16;
                s[0] = (src_phys + (((uint32_t)sy0 + (uint32_t)row) *
                                    (uint32_t)src_w + (uint32_t)sx0 +
                                    (uint32_t)group * 16u) * 4u) |
                        VC4_BUS_ALIAS;
                s[1] = (dst_phys + (((uint32_t)y0 + (uint32_t)row) *
                                    (uint32_t)dst_w + (uint32_t)x0 +
                                    (uint32_t)group * 16u) * 4u) |
                        VC4_BUS_ALIAS_DIRECT;
                s[2] = 0x83010000u |
                       (span == 16 ? 0u : (uint32_t)span << 20);
                s[3] = 0x80804000u | ((uint32_t)span << 16);
            }
        }
        nqb = gpu_vc4_run_once(g2d_qpu_argb_blit_vc4,
                               g2d_qpu_argb_blit_vc4_n, u, nqb,
                               argb_src, (size_t)src_w * (size_t)src_h * 4u,
                               argb_dst, (size_t)dst_w * (size_t)dst_h * 4u);
        if (nqb > 0)
            return batch == 0 ? GPU_UNSUPPORTED : GPU_FAILED;
        if (nqb < 0)
            return GPU_FAILED;
    }
    return GPU_DONE;
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
        u[18] = (uint32_t)((int64_t)m->pu * 16);             /* pu16 */
        u[19] = (uint32_t)((int64_t)m->pv * 16);             /* pv16 */
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
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
           GPU_DONE : GPU_FAILED;
}

/* VC4 affine gather.  The ARM builds only source-address vectors; TMU0
 * performs every pixel read and VDW performs every destination write.
 * This avoids signed-mul24 limitations while retaining exact Q15 mapping.
 * Blits clamp out-of-range samples; rotations write transparent zero. */
static int gpu_affine_surface_vc4(const g2d_map_t *m, int transparent_oob,
                                  uint32_t src_phys, uint32_t *argb_src,
                                  int32_t src_w, int32_t src_h,
                                  uint32_t dst_phys, uint32_t *argb_dst,
                                  int32_t dst_w, int32_t dst_h,
                                  int32_t x0, int32_t y0,
                                  int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t groups, rows, maxq, q;
    int64_t total, batch;

    if (!m || !argb_src || !argb_dst || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 ||
        y1 <= y0 || x1 > dst_w || y1 > dst_h)
        return GPU_UNSUPPORTED;
    if (!gpu_map_fits(m, x1, y1))
        return GPU_UNSUPPORTED;
    groups = (x1 - x0 + 15) >> 4;
    rows = y1 - y0;
    maxq = v3d_g2d_num_qpus();
    if (groups <= 0 || rows <= 0 || maxq <= 0 || maxq > 16)
        return GPU_UNSUPPORTED;
    total = (int64_t)groups * rows;
    for (batch = 0; batch < total; batch += maxq) {
        int32_t nqb = (int32_t)(total - batch);
        uint32_t *addrv;
        uint32_t addrv_phys;
        uint32_t zero_addr;

        if (nqb > maxq)
            nqb = maxq;
        addrv = (uint32_t *)gpu_dma_alloc(
                    ((size_t)nqb * 16u + 1u) * 4u, &addrv_phys);
        if (!addrv)
            return batch == 0 ? GPU_UNSUPPORTED : GPU_FAILED;
        addrv[(size_t)nqb * 16u] = 0u;
        zero_addr = (addrv_phys + (uint32_t)nqb * 16u * 4u) |
                    VC4_BUS_ALIAS;

        for (q = 0; q < nqb; q++) {
            uint32_t *s = u + (uint32_t)q * VC4_UNIF_QWORDS;
            uint32_t *av = addrv + (uint32_t)q * 16u;
            int64_t gid = batch + q;
            int32_t row = (int32_t)(gid / groups);
            int32_t group = (int32_t)(gid % groups);
            int32_t X0 = x0 + group * 16;
            int32_t Y = y0 + row;
            int32_t span = x1 - X0;
            int32_t lane;

            if (span > 16)
                span = 16;
            for (lane = 0; lane < 16; lane++) {
                int32_t X = X0 + (lane < span ? lane : span - 1);
                int64_t up = (int64_t)m->pu * X + (int64_t)m->qu * Y;
                int64_t vp = (int64_t)m->pv * X + (int64_t)m->qv * Y;
                int64_t sx = (up >> 15) + m->cu;
                int64_t sy = (vp >> 15) + m->cv;

                if (transparent_oob &&
                    (sx < 0 || sy < 0 || sx >= src_w || sy >= src_h)) {
                    av[lane] = zero_addr;
                    continue;
                }
                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                if (sx >= src_w) sx = src_w - 1;
                if (sy >= src_h) sy = src_h - 1;
                av[lane] = (src_phys +
                            ((uint32_t)sy * (uint32_t)src_w +
                             (uint32_t)sx) * 4u) | VC4_BUS_ALIAS;
            }
            s[0] = (addrv_phys + (uint32_t)q * 16u * 4u) |
                   VC4_BUS_ALIAS;
            s[1] = (dst_phys +
                    ((uint32_t)Y * (uint32_t)dst_w + (uint32_t)X0) * 4u) |
                   VC4_BUS_ALIAS_DIRECT;
            s[2] = 0x80804080u | ((uint32_t)span << 16);
            s[3] = (uint32_t)q * 4u;
        }
        q = gpu_vc4_run_once(g2d_qpu_argb_gather_vc4,
                             g2d_qpu_argb_gather_vc4_n, u, nqb,
                             argb_src, (size_t)src_w * (size_t)src_h * 4u,
                             argb_dst, (size_t)dst_w * (size_t)dst_h * 4u);
        if (q != 0)
            return GPU_FAILED;
    }
    return GPU_DONE;
}

/* clamped-edge blit (argb_blit kernel) */
static int gpu_blit_surface(const g2d_map_t *m,
                            uint32_t src_phys, uint32_t *argb_src,
                            int32_t src_w, int32_t src_h,
                            uint32_t dst_phys, uint32_t *argb_dst,
                            int32_t dst_w, int32_t dst_h,
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (v3d_g2d_ver() == 21) {
        /* L2CACTL/SLCACTL cannot invalidate BCM2837's shared system L3.
         * Evict it once for the whole public operation, before any batch
         * reads caller memory. */
        if (v3d_g2d_vc4_prepare_reads() != 0)
            return GPU_FAILED;
        int gr = gpu_blit_surface_vc4(m, src_phys, argb_src, src_w, src_h,
                                      dst_phys, argb_dst, dst_w, dst_h,
                                      x0, y0, x1, y1);
        if (gr != GPU_UNSUPPORTED)
            return gr;
        return gpu_affine_surface_vc4(m, 0, src_phys, argb_src,
                                      src_w, src_h, dst_phys, argb_dst,
                                      dst_w, dst_h, x0, y0, x1, y1);
    }
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
    if (v3d_g2d_ver() == 21) {
        if (v3d_g2d_vc4_prepare_reads() != 0)
            return GPU_FAILED;
        return gpu_affine_surface_vc4(m, 1, src_phys, argb_src,
                                      src_w, src_h, dst_phys, argb_dst,
                                      dst_w, dst_h, 0, 0, dst_w, dst_h);
    }
    return gpu_affine_surface(g2d_qpu_argb_rotate, g2d_qpu_argb_rotate_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              0, 0, dst_w, dst_h);
}

/* Alpha blend of a clipped dst rect.  V3D >= 4 uses the CSD kernel below;
 * VC4 selects its staged VDR/TMU pipeline in gpu_alpha_surface_vc4(). */
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
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
           GPU_DONE : GPU_FAILED;
}

/* VC4 source-over pipeline.  Short gather kernels stage source and
 * destination pixels, then the long blend kernel processes one span per
 * QPU in a single SRQ batch.  Each QPU derives a private VPM window from
 * its physical qpu_number. */
static int gpu_alpha_surface_vc4(const g2d_map_t *m, uint8_t alpha,
                                 uint32_t src_phys, uint32_t *argb_src,
                                 int32_t src_w, int32_t src_h,
                                 uint32_t dst_phys, uint32_t *argb_dst,
                                 int32_t dst_w, int32_t dst_h,
                                 int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1)
{
    uint32_t us[16 * VC4_UNIF_QWORDS] = { 0 };
    uint32_t ud[16 * VC4_UNIF_QWORDS] = { 0 };
    uint32_t ua[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t groups, rows, maxq, q;
    int64_t total, batch;

    if (!m || !argb_src || !argb_dst || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 ||
        y1 <= y0 || x1 > dst_w || y1 > dst_h)
        return GPU_UNSUPPORTED;
    if (!gpu_map_fits(m, x1, y1))
        return GPU_UNSUPPORTED;
    groups = (x1 - x0 + 15) >> 4;
    rows = y1 - y0;
    maxq = v3d_g2d_num_qpus();
    if (groups <= 0 || rows <= 0 || maxq <= 0 || maxq > 16)
        return GPU_UNSUPPORTED;
    if (v3d_g2d_vc4_prepare_reads() != 0)
        return GPU_FAILED;
    total = (int64_t)groups * rows;
    for (batch = 0; batch < total; batch += maxq) {
        int32_t nqb = (int32_t)(total - batch);
        uint32_t *addrv, *stage;
        uint32_t addrv_phys, stage_phys;

        if (nqb > maxq)
            nqb = maxq;
        /* These blocks are consumed by the three launches below and are
         * then fully retired before the next batch allocates from the
         * staging ring.  Per-batch allocation is required: keeping one
         * pair for the whole operation lets uniform allocations wrap the
         * ring and overwrite that pair during large alpha operations. */
        addrv = (uint32_t *)gpu_dma_alloc((size_t)nqb * 16u * 4u,
                                          &addrv_phys);
        stage = (uint32_t *)gpu_dma_alloc((size_t)nqb * 32u * 4u,
                                          &stage_phys);
        if (!addrv || !stage)
            return batch == 0 ? GPU_UNSUPPORTED : GPU_FAILED;
        for (q = 0; q < nqb; q++) {
            uint32_t *s = us + (uint32_t)q * VC4_UNIF_QWORDS;
            uint32_t *d = ud + (uint32_t)q * VC4_UNIF_QWORDS;
            uint32_t *a = ua + (uint32_t)q * VC4_UNIF_QWORDS;
            uint32_t *sav = addrv + (uint32_t)q * 16u;
            int64_t gid = batch + q;
            int32_t row = (int32_t)(gid / groups);
            int32_t group = (int32_t)(gid % groups);
            int32_t X0 = x0 + group * 16;
            int32_t Y = y0 + row;
            int32_t span = x1 - X0;
            int32_t lane;

            if (span > 16)
                span = 16;
            for (lane = 0; lane < 16; lane++) {
                int32_t X = X0 + (lane < span ? lane : span - 1);
                int64_t up = (int64_t)m->pu * X + (int64_t)m->qu * Y;
                int64_t vp = (int64_t)m->pv * X + (int64_t)m->qv * Y;
                int64_t sx = (up >> 15) + m->cu;
                int64_t sy = (vp >> 15) + m->cv;

                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                if (sx >= src_w) sx = src_w - 1;
                if (sy >= src_h) sy = src_h - 1;
                sav[lane] = (src_phys +
                             ((uint32_t)sy * (uint32_t)src_w +
                              (uint32_t)sx) * 4u) | VC4_BUS_ALIAS;
            }
            s[0] = (addrv_phys + (uint32_t)q * 16u * 4u) |
                   VC4_BUS_ALIAS;
            s[1] = (stage_phys + (uint32_t)q * 32u * 4u) |
                   VC4_BUS_ALIAS;
            s[2] = 0x80804080u | ((uint32_t)span << 16);
            s[3] = (uint32_t)q * 4u;
            d[0] = (dst_phys + ((uint32_t)Y * (uint32_t)dst_w +
                                (uint32_t)X0) * 4u) |
                   VC4_BUS_ALIAS;
            d[1] = (stage_phys +
                    ((uint32_t)q * 32u + 16u) * 4u) |
                   VC4_BUS_ALIAS;
            d[2] = 0x83010000u |
                   (span == 16 ? 0u : (uint32_t)span << 20);
            d[3] = 0x80804000u | ((uint32_t)span << 16);
            a[0] = (stage_phys + (uint32_t)q * 32u * 4u) |
                   VC4_BUS_ALIAS;
            a[1] = (dst_phys + ((uint32_t)Y * (uint32_t)dst_w +
                                (uint32_t)X0) * 4u) |
                   VC4_BUS_ALIAS_DIRECT;
            a[2] = alpha == 0xff ? 256u : (uint32_t)alpha;
            /* The kernel derives its private four-row VPM window from the
             * physical qpu_number; SRQ slot order is not a stable QPU id. */
            a[3] = 0u;
            a[4] = (uint32_t)span;
        }
        q = gpu_vc4_run_once(g2d_qpu_argb_gather_vc4,
                             g2d_qpu_argb_gather_vc4_n, us, nqb,
                             argb_src, (size_t)src_w * (size_t)src_h * 4u,
                             stage, (size_t)nqb * 32u * 4u);
        if (q != 0)
            return GPU_FAILED;
        q = gpu_vc4_run_once(g2d_qpu_argb_blit_vc4,
                             g2d_qpu_argb_blit_vc4_n, ud, nqb,
                             argb_dst, (size_t)dst_w * (size_t)dst_h * 4u,
                             stage, (size_t)nqb * 32u * 4u);
        if (q != 0)
            return GPU_FAILED;
        /* Private VPM windows let all spans blend in one SRQ launch. */
        q = gpu_vc4_run_once(g2d_qpu_argb_alpha_vc4,
                             g2d_qpu_argb_alpha_vc4_n, ua, nqb,
                             stage, (size_t)nqb * 32u * 4u,
                             argb_dst,
                             (size_t)dst_w * (size_t)dst_h * 4u);
        if (q != 0)
            return GPU_FAILED;
    }
    return GPU_DONE;
}

/* ------------------------------------------------------------------ */
/* bsp_g2d API                                                         */
/* ------------------------------------------------------------------ */

int32_t bsp_g2d_init(void)
{
    return v3d_g2d_init();
}

uint32_t bsp_g2d_clock_hz(void)
{
    return v3d_g2d_clock_hz();
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
        if (v3d_g2d_ver() == 21)
            gr = gpu_fill_surface_vc4(phys, argb, argb_w, argb_h, rx, ry,
                                      rx + rw, ry + rh, color);
        else
            gr = gpu_fill_surface(phys, argb, argb_w, argb_h, rx, ry,
                                  rx + rw, ry + rh, color);
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
                                  rx + rw, ry + rh);
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
                                  0, 0, dst_w, dst_h);
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
