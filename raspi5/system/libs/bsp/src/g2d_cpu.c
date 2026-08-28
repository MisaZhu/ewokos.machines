/*
 * g2d_cpu.c - software (CPU) rasterizer for the bsp_g2d ARGB8888 API.
 *
 * The blit/scale/rotate mapping semantics come from ../app/src/g2d.c
 * (identical Q15 coefficients and clamping, so CPU and GPU outputs agree
 * pixel-for-pixel) and the rotate / rotated_size semantics from the
 * EwokOS aarch64 arch_g2d.c (14-bit fixed-point trig, clockwise rotation,
 * bounding box with transparent outside pixels).
 */

#include "g2d_cpu.h"

/* ------------------------------------------------------------------ */
/* affine map coefficients (shared with the GPU path)                  */
/* ------------------------------------------------------------------ */

void g2d_cpu_map_params(int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    int rotate, g2d_cpu_map_t *m)
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
         * the dst X axis maps to the src Y axis (scale sh/dw) and the dst
         * Y axis maps to the src X axis reversed (scale sw/dh). */
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

/* Sample source pixel for destination (X, Y); out-of-range samples clamp
 * to the source edge (app g2d.c behaviour). */
static inline uint32_t g2d_cpu_sample(const uint32_t *src, int32_t src_w,
                                      int32_t src_h, const g2d_cpu_map_t *m,
                                      int32_t X, int32_t Y)
{
    int64_t u = ((int64_t)m->pu * X + (int64_t)m->qu * Y) >> 15;
    int64_t v = ((int64_t)m->pv * X + (int64_t)m->qv * Y) >> 15;
    int32_t ui = (int32_t)(u + m->cu);
    int32_t vi = (int32_t)(v + m->cv);

    if (ui < 0)
        ui = 0;
    if (vi < 0)
        vi = 0;
    if (ui >= src_w)
        ui = src_w - 1;
    if (vi >= src_h)
        vi = src_h - 1;
    return src[(uint32_t)vi * (uint32_t)src_w + (uint32_t)ui];
}

/* ------------------------------------------------------------------ */
/* fill                                                               */
/* ------------------------------------------------------------------ */

void g2d_cpu_fill(uint32_t *argb, int32_t argb_w, int32_t argb_h,
                  int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color)
{
    int32_t yy;

    if (argb == NULL || argb_w <= 0 || argb_h <= 0 || w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > argb_w)
        w = argb_w - x;
    if (y + h > argb_h)
        h = argb_h - y;
    if (w <= 0 || h <= 0)
        return;
    for (yy = y; yy < y + h; yy++) {
        uint32_t *row = argb + (uint32_t)yy * (uint32_t)argb_w + (uint32_t)x;
        int32_t xx;
        for (xx = 0; xx < w; xx++)
            row[xx] = color;
    }
}

/* ------------------------------------------------------------------ */
/* blit / blit-alpha (Q15 map over the destination rect)               */
/* ------------------------------------------------------------------ */

static void g2d_cpu_blit_map(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                             const g2d_cpu_map_t *m,
                             uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                             int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                             uint8_t alpha, int do_alpha)
{
    int32_t Y, X;

    if (argb_src == NULL || argb_dst == NULL || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || dw <= 0 || dh <= 0)
        return;
    for (Y = dy; Y < dy + dh; Y++) {
        uint32_t *drow;
        if (Y < 0 || Y >= dst_h)
            continue;
        drow = argb_dst + (uint32_t)Y * (uint32_t)dst_w;
        for (X = dx; X < dx + dw; X++) {
            uint32_t c, d;
            uint8_t sa;
            if (X < 0 || X >= dst_w)
                continue;
            c = g2d_cpu_sample(argb_src, src_w, src_h, m, X, Y);
            if (!do_alpha) {
                drow[X] = c;
                continue;
            }
            sa = (uint8_t)((((c >> 24) & 0xff) * alpha) >> 8);
            if (sa == 0)
                continue;
            if (sa == 255) {
                drow[X] = c;
                continue;
            }
            d = drow[X];
            {
                uint8_t da = (uint8_t)(d >> 24), dr = (uint8_t)(d >> 16),
                        dg = (uint8_t)(d >> 8),  db = (uint8_t)d;
                uint8_t sr = (uint8_t)(c >> 16), sg = (uint8_t)(c >> 8),
                        sb = (uint8_t)c;
                uint8_t oa = (uint8_t)(da + ((255 - da) * sa) / 255);
                uint8_t orr = (uint8_t)((sr * sa + dr * (255 - sa)) / 255);
                uint8_t og = (uint8_t)((sg * sa + dg * (255 - sa)) / 255);
                uint8_t ob = (uint8_t)((sb * sa + db * (255 - sa)) / 255);
                drow[X] = ((uint32_t)oa << 24) | (orr << 16) | (og << 8) | ob;
            }
        }
    }
}

