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
 * bsp_g2d_blt_alpha uses a dedicated source-over pipeline.  V3D >= 4
 * blends through the CSD kernel; on VC4 the self-looping TMU blend
 * kernel gathers source and destination, blends in the ALU and writes
 * back through the direct bus alias - one SRQ launch per 12 rows.
 *
 * The rotate kernel runs the same affine engine as blit for every angle
 * (the canonical centres-based map degenerates exactly to the right-angle
 * coefficient sets) and writes every destination pixel: rotated content,
 * or transparent 0 for pixels whose pre-clamp source coordinate is out of
 * range (a five-instruction flag chain, then `mov ifna <data>, 0` right
 * after the LDTMU).  The clamp is kept so out-of-range TMU reads stay in
 * the source buffer.
 *
 * VC4 path (Pi3, V3D 2.1): fill dispatches the self-looping group-fill
 * kernel (one SRQ launch covers up to 12 x 512 groups); blit, scale and
 * rotate dispatch the self-looping TMU copy kernel whenever the Q15 map
 * has exact integer coefficients (identity blits, right-angle rotates,
 * 1:1 scales) - one launch per 12 destination rows.  Every loop thread
 * is bounded by G2D_BAND_MAX_VDW iterations, and the exact 1..16-pixel
 * span/gather batches remain the verified fallback for everything the
 * opaque loop kernels decline (narrow rects, fractional maps) and for
 * a rejected first launch.  Alpha has deliberately no fallback: blends
 * are not idempotent, so the loop kernel validates everything up front
 * and any decline or failure reports -1 to the caller.
 * Public hardware APIs are GPU-only.
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
#include "g2d_qpu_kernels_v42.h"

#define G2D_MAX_COEF (1 << 23)  /* |map coefficient| must fit smul24 */

/* VC bus aliases for VC4 (V3D 2.1). Reads, code and uniforms use the
 * L2-cached alias. VDW destinations use the direct alias: a cached GPU
 * write followed by a Normal-NC ARM rewrite can otherwise be resurrected
 * by a later L2 clear and overwrite the newer ARM data. Raw ARM physical
 * addresses wedge the QPU memory pipe on real Pi3 hardware. V3D >= 4.x
 * CSD paths take plain physical addresses and must not be aliased. */
#define VC4_BUS_ALIAS 0x40000000u
#define VC4_BUS_ALIAS_DIRECT 0xc0000000u

/* Hard ceiling on the VDW transactions one looping-kernel QPU thread may
 * issue in a single launch.
 *
 * The loop kernels iterate INSIDE one thread - one VDW per 16-pixel
 * group - so the iteration count is the number of VDW DMAs a thread has
 * outstanding before it retires.  On BCM2837 with the Mar-2023 firmware
 * that count is the wedge trigger: a thread asked for 640 iterations
 * retires cleanly, 800 retires cleanly, and 1280 wedges VideoCore for
 * good (no SRQ completion, GPU dead until power cycle).  The cap is what
 * makes the looping shape usable at all.
 *
 * 512 sits 2.5x below the smallest observed failure and 1.6x below the
 * smallest observed success, and still lets one thread walk a whole
 * 1920-wide destination row (120 groups) with 4x headroom. */
#define G2D_BAND_MAX_VDW 512

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
    int32_t g0 = x0 >> 4;
    int32_t g1 = (int32_t)(((uint32_t)x1 + 15u) >> 4);
    int32_t L = g1 - g0;
    int32_t band_h = y1 - y0;
    int full = ((w & 15) == 0 &&
                x0 == 0 && y0 == 0 && x1 == w && y1 == h);
    int nq, rows;

    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 ||
        x1 <= x0 || y1 <= y0 || x1 > w || y1 > h)
        return 0;
    if (v3d_g2d_ver() == 42 && x0 == 0 && y0 == 0 && x1 == w && y1 == h) {
        int rc = v3d_g2d_render_clear(phys, (uint32_t)w * 4u,
                                      (uint32_t)w, (uint32_t)h, color);
        return rc == 0 ? GPU_DONE : GPU_FAILED;
    }
    /* One dispatch covers the whole surface.  The legal V3D thread-end
     * sequence retires every QPU cleanly, so no 16 KiB batching or dummy
     * TMU transaction is needed. */
    nq = v3d_g2d_num_qpus();
    if (nq > band_h)
        nq = band_h;
    rows = (band_h + nq - 1) / nq;
    u[0] = color;
    u[1] = (uint32_t)(L - 1);
    u[2] = (uint32_t)(w * 4u);
    u[3] = (uint32_t)(y1 - 1);
    u[4] = (uint32_t)(L * 64u - (uint32_t)w * 4u);
    u[5] = (uint32_t)(L * rows);
    u[6] = phys + ((uint32_t)y0 * (uint32_t)w +
                   (uint32_t)g0 * 16u) * 4u;
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

/* VC4 exact fill fallback, one SRQ/QPU thread per 1..16-pixel span.  Each
 * thread performs one VDW and the ARM batches spans at the physical QPU
 * count.  Used where gpu_fill_loop_vc4() declines (rects narrower than
 * one 16-pixel group, or a rejected first launch) and for the 1..15
 * pixel right-edge tail of a loop fill. */
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
    /* One 1..16-pixel span per QPU per launch, with work beyond the
     * physical QPU count split into fully retired SRQ batches.
     *
     * This shape pays a launch per maxq spans, and the launch is almost
     * all of the cost.  Measured on Pi3 at 640x480 (19200 spans, 1600
     * launches) a blit frame takes 15807 us - 9.9 us per launch, or
     * 0.82 us per span - while a looping kernel retires the same 19200
     * VDWs in a handful of launches.  So the loop path is the primary
     * one and this is the fallback.
     *
     * What this shape buys is a hard bound on thread lifetime: no launch
     * here asks a thread for more than one VDW, so the BCM2837 iteration
     * wedge cannot be reached at any surface size.  The loop path gets
     * the same bound from G2D_BAND_MAX_VDW instead of from the kernel
     * shape, which is why it can afford to be fast.
     *
     * The L2 and slice cache clears around each dispatch are NOT
     * negotiable: eliding them was tried and measured ~4x slower per
     * launch, because a bulk invalidate is cheaper than letting a 1.2 MiB
     * source stream thrash-evict its way through the 128 KiB V3D L2, and
     * they are also the documented cure for stale QPU code/uniform
     * fetches.  With the loop path there are only a few launches per
     * frame instead of 1600, so that cost stops mattering.
     *
     * The span kernel is also the fill path the pixel-checking
     * functional tests verified on silicon, so it stays reachable as
     * the trusted fallback. */
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

/* Primary VC4 fill: the self-looping group-fill kernel.  Each QPU thread
 * writes one 16-pixel group per iteration (one VDW each) and loops up to
 * G2D_BAND_MAX_VDW times, so a full-width rect retires in a handful of
 * launches instead of one launch per 12 spans.
 *
 * Two shapes:
 *   - the rect spans the full surface width and starts at x0 == 0: the
 *     groups form one contiguous run, so it is cut into equal per-thread
 *     slices regardless of row boundaries (a 640x480 clear is 4 launches);
 *   - otherwise one thread per destination row, 12 rows per launch.
 * The kernel decrements its count until zero, so a thread must NEVER be
 * handed count == 0 (it would loop 2^32 times); both shapes guarantee
 * count >= 1 per launched thread.  The 1..15-pixel right-edge tail is
 * finished by the exact span kernel. */