void g2d_cpu_blt(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                 int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                 uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                 int32_t dx, int32_t dy, int32_t dw, int32_t dh)
{
    g2d_cpu_map_t m;

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    g2d_cpu_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
    g2d_cpu_blit_map(argb_src, src_w, src_h, &m,
                     argb_dst, dst_w, dst_h, dx, dy, dw, dh, 0xff, 0);
}

void g2d_cpu_blt_alpha(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                       int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                       uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                       int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                       uint8_t alpha)
{
    g2d_cpu_map_t m;

    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || alpha == 0)
        return;
    g2d_cpu_map_params(sx, sy, sw, sh, dx, dy, dw, dh, G2D_MAP_ROT_0, &m);
    g2d_cpu_blit_map(argb_src, src_w, src_h, &m,
                     argb_dst, dst_w, dst_h, dx, dy, dw, dh, alpha, 1);
}

/* ------------------------------------------------------------------ */
/* scale_to: whole-surface nearest-neighbour through the Q15 map       */
/* ------------------------------------------------------------------ */

void g2d_cpu_scale_to(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                      uint32_t *argb_dst, int32_t dst_w, int32_t dst_h)
{
    g2d_cpu_map_t m;

    if (argb_src == NULL || argb_dst == NULL || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0)
        return;
    g2d_cpu_map_params(0, 0, src_w, src_h, 0, 0, dst_w, dst_h,
                   G2D_MAP_ROT_0, &m);
    g2d_cpu_blit_map(argb_src, src_w, src_h, &m,
                     argb_dst, dst_w, dst_h, 0, 0, dst_w, dst_h, 0xff, 0);
}

/* ------------------------------------------------------------------ */
/* rotate / rotated_size                                               */
/* ------------------------------------------------------------------ */

/* 14-bit fixed point trig (table[i] = round(sin(i deg) * 16384)),
 * identical to the EwokOS arch_g2d table. */
#define G2D_FP_BITS 14
#define G2D_FP_ONE  (1 << G2D_FP_BITS)
#define G2D_FP_HALF (1 << (G2D_FP_BITS - 1))

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

void g2d_cpu_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
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