static int gpu_fill_loop_vc4(uint32_t phys, uint32_t *argb,
                             int32_t w, int32_t h, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, uint32_t color)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t maxq = v3d_g2d_num_qpus();
    int32_t L, tail_x, r;
    int launched = 0;

    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 ||
        x1 > w || y1 > h || maxq <= 0 || maxq > 16)
        return GPU_UNSUPPORTED;
    L = (x1 - x0) >> 4;
    tail_x = x0 + L * 16;
    if (L <= 0)
        return GPU_UNSUPPORTED;
    if (x0 == 0 && tail_x == w) {
        /* contiguous run: rows abut in memory, slice by group count */
        int64_t total = (int64_t)L * (y1 - y0), done = 0;
        uint32_t base = phys + (uint32_t)y0 * (uint32_t)w * 4u;

        while (done < total) {
            int64_t left = total - done;
            int32_t iters = (int32_t)((left + maxq - 1) / maxq);
            int32_t nq, q;
            int64_t covered;

            if (iters > G2D_BAND_MAX_VDW)
                iters = G2D_BAND_MAX_VDW;
            nq = (int32_t)((left + iters - 1) / iters);
            if (nq > maxq)
                nq = maxq;
            for (q = 0; q < nq; q++) {
                uint32_t *s = u + q * VC4_UNIF_QWORDS;
                int64_t off = done + (int64_t)q * iters;
                int64_t cnt = left - (int64_t)q * iters;

                if (cnt > iters)
                    cnt = iters;
                s[0] = (base + (uint32_t)(off * 64)) | VC4_BUS_ALIAS_DIRECT;
                s[1] = color;
                s[2] = (uint32_t)cnt;
            }
            r = gpu_vc4_run_once(g2d_qpu_argb_fill_loop_vc4,
                                 g2d_qpu_argb_fill_loop_vc4_n, u, nq,
                                 NULL, 0, argb,
                                 (size_t)w * (size_t)h * 4u);
            if (r != 0)
                return launched ? GPU_FAILED : GPU_UNSUPPORTED;
            launched = 1;
            covered = (int64_t)nq * iters;
            if (covered > left)
                covered = left;
            done += covered;
        }
    } else {
        /* one thread per destination row, maxq rows per launch */
        int32_t y;

        if (L > G2D_BAND_MAX_VDW)
            return GPU_UNSUPPORTED;
        for (y = y0; y < y1; y += maxq) {
            int32_t nq = y1 - y, q;

            if (nq > maxq)
                nq = maxq;
            for (q = 0; q < nq; q++) {
                uint32_t *s = u + q * VC4_UNIF_QWORDS;

                s[0] = (phys + (((uint32_t)(y + q)) * (uint32_t)w +
                                (uint32_t)x0) * 4u) | VC4_BUS_ALIAS_DIRECT;
                s[1] = color;
                s[2] = (uint32_t)L;
            }
            r = gpu_vc4_run_once(g2d_qpu_argb_fill_loop_vc4,
                                 g2d_qpu_argb_fill_loop_vc4_n, u, nq,
                                 NULL, 0, argb,
                                 (size_t)w * (size_t)h * 4u);
            if (r != 0)
                return launched ? GPU_FAILED : GPU_UNSUPPORTED;
            launched = 1;
        }
    }
    if (tail_x < x1) {
        /* 1..15-pixel right-edge tail via the exact span kernel */
        r = gpu_fill_surface_vc4(phys, argb, w, h, tail_x, y0, x1, y1,
                                 color);
        if (r != GPU_DONE)
            return launched ? GPU_FAILED : r;
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
    total = rows * groups;
    /* One 1..16-pixel VDR/VDW span per QPU per launch.  An aligned
     * full-width copy is physically contiguous across rows and so could be
     * walked by a single loop thread, but that shape is both the slower one
     * and the BCM2837 wedge source; see the measured argument in
     * gpu_fill_surface_vc4().
     *
     * A short launch must use its actual QPU count; forcing maxq can wedge
     * SRQ before the first VDW on BCM2837. */
    for (batch = 0; batch < total; batch += maxq) {
        nqb = total - batch;
        if (nqb > maxq)
            nqb = maxq;
        for (q = 0; q < nqb; q++) {
            uint32_t *s = u + q * VC4_UNIF_QWORDS;
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
                              int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                              int no_clamp)
{
    uint32_t u[26];
    int32_t g0 = x0 >> 4;
    int32_t g1 = (int32_t)(((uint32_t)x1 + 15u) >> 4);
    int32_t L = g1 - g0;
    int32_t band_h = y1 - y0;
    int nq = v3d_g2d_num_qpus();
    int32_t rows;
    int full = ((dst_w & 15) == 0 &&
                x0 == 0 && y0 == 0 && x1 == dst_w && y1 == dst_h);

    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 ||
        x1 <= x0 || y1 <= y0 || x1 > dst_w || y1 > dst_h)
        return 0;
    if (!gpu_map_fits(m, x1, y1))
        return 0;
    if (nq > band_h)
        nq = band_h;
    rows = (band_h + nq - 1) / nq;
    u[0] = (uint32_t)m->pu;
    u[1] = (uint32_t)m->qu;
    u[2] = (uint32_t)m->pv;
    u[3] = (uint32_t)m->qv;
    u[4] = (uint32_t)m->cu;
    u[5] = (uint32_t)m->cv;
    u[6] = (uint32_t)(src_w - 1);
    u[7] = (uint32_t)(src_h - 1);
    u[8] = (uint32_t)src_w;
    u[9] = dst_phys + ((uint32_t)y0 * (uint32_t)dst_w +
                       (uint32_t)g0 * 16u) * 4u;
    u[10] = src_phys;
    u[11] = (uint32_t)(L - 1);
    u[12] = (uint32_t)dst_w * 4u;
    u[13] = (uint32_t)(y1 - 1);
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
    u[24] = (uint32_t)no_clamp;
    u[25] = 0;
    if (kcode == g2d_qpu_argb_blit && full) {
        if (src_w == dst_w && src_h == dst_h &&
            m->pu == 32768 && m->qu == 0 &&
            m->pv == 0 && m->qv == 32768 &&
            m->cu == 0 && m->cv == 0)
            u[25] = 1;       /* exact linear copy */
        else if (no_clamp)
            u[25] = 2;       /* full-surface in-bounds scale */
    }
    return v3d_g2d_run(kcode, knwords, u, 26,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
           GPU_DONE : GPU_FAILED;
}

/* VC4 affine gather.  The ARM builds only source-address vectors; TMU0
 * performs every pixel read and VDW performs every destination write.
 * This avoids signed-mul24 limitations while retaining exact Q15 mapping.
 * Blits clamp out-of-range samples; rotations write transparent zero.
 * Fallback for maps gpu_copy_loop_vc4() declines. */
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
            /* Walk the map incrementally.  The coefficients are constant
             * across a span, so consecutive lanes advance the source
             * position by exactly (pu, pv) in Q15 and two int64 multiplies
             * per QPU replace two per lane.  That matters because this loop
             * is the ARM-side half of the affine dispatch cost: a 45-degree
             * rotate of a 640x480 source walks ~39600 spans, ~633k lanes.
             * Lanes past the span end clamp to the last sampled pixel, so
             * they stop advancing - the same value the per-lane form got
             * from X0 + span - 1. */
            int64_t up = (int64_t)m->pu * X0 + (int64_t)m->qu * Y;
            int64_t vp = (int64_t)m->pv * X0 + (int64_t)m->qv * Y;

            if (span > 16)
                span = 16;
            for (lane = 0; lane < 16; lane++) {
                int64_t sx = (up >> 15) + m->cu;
                int64_t sy = (vp >> 15) + m->cv;

                if (lane + 1 < span) {
                    up += m->pu;
                    vp += m->pv;
                }

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

/* Primary VC4 blit/rotate path: the self-looping TMU copy kernel.  It
 * handles exactly the Q15 maps whose coefficients are whole pixels -
 * identity blits, right-angle rotates, 1:1 scale sub-rects - where each
 * destination row samples the source at a constant byte step, so one
 * thread copies a whole row (one 16-lane linear TMU gather + one VDW per
 * group) and one launch covers maxq rows.  Everything else (fractional
 * scales, 45-degree rotates) is declined to the exact gather path.
 *
 * Correctness guards:
 *   - all four corners of the full-group region must land inside the
 *     source, so the kernel needs no clamp and blit/rotate out-of-source
 *     policies never differ inside the loop region;
 *   - the lane offset is built with unsigned mul24 (lane * |step|), so
 *     |step| is bounded; the signed part is a separate subtract;
 *   - L <= G2D_BAND_MAX_VDW bounds thread lifetime, and count >= 1 is
 *     guaranteed (a zero count would loop 2^32 times).
 * The 1..15-pixel right-edge tail keeps the operation's own OOB policy
 * via the exact gather path (transparent_oob). */
static int gpu_copy_loop_vc4(const g2d_map_t *m, int transparent_oob,
                             uint32_t src_phys, uint32_t *argb_src,
                             int32_t src_w, int32_t src_h,
                             uint32_t dst_phys, uint32_t *argb_dst,
                             int32_t dst_w, int32_t dst_h,
                             int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t maxq = v3d_g2d_num_qpus();
    int32_t ipu, iqu, ipv, iqv, L, tail_x, y;
    int64_t sb;
    int launched = 0;

    if (!m || !argb_src || !argb_dst || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 ||
        y1 <= y0 || x1 > dst_w || y1 > dst_h || maxq <= 0 || maxq > 16)
        return GPU_UNSUPPORTED;
    if ((m->pu | m->qu | m->pv | m->qv) & 32767)
        return GPU_UNSUPPORTED;          /* integer maps only */
    ipu = m->pu >> 15;
    iqu = m->qu >> 15;
    ipv = m->pv >> 15;
    iqv = m->qv >> 15;
    L = (x1 - x0) >> 4;
    tail_x = x0 + L * 16;
    if (L <= 0 || L > G2D_BAND_MAX_VDW)
        return GPU_UNSUPPORTED;
    /* per-lane source byte step along a destination row */
    sb = ((int64_t)ipu + (int64_t)ipv * src_w) * 4;
    if (sb > (1 << 20) || sb < -(1 << 20))
        return GPU_UNSUPPORTED;          /* mul24 lane-offset range */
    {
        /* all 4 corners of the full-group region must be in-source */
        int32_t xs[2], ys[2], i, j;

        xs[0] = x0; xs[1] = tail_x - 1;
        ys[0] = y0; ys[1] = y1 - 1;
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 2; j++) {
                int64_t su = (int64_t)ipu * xs[i] + (int64_t)iqu * ys[j] +
                             m->cu;
                int64_t sv = (int64_t)ipv * xs[i] + (int64_t)iqv * ys[j] +
                             m->cv;

                if (su < 0 || sv < 0 || su >= src_w || sv >= src_h)
                    return GPU_UNSUPPORTED;
            }
        }
    }
    for (y = y0; y < y1; y += maxq) {
        int32_t nq = y1 - y, q, r;

        if (nq > maxq)
            nq = maxq;
        for (q = 0; q < nq; q++) {
            uint32_t *s = u + q * VC4_UNIF_QWORDS;
            int32_t Y = y + q;
            int64_t su = (int64_t)ipu * x0 + (int64_t)iqu * Y + m->cu;
            int64_t sv = (int64_t)ipv * x0 + (int64_t)iqv * Y + m->cv;

            s[0] = (src_phys + (uint32_t)((sv * src_w + su) * 4)) |
                   VC4_BUS_ALIAS;
            s[1] = (dst_phys + ((uint32_t)Y * (uint32_t)dst_w +
                                (uint32_t)x0) * 4u) | VC4_BUS_ALIAS_DIRECT;
            s[2] = (uint32_t)L;
            s[3] = sb >= 0 ? (uint32_t)sb : 0u;      /* lane step, add part */
            s[4] = sb >= 0 ? 0u : (uint32_t)(-sb);   /* lane step, sub part */
            s[5] = (uint32_t)(int32_t)(sb * 16);     /* per-group src step */
            s[6] = 0x80904080u;                      /* VDW setup base */
        }
        r = gpu_vc4_run_once(g2d_qpu_argb_copy_loop_vc4,
                             g2d_qpu_argb_copy_loop_vc4_n, u, nq,
                             argb_src, (size_t)src_w * (size_t)src_h * 4u,
                             argb_dst, (size_t)dst_w * (size_t)dst_h * 4u);
        if (r != 0)
            return launched ? GPU_FAILED : GPU_UNSUPPORTED;
        launched = 1;
    }
    if (tail_x < x1) {
        /* unaligned tail columns via the exact gather path */
        int r = gpu_affine_surface_vc4(m, transparent_oob, src_phys,
                                       argb_src, src_w, src_h, dst_phys,
                                       argb_dst, dst_w, dst_h,
                                       tail_x, y0, x1, y1);
        if (r != GPU_DONE)
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
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int no_clamp)
{
    if (v3d_g2d_ver() == 21) {
        /* L2CACTL/SLCACTL cannot invalidate BCM2837's shared system L3.
         * Evict it once for the whole public operation, before any batch
         * reads caller memory. */
        if (v3d_g2d_vc4_prepare_reads() != 0)
            return GPU_FAILED;
        /* looping copy kernel first: one launch per 12 rows instead of
         * one launch per 12 spans (see the measured argument in
         * gpu_fill_surface_vc4()) */
        int gr = gpu_copy_loop_vc4(m, 0, src_phys, argb_src,
                                   src_w, src_h, dst_phys, argb_dst,
                                   dst_w, dst_h, x0, y0, x1, y1);
        if (gr != GPU_UNSUPPORTED)
            return gr;
        gr = gpu_blit_surface_vc4(m, src_phys, argb_src, src_w, src_h,
                                  dst_phys, argb_dst, dst_w, dst_h,
                                  x0, y0, x1, y1);
        if (gr != GPU_UNSUPPORTED)
            return gr;
        return gpu_affine_surface_vc4(m, 0, src_phys, argb_src,
                                      src_w, src_h, dst_phys, argb_dst,
                                      dst_w, dst_h, x0, y0, x1, y1);
    }
    if (v3d_g2d_ver() == 42 && src_w == dst_w && src_h == dst_h &&
        x0 == 0 && y0 == 0 && x1 == dst_w && y1 == dst_h &&
        m->pu == 32768 && m->qu == 0 && m->pv == 0 && m->qv == 32768 &&
        m->cu == 0 && m->cv == 0) {
        int rc = v3d_g2d_render_copy(src_phys, dst_phys,
                                     (uint32_t)dst_w * 4u,
                                     (uint32_t)dst_w, (uint32_t)dst_h);
        return rc == 0 ? GPU_DONE : GPU_FAILED;
    }
    return gpu_affine_surface(g2d_qpu_argb_blit, g2d_qpu_argb_blit_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              x0, y0, x1, y1, no_clamp);
}

/* The dedicated 90/270 kernel consumes four source rows per inner pass.
 * Choose the largest QPU count whose band height is an exact multiple of
 * four, so every QPU ends on a band boundary. */
static int g2d_fit_nq_rot90_block(int nq, int32_t h, int32_t block)
{
    if (nq > h)
        nq = h;
    while (nq >= 1) {
        if (h % nq == 0) {
            int32_t rows = h / nq;
            if (rows >= block && rows % block == 0)
                return nq;
        }
        nq--;
    }
    return 0;
}

/* Dedicated right-angle path: source reads are contiguous and only writes
 * stride, avoiding the generic affine kernel's 16 stride-apart TMU reads. */
static int gpu_rot90_surface(uint32_t src_phys, uint32_t *argb_src,
                             int32_t src_w, int32_t src_h,
                             uint32_t dst_phys, uint32_t *argb_dst,
                             int32_t dst_w, int32_t dst_h, int rot)
{
    uint32_t u[18];
    uint32_t tu[14];
    int32_t L = (src_w + 15) >> 4;
    int32_t rows, ss, ds;
    int nq;
    int r90 = (rot == 90);

    if (src_w < 16 || src_h < 4 ||
        (!r90 && (src_w & 15) != 0) ||
        dst_w != src_h || dst_h != src_w)
        return GPU_UNSUPPORTED;
    ss = src_w * 4;
    ds = dst_w * 4;
#if V42_ROT90_VPM_PROBE
    /* Bring-up only.  Mode 1 isolates the VPM algorithm on one workgroup;
     * mode 2 exercises concurrent workgroups.  Production remains on the
     * proven 4x4 ROT path until both modes are exact on real BCM2711. */
    if (v3d_g2d_ver() == 42 && r90 && (src_w & 15) == 0) {
        nq = V42_ROT90_VPM_PROBE == 1 ? 1 :
             g2d_fit_nq_rot90_block(v3d_g2d_num_qpus(), src_h, 16);
        if (nq == 0)
            return GPU_UNSUPPORTED;
        rows = src_h / nq;
        tu[0] = src_phys;
        tu[1] = dst_phys;
        tu[2] = (uint32_t)((int64_t)rows * ss);
        tu[3] = (uint32_t)(((int64_t)rows - 1) * ss);
        tu[4] = (uint32_t)(-(int64_t)ss);
        tu[5] = (uint32_t)((int64_t)rows * ss + 64);
        tu[6] = (uint32_t)(-((int64_t)rows * 4));
        tu[7] = (uint32_t)(((int64_t)src_h - rows) * 4);
        tu[8] = (uint32_t)((int64_t)16 * ds - (int64_t)rows * 4);
        tu[9] = (uint32_t)(rows / 16);
        tu[10] = (uint32_t)((int64_t)rows * L / 16);
        tu[11] = v3d_g2d_scratch_phys();
        tu[12] = (uint32_t)nq;
        tu[13] = (uint32_t)ds;
        return v3d_g2d_run(g2d_qpu_v42_argb_rot90_vpm,
                           g2d_qpu_v42_argb_rot90_vpm_n, tu, 14,
                           nq, argb_src, (size_t)src_w * src_h * 4,
                           argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
               GPU_DONE : GPU_FAILED;
    }
#endif
    /* The 4x4 ROT path is fully QPU/GPU accelerated and has exact
     * multi-workgroup results on BCM2711. */
    nq = g2d_fit_nq_rot90_block(v3d_g2d_num_qpus(), src_h, 4);
    if (nq == 0)
        return GPU_UNSUPPORTED;
    rows = src_h / nq;
    if (v3d_g2d_ver() == 42 && r90 && (src_w & 15) == 0) {
        tu[0] = src_phys;
        tu[1] = dst_phys;
        tu[2] = (uint32_t)((int64_t)rows * ss);
        tu[3] = (uint32_t)(((int64_t)rows - 1) * ss);
        tu[4] = (uint32_t)(-(int64_t)ss);
        tu[5] = (uint32_t)((int64_t)rows * ss + 64);
        tu[6] = (uint32_t)(-((int64_t)rows * 4));
        tu[7] = (uint32_t)(((int64_t)src_h - rows) * 4);
        tu[8] = (uint32_t)((int64_t)16 * ds - (int64_t)rows * 4);
        tu[9] = (uint32_t)(rows / 4);
        tu[10] = (uint32_t)((int64_t)rows * L / 4);
        tu[11] = v3d_g2d_scratch_phys();
        tu[12] = (uint32_t)nq;
        tu[13] = (uint32_t)ds;
        return v3d_g2d_run(g2d_qpu_v42_argb_rot90_tile,
                           g2d_qpu_v42_argb_rot90_tile_n, tu, 14,
                           nq, argb_src, (size_t)src_w * src_h * 4,
                           argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
               GPU_DONE : GPU_FAILED;
    }
    u[0] = src_phys;
    u[1] = dst_phys;
    u[2] = (uint32_t)((int64_t)rows * ss);
    u[3] = r90 ? (uint32_t)(((int64_t)rows - 1) * ss)
               : (uint32_t)((int64_t)(L - 1) * 64);
    u[4] = r90 ? (uint32_t)(-(int64_t)ss) : (uint32_t)ss;
    u[5] = (uint32_t)(r90 ? ((int64_t)rows * ss + 64)
                          : -((int64_t)rows * ss + 64));
    u[6] = (uint32_t)(r90 ? -((int64_t)rows * 4)
                          : (int64_t)rows * 4);
    u[7] = r90 ? (uint32_t)(((int64_t)src_h - rows) * 4)
               : (uint32_t)(((int64_t)src_w - 1 -
                              (int64_t)(L - 1) * 16) * ds);
    u[8] = (uint32_t)((int64_t)16 * ds -
                      ((int64_t)rows - 1) * 4 - 4);
    u[9] = (uint32_t)(rows - 1);
    u[10] = (uint32_t)((int64_t)rows * L / 4);
    u[11] = v3d_g2d_scratch_phys();
    u[12] = (uint32_t)nq;
    u[13] = (uint32_t)rows;
    u[14] = 16u;
    u[15] = (uint32_t)(r90 ? (int64_t)ds : -(int64_t)ds);
    u[16] = (uint32_t)(src_w - (L - 1) * 16); /* valid lanes in last group */
    u[17] = (uint32_t)(L - 1);
    return v3d_g2d_run(g2d_qpu_argb_rot90, g2d_qpu_argb_rot90_n, u, 18,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
           GPU_DONE : GPU_FAILED;
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
        /* looping copy kernel first; the gather path below pays a launch
         * per 12 spans plus the ARM-side per-lane address vectors */
        int gr = gpu_copy_loop_vc4(m, 1, src_phys, argb_src,
                                   src_w, src_h, dst_phys, argb_dst,
                                   dst_w, dst_h, 0, 0, dst_w, dst_h);
        if (gr != GPU_UNSUPPORTED)
            return gr;
        return gpu_affine_surface_vc4(m, 1, src_phys, argb_src,
                                      src_w, src_h, dst_phys, argb_dst,
                                      dst_w, dst_h, 0, 0, dst_w, dst_h);
    }
    return gpu_affine_surface(g2d_qpu_argb_rotate, g2d_qpu_argb_rotate_n, m,
                              src_phys, argb_src, src_w, src_h,
                              dst_phys, argb_dst, dst_w, dst_h,
                              0, 0, dst_w, dst_h, 0);
}

/* Alpha blend of a clipped dst rect.  V3D >= 4 uses the CSD kernel below;
 * VC4 uses the self-looping TMU blend kernel only. */

/* The only VC4 alpha path: the self-looping TMU blend kernel.  Same
 * shape and eligibility as gpu_copy_loop_vc4() - integer Q15 maps whose
 * region is entirely in-source - plus a dst TMU gather and the
 * source-over ALU blend inside the loop, so one launch blends maxq
 * whole rows instead of three launches per maxq 16-pixel spans (the
 * launch count is what made blit_alpha ~10x slower than blit_opaque).
 * There is deliberately no fallback: blending is not idempotent, so
 * anything the kernel cannot cover (fractional maps, widths that are
 * not 16-pixel multiples, out-of-source corners) fails up front -
 * before any destination row is touched - and reports -1. */
static int gpu_alpha_loop_vc4(const g2d_map_t *m, uint8_t alpha,
                              uint32_t src_phys, uint32_t *argb_src,
                              int32_t src_w, int32_t src_h,
                              uint32_t dst_phys, uint32_t *argb_dst,
                              int32_t dst_w, int32_t dst_h,
                              int32_t x0, int32_t y0,
                              int32_t x1, int32_t y1)
{
    uint32_t u[16 * VC4_UNIF_QWORDS] = { 0 };
    int32_t maxq = v3d_g2d_num_qpus();
    int32_t ipu, iqu, ipv, iqv, L, y;
    int64_t sb;

    if (!m || !argb_src || !argb_dst || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 || x1 <= x0 ||
        y1 <= y0 || x1 > dst_w || y1 > dst_h || alpha == 0 ||
        maxq <= 0 || maxq > 16)
        return GPU_FAILED;
    if ((m->pu | m->qu | m->pv | m->qv) & 32767)
        return GPU_FAILED;               /* integer maps only */
    if ((x1 - x0) & 15)
        return GPU_FAILED;               /* whole 16-pixel groups only */
    ipu = m->pu >> 15;
    iqu = m->qu >> 15;
    ipv = m->pv >> 15;
    iqv = m->qv >> 15;
    L = (x1 - x0) >> 4;
    if (L <= 0 || L > G2D_BAND_MAX_VDW)
        return GPU_FAILED;
    /* per-lane source byte step along a destination row */
    sb = ((int64_t)ipu + (int64_t)ipv * src_w) * 4;
    if (sb > (1 << 20) || sb < -(1 << 20))
        return GPU_FAILED;               /* mul24 lane-offset range */
    {
        /* all 4 corners of the full-group region must be in-source */
        int32_t xs[2], ys[2], i, j;

        xs[0] = x0; xs[1] = x1 - 1;
        ys[0] = y0; ys[1] = y1 - 1;
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 2; j++) {
                int64_t su = (int64_t)ipu * xs[i] + (int64_t)iqu * ys[j] +
                             m->cu;
                int64_t sv = (int64_t)ipv * xs[i] + (int64_t)iqv * ys[j] +
                             m->cv;

                if (su < 0 || sv < 0 || su >= src_w || sv >= src_h)
                    return GPU_FAILED;
            }
        }
    }
    /* evict the shared system L3 once before any row is read */
    if (v3d_g2d_vc4_prepare_reads() != 0)
        return GPU_FAILED;
    for (y = y0; y < y1; y += maxq) {
        int32_t nq = y1 - y, q, r;

        if (nq > maxq)
            nq = maxq;
        for (q = 0; q < nq; q++) {
            uint32_t *s = u + q * VC4_UNIF_QWORDS;
            int32_t Y = y + q;
            int64_t su = (int64_t)ipu * x0 + (int64_t)iqu * Y + m->cu;
            int64_t sv = (int64_t)ipv * x0 + (int64_t)iqv * Y + m->cv;
            uint32_t drow = dst_phys + ((uint32_t)Y * (uint32_t)dst_w +
                                        (uint32_t)x0) * 4u;

            s[0] = (src_phys + (uint32_t)((sv * src_w + su) * 4)) |
                   VC4_BUS_ALIAS;
            s[1] = drow | VC4_BUS_ALIAS_DIRECT;      /* VDW target */
            s[2] = (uint32_t)L;
            s[3] = sb >= 0 ? (uint32_t)sb : 0u;      /* lane step, add part */
            s[4] = sb >= 0 ? 0u : (uint32_t)(-sb);   /* lane step, sub part */
            s[5] = (uint32_t)(int32_t)(sb * 16);     /* per-group src step */
            s[6] = 0x80904080u;                      /* VDW setup base */
            s[7] = drow | VC4_BUS_ALIAS;             /* dst TMU gather */
            s[8] = alpha == 0xff ? 256u : (uint32_t)alpha;
        }
        r = gpu_vc4_run_once(g2d_qpu_argb_alpha_loop_vc4,
                             g2d_qpu_argb_alpha_loop_vc4_n, u, nq,
                             argb_src, (size_t)src_w * (size_t)src_h * 4u,
                             argb_dst, (size_t)dst_w * (size_t)dst_h * 4u);
        if (r != 0)
            return GPU_FAILED;
    }
    return GPU_DONE;
}

static int gpu_alpha_surface(const g2d_map_t *m, uint8_t alpha,
                             uint32_t src_phys, uint32_t *argb_src,
                             int32_t src_w, int32_t src_h,
                             uint32_t dst_phys, uint32_t *argb_dst,
                             int32_t dst_w, int32_t dst_h,
                             int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t u[28];
    int32_t g0 = x0 >> 4;
    int32_t g1 = (int32_t)(((uint32_t)x1 + 15u) >> 4);
    int32_t L = g1 - g0;
    int32_t band_h = y1 - y0;
    int nq = v3d_g2d_num_qpus();
    int32_t rows;
    int full = ((dst_w & 15) == 0 &&
                x0 == 0 && y0 == 0 && x1 == dst_w && y1 == dst_h);

    /* The kernel's dst-read stream is rect-gated (out-of-rect lanes read
     * their lane-private scratch word), so any dst width works - no
     * 16-pixel alignment requirement. */
    if (dst_w <= 0 || dst_h <= 0 || x0 < 0 || y0 < 0 ||
        x1 <= x0 || y1 <= y0 || x1 > dst_w || y1 > dst_h || alpha == 0)
        return 0;
    if (!gpu_map_fits(m, x1, y1))
        return 0;
    if (v3d_g2d_ver() == 21)
        /* The loop path validates the complete operation before launch;
         * alpha cannot fall back after a possibly successful blend. */
        return gpu_alpha_loop_vc4(m, alpha, src_phys, argb_src,
                                  src_w, src_h, dst_phys, argb_dst,
                                  dst_w, dst_h, x0, y0, x1, y1);
    if (nq > band_h)
        nq = band_h;
    rows = (band_h + nq - 1) / nq;
    u[0] = (uint32_t)m->pu;
    u[1] = (uint32_t)m->cu;
    u[2] = (uint32_t)m->qv;
    u[3] = (uint32_t)m->cv;
    u[4] = (uint32_t)(src_w - 1);
    u[5] = (uint32_t)(src_h - 1);
    u[6] = (uint32_t)src_w;
    u[7] = dst_phys + ((uint32_t)y0 * (uint32_t)dst_w +
                       (uint32_t)g0 * 16u) * 4u;
    u[8] = src_phys;
    u[9] = (uint32_t)(L - 1);
    u[10] = (uint32_t)(y1 - 1);
    /* Preserve source alpha exactly for the public alpha==255 case.  The
     * kernel computes effective alpha as (src_a * u11) >> 8. */
    u[11] = alpha == 0xff ? 256u : (uint32_t)alpha;
    u[12] = (uint32_t)(L * rows);
    u[13] = full ? (uint32_t)((int64_t)m->pu * 16)
                 : (uint32_t)x0;
    u[14] = full ? (uint32_t)((int64_t)m->pu * 16 * L)
                 : (uint32_t)x1;
    u[15] = (uint32_t)y0;
    u[16] = (uint32_t)y1;
    u[17] = 16u;
    u[18] = (uint32_t)(L * 64 - dst_w * 4);   /* jump: L*64 - dst stride */
    u[19] = 255u;
    u[20] = (uint32_t)rows;
    u[21] = (uint32_t)(rows * (int32_t)dst_w * 4); /* rows * stride (bytes) */
    u[22] = v3d_g2d_scratch_phys();                 /* out-of-rect write target */
    u[23] = (uint32_t)full;
    u[24] = 0x00ff00ffu;
    u[25] = 0x00010001u;
    u[26] = 0x00ff0000u;
    u[27] = (uint32_t)(full && src_w == dst_w && src_h == dst_h &&
                       m->pu == 32768 && m->cu == 0 &&
                       m->qv == 32768 && m->cv == 0);
    return v3d_g2d_run(g2d_qpu_argb_alpha, g2d_qpu_argb_alpha_n, u, 28,
                       nq, argb_src, (size_t)src_w * src_h * 4,
                       argb_dst, (size_t)dst_w * dst_h * 4) == 0 ?
           GPU_DONE : GPU_FAILED;
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
        if (v3d_g2d_ver() == 21) {
            gr = gpu_fill_loop_vc4(phys, argb, argb_w, argb_h, rx, ry,
                                   rx + rw, ry + rh, color);
            if (gr == GPU_UNSUPPORTED)
                gr = gpu_fill_surface_vc4(phys, argb, argb_w, argb_h,
                                          rx, ry, rx + rw, ry + rh,
                                          color);
        } else
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
            if (v3d_g2d_ver() != 21 && (rot == 90 || rot == 270)) {
                gr = gpu_rot90_surface(src_phys, argb_src, src_w, src_h,
                                       dst_phys, argb_dst, dst_w, dst_h, rot);
                if (gr == GPU_DONE)
                    return 0;
                if (gr == GPU_FAILED)
                    return -1;
            }
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