void g2d_cpu_rotate(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                    uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                    int32_t degree)
{
    int32_t bw, bh, c, s, scx, scy, dcx, dcy;
    int32_t x, y;

    if (argb_src == NULL || argb_dst == NULL ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    degree = g2d_norm_degree(degree);
    if (degree == 0) {
        if (dst_w < src_w || dst_h < src_h)
            return;
        if (argb_src != argb_dst) {
            uint32_t n = (uint32_t)src_w * (uint32_t)src_h;
            uint32_t i;
            if (dst_w != src_w || dst_h != src_h)
                g2d_cpu_fill(argb_dst, dst_w, dst_h, 0, 0, dst_w, dst_h, 0);
            for (i = 0; i < n; i++)
                argb_dst[i] = argb_src[i];
        }
        return;
    }

    if (degree == 180) {
        if (dst_w < src_w || dst_h < src_h)
            return;
        if (argb_src == argb_dst) {
            /* in-place reversal */
            uint32_t *lo = argb_src;
            uint32_t *hi = argb_src + (size_t)src_w * src_h - 1;
            while (lo < hi) {
                uint32_t t = *lo;
                *lo = *hi;
                *hi = t;
                lo++;
                hi--;
            }
            return;
        }
        if (dst_w != src_w || dst_h != src_h)
            g2d_cpu_fill(argb_dst, dst_w, dst_h, 0, 0, dst_w, dst_h, 0);
        for (y = 0; y < src_h; y++) {
            uint32_t *drow = argb_dst + (uint32_t)y * (uint32_t)dst_w;
            const uint32_t *srow =
                argb_src + (uint32_t)(src_h - 1 - y) * (uint32_t)src_w;
            for (x = 0; x < src_w; x++)
                drow[x] = srow[src_w - 1 - x];
        }
        return;
    }

    if (degree == 90 || degree == 270) {
        /* 90/270 swap the surface dimensions; in-place is not supported */
        if (argb_src == argb_dst || dst_w < src_h || dst_h < src_w)
            return;
        if (dst_w != src_h || dst_h != src_w)
            g2d_cpu_fill(argb_dst, dst_w, dst_h, 0, 0, dst_w, dst_h, 0);
        if (degree == 90) {
            /* dst[y][x] = src[src_h-1-x][y] */
            for (y = 0; y < src_w; y++) {
                uint32_t *drow = argb_dst + (uint32_t)y * (uint32_t)dst_w;
                for (x = 0; x < src_h; x++)
                    drow[x] =
                        argb_src[(uint32_t)(src_h - 1 - x) * (uint32_t)src_w +
                                 (uint32_t)y];
            }
        } else {
            /* dst[y][x] = src[x][src_w-1-y] */
            for (y = 0; y < src_w; y++) {
                uint32_t *drow = argb_dst + (uint32_t)y * (uint32_t)dst_w;
                for (x = 0; x < src_h; x++)
                    drow[x] = argb_src[(uint32_t)x * (uint32_t)src_w +
                                       (uint32_t)(src_w - 1 - y)];
            }
        }
        return;
    }

    /* arbitrary angle: inverse-mapped nearest neighbour, rotation around
     * the centres, content written into the top-left bw x bh box. */
    if (argb_src == argb_dst)
        return;
    g2d_cpu_rotated_size(src_w, src_h, degree, &bw, &bh);
    if (bw <= 0 || bh <= 0 || dst_w < bw || dst_h < bh)
        return;

    g2d_cpu_fill(argb_dst, dst_w, dst_h, 0, 0, dst_w, dst_h, 0);

    c = g2d_cos_fp(degree);
    s = g2d_sin_fp(degree);
    scx = (src_w - 1) << (G2D_FP_BITS - 1);
    scy = (src_h - 1) << (G2D_FP_BITS - 1);
    dcx = (bw - 1) << (G2D_FP_BITS - 1);
    dcy = (bh - 1) << (G2D_FP_BITS - 1);

    for (y = 0; y < bh; y++) {
        uint32_t *drow = argb_dst + (uint32_t)y * (uint32_t)dst_w;
        int64_t dy = (int64_t)y * G2D_FP_ONE - dcy;
        for (x = 0; x < bw; x++) {
            int64_t dx = (int64_t)x * G2D_FP_ONE - dcx;
            /* inverse of clockwise rotation: src = R(-degree) * dst */
            int64_t sxf = ((dx * c + dy * s) >> G2D_FP_BITS) + scx;
            int64_t syf = ((dy * c - dx * s) >> G2D_FP_BITS) + scy;
            int32_t sx = (int32_t)((sxf + G2D_FP_HALF) >> G2D_FP_BITS);
            int32_t sy = (int32_t)((syf + G2D_FP_HALF) >> G2D_FP_BITS);
            if (sx >= 0 && sx < src_w && sy >= 0 && sy < src_h)
                drow[x] = argb_src[(uint32_t)sy * (uint32_t)src_w +
                                   (uint32_t)sx];
        }
    }
}
