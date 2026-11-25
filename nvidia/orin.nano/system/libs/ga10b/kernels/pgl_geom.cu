/*
 * pgl_geom.cu - portablegl geometry kernels for GA10B (sm_87).
 *
 * CUDA ports of the fixed compute kernels in
 * projects/opengl/libs/gles/pglcompiler/include/geom_raster_cs.h; statement
 * order follows the GLSL so results match the other back ends.  Single
 * source: nvcc -arch=sm_87 -cubin for the GPU, or plain C with PGL_SIM
 * defined (pgl_sim.h) for the CPU reference.
 *
 * GLSL local sizes become 1-D blocks: xform 8x8 -> 64 threads, raster
 * 16x16 -> 256 threads, with the same linear lane numbering.  Uniforms
 * arrive as one u32/float word array (offsets below; the Orin pglc back end
 * publishes the same table by name).
 */
#ifdef PGL_SIM
#include "pgl_sim.h"
#else
#define PGL_KERNEL extern "C" __global__
#endif

/* uniform word offsets */
#define XF_MVP          0   /* mat4, column major */
#define XF_VIEWPORT_WH  16  /* vec2 */
#define XF_VERT_COUNT   18
#define XF_COL_ALPHA    19

#define RS_VIEWPORT_WH  0   /* vec2 */
#define RS_FB_STRIDE    2   /* pixels per row */
#define RS_TRI_FIRST    3
#define RS_TRI_COUNT    4
#define RS_BLEND_MODE   5
#define RS_DEPTH_WRITE  6
#define RS_CULL_SIGN    7

static __device__ float pgl_uf(const unsigned *u, int i)
{
    return ((const float *)u)[i];
}

static __device__ float pgl_clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* dst[i] = value for i < count (frame / depth clears) */
PGL_KERNEL void pgl_fill(unsigned *dst, unsigned value, unsigned count)
{
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count)
        dst[i] = value;
}

/* geom_xform_color_cs_glsl: one thread per vertex, precomputed colour */
PGL_KERNEL void pgl_xform_color(float *vout, const float *vpos,
                                const float *vcol, const unsigned *u)
{
    int v = (int)(blockIdx.x * 64u + threadIdx.x);
    if (v >= (int)u[XF_VERT_COUNT])
        return;

    float px = vpos[v * 3 + 0], py = vpos[v * 3 + 1], pz = vpos[v * 3 + 2];
    const float *m = (const float *)u + XF_MVP;
    float cx = m[0] * px + m[4] * py + m[8]  * pz + m[12];
    float cy = m[1] * px + m[5] * py + m[9]  * pz + m[13];
    float cz = m[2] * px + m[6] * py + m[10] * pz + m[14];
    float cw = m[3] * px + m[7] * py + m[11] * pz + m[15];

    float Wx = pgl_uf(u, XF_VIEWPORT_WH), Wy = pgl_uf(u, XF_VIEWPORT_WH + 1);
    float iw = (cw > 0.0f) ? (1.0f / cw) : 0.0f;   /* clip sentinel */
    float sx = (cx * iw * 0.5f + 0.5f) * Wx;
    float sy = (0.5f - cy * iw * 0.5f) * Wy;
    float z = cz * iw * 0.5f + 0.5f;
    int cs = (u[XF_COL_ALPHA] != 0u) ? 4 : 3;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (cw > 0.0f) {
        r = vcol[v * cs + 0];
        g = vcol[v * cs + 1];
        b = vcol[v * cs + 2];
    }

    vout[v * 8 + 0] = sx;
    vout[v * 8 + 1] = sy;
    vout[v * 8 + 2] = z;
    vout[v * 8 + 3] = iw;
    vout[v * 8 + 4] = r;
    vout[v * 8 + 5] = g;
    vout[v * 8 + 6] = b;
    vout[v * 8 + 7] = (u[XF_COL_ALPHA] != 0u) ? vcol[v * 4 + 3] : 1.0f;
}

/* shared tail of every vertex transform: clip -> screen, z, 1/w */
static __device__ float pgl_project(const float *m, float px, float py,
                                    float pz, const unsigned *u, int wh,
                                    float *o)
{
    float cx = m[0] * px + m[4] * py + m[8]  * pz + m[12];
    float cy = m[1] * px + m[5] * py + m[9]  * pz + m[13];
    float cz = m[2] * px + m[6] * py + m[10] * pz + m[14];
    float cw = m[3] * px + m[7] * py + m[11] * pz + m[15];
    float Wx = pgl_uf(u, wh), Wy = pgl_uf(u, wh + 1);
    float iw = (cw > 0.0f) ? (1.0f / cw) : 0.0f;   /* clip sentinel */
    o[0] = (cx * iw * 0.5f + 0.5f) * Wx;
    o[1] = (0.5f - cy * iw * 0.5f) * Wy;
    o[2] = cz * iw * 0.5f + 0.5f;
    o[3] = iw;
    return cw;
}

/* (m * vec4(n, 0)).xyz, normalized */
static __device__ void pgl_xnormal(const float *m, const float *n, float *o)
{
    float x = m[0] * n[0] + m[4] * n[1] + m[8]  * n[2];
    float y = m[1] * n[0] + m[5] * n[1] + m[9]  * n[2];
    float z = m[2] * n[0] + m[6] * n[1] + m[10] * n[2];
    float inv = 1.0f / sqrtf(x * x + y * y + z * z);
    o[0] = x * inv; o[1] = y * inv; o[2] = z * inv;
}

static __device__ void pgl_normalize(float *v)
{
    float inv = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
}

/* geom_xform_cs_glsl: gouraud lighting in the vertex pass */
#define XL_MVP          0
#define XL_MODELVIEW    16
#define XL_NORMALMAT    32
#define XL_MATERIAL     48
#define XL_LIGHT_EYE    52
#define XL_SHADE        56
#define XL_VIEWPORT_WH  60
#define XL_VERT_COUNT   62

PGL_KERNEL void pgl_xform_lit(float *vout, const float *vpos,
                              const float *vnrm, const unsigned *u)
{
    int v = (int)(blockIdx.x * 64u + threadIdx.x);
    if (v >= (int)u[XL_VERT_COUNT])
        return;
    const float *f = (const float *)u;
    const float *p = vpos + v * 3;
    float *o = vout + v * 8;
    float cw = pgl_project(f + XL_MVP, p[0], p[1], p[2], u, XL_VIEWPORT_WH, o);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (cw > 0.0f) {
        const float *mat = f + XL_MATERIAL;
        const float *le = f + XL_LIGHT_EYE;
        const float *sh = f + XL_SHADE;
        float ne[3], L[3], spec = 0.0f;
        pgl_xnormal(f + XL_NORMALMAT, vnrm + v * 3, ne);
        if (le[3] > 0.0f) {
            const float *mv = f + XL_MODELVIEW;
            float pe[3], V[3], H[3];
            pe[0] = mv[0] * p[0] + mv[4] * p[1] + mv[8]  * p[2] + mv[12];
            pe[1] = mv[1] * p[0] + mv[5] * p[1] + mv[9]  * p[2] + mv[13];
            pe[2] = mv[2] * p[0] + mv[6] * p[1] + mv[10] * p[2] + mv[14];
            V[0] = -pe[0]; V[1] = -pe[1]; V[2] = -pe[2];
            pgl_normalize(V);
            if (ne[0] * V[0] + ne[1] * V[1] + ne[2] * V[2] < 0.0f) {
                ne[0] = -ne[0]; ne[1] = -ne[1]; ne[2] = -ne[2];
            }
            L[0] = le[0] - pe[0]; L[1] = le[1] - pe[1]; L[2] = le[2] - pe[2];
            pgl_normalize(L);
            H[0] = L[0] + V[0]; H[1] = L[1] + V[1]; H[2] = L[2] + V[2];
            pgl_normalize(H);
            float s1 = fmaxf(ne[0] * H[0] + ne[1] * H[1] + ne[2] * H[2], 0.0f);
            float s2 = s1 * s1, s4 = s2 * s2, s8 = s4 * s4, s16 = s8 * s8;
            spec = s16 * (s16 * s16);
        } else {
            L[0] = le[0]; L[1] = le[1]; L[2] = le[2];
            pgl_normalize(L);
        }
        float diff = fmaxf(ne[0] * L[0] + ne[1] * L[1] + ne[2] * L[2], 0.0f);
        float k = sh[0] + sh[1] * diff, sp = sh[2] * spec;
        r = pgl_clamp01(mat[0] * k + sp);
        g = pgl_clamp01(mat[1] * k + sp);
        b = pgl_clamp01(mat[2] * k + sp);
    }
    o[4] = r; o[5] = g; o[6] = b;
    o[7] = pgl_uf(u, XL_MATERIAL + 3);
}

/* geom_xform_tex_cs_glsl: interleaved pos.xyz + uv (stride 6) */
PGL_KERNEL void pgl_xform_tex(float *vout, const float *vattr,
                              const unsigned *u)
{
    int v = (int)(blockIdx.x * 64u + threadIdx.x);
    if (v >= (int)u[XF_VERT_COUNT])
        return;
    const float *a = vattr + v * 6;
    float *o = vout + v * 8;
    pgl_project((const float *)u + XF_MVP, a[0], a[1], a[2], u,
                XF_VIEWPORT_WH, o);
    o[4] = a[3]; o[5] = a[4]; o[6] = 0.0f; o[7] = 0.0f;
}

/* geom_xform_texcol_cs_glsl: pos.xyz + uv + rgba (stride 10), vout stride 12 */
PGL_KERNEL void pgl_xform_texcol(float *vout, const float *vattr,
                                 const unsigned *u)
{
    int v = (int)(blockIdx.x * 64u + threadIdx.x);
    if (v >= (int)u[XF_VERT_COUNT])
        return;
    const float *a = vattr + v * 10;
    float *o = vout + v * 12;
    pgl_project((const float *)u + XF_MVP, a[0], a[1], a[2], u,
                XF_VIEWPORT_WH, o);
    o[4] = a[3]; o[5] = a[4];
    o[6] = a[5]; o[7] = a[6]; o[8] = a[7]; o[9] = a[8];
    o[10] = 0.0f; o[11] = 0.0f;
}

/* geom_xform_inst_cs_glsl / geom_xform_instlit_cs_glsl: per-vertex instance
 * index in pos.w, per-instance record (mvp, normal matrix, material ...) of
 * `rec` floats; lit != 0 adds the reflect specular, emissive and alpha */
#define XI_LIGHT_EYE    0
#define XI_SHADE        4
#define XI_VIEWPORT_WH  8
#define XI_VERT_COUNT   10

static __device__ void pgl_xform_instanced(float *vout, const float *vpos,
                                           const float *vnrm,
                                           const float *inst,
                                           const unsigned *u, int rec, int lit)
{
    int v = (int)(blockIdx.x * 64u + threadIdx.x);
    if (v >= (int)u[XI_VERT_COUNT])
        return;
    const float *f = (const float *)u;
    const float *p = vpos + v * 4;
    const float *g = inst + (int)p[3] * rec;
    float *o = vout + v * 8;
    float ne[3], L[3];
    pgl_xnormal(g + 16, vnrm + v * 3, ne);
    L[0] = f[XI_LIGHT_EYE]; L[1] = f[XI_LIGHT_EYE + 1];
    L[2] = f[XI_LIGHT_EYE + 2];
    pgl_normalize(L);
    float ndl = ne[0] * L[0] + ne[1] * L[1] + ne[2] * L[2];
    float diff = fmaxf(ndl, 0.0f);
    float k = f[XI_SHADE] + f[XI_SHADE + 1] * diff, sp = 0.0f;
    if (lit) {
        float rd = fmaxf(2.0f * ndl * ne[2] - L[2], 0.0f);   /* reflect(-L,n).z */
        float rd2 = rd * rd, rd4 = rd2 * rd2, rd8 = rd4 * rd4;
        sp = rd8 * rd8 * f[XI_SHADE + 2];
        k += g[35];                                          /* emissive */
    }
    float cw = pgl_project(g, p[0], p[1], p[2], u, XI_VIEWPORT_WH, o);
    float r = 0.0f, gg = 0.0f, b = 0.0f;
    if (cw > 0.0f) {
        r = pgl_clamp01(g[32] * k + sp);
        gg = pgl_clamp01(g[33] * k + sp);
        b = pgl_clamp01(g[34] * k + sp);
    }
    o[4] = r; o[5] = gg; o[6] = b;
    o[7] = lit ? g[36] : 0.0f;
}

PGL_KERNEL void pgl_xform_inst(float *vout, const float *vpos,
                               const float *vnrm, const float *inst,
                               const unsigned *u)
{
    pgl_xform_instanced(vout, vpos, vnrm, inst, u, 36, 0);
}

PGL_KERNEL void pgl_xform_instlit(float *vout, const float *vpos,
                                  const float *vnrm, const float *inst,
                                  const unsigned *u)
{
    pgl_xform_instanced(vout, vpos, vnrm, inst, u, 40, 1);
}

static __device__ unsigned pgl_pack(float r, float g, float b)
{
    unsigned ur = (unsigned)(pgl_clamp01(r) * 255.0f + 0.5f);
    unsigned ug = (unsigned)(pgl_clamp01(g) * 255.0f + 0.5f);
    unsigned ub = (unsigned)(pgl_clamp01(b) * 255.0f + 0.5f);
    return (255u << 24) | (ur << 16) | (ug << 8) | ub;
}

/* geom_raster_cs_glsl: one thread per triangle, bounding-box sweep */
PGL_KERNEL void pgl_raster(unsigned *fb, const float *vout, unsigned *depth,
                           const unsigned *u)
{
    int tri_first = (int)u[RS_TRI_FIRST];
    int tri = (int)(blockIdx.x * 256u + threadIdx.x) + tri_first;
    if (tri >= tri_first + (int)u[RS_TRI_COUNT])
        return;

    const float *v0 = vout + (tri * 3 + 0) * 8;
    const float *v1 = vout + (tri * 3 + 1) * 8;
    const float *v2 = vout + (tri * 3 + 2) * 8;

    float iw0 = v0[3], iw1 = v1[3], iw2 = v2[3];
    if (iw0 <= 0.0f || iw1 <= 0.0f || iw2 <= 0.0f)
        return;
    unsigned cull = u[RS_CULL_SIGN];
    if (cull == 3u)
        return;   /* GL_FRONT_AND_BACK */

    float s0x = v0[0], s0y = v0[1];
    float s1x = v1[0], s1y = v1[1];
    float s2x = v2[0], s2y = v2[1];
    float z0 = v0[2], z1 = v1[2], z2 = v2[2];

    /* edgef(s1, s2, s0) */
    float area = (s0x - s1x) * (s2y - s1y) - (s0y - s1y) * (s2x - s1x);
    if (area == 0.0f)
        return;
    if (cull == 1u && area >= 0.0f)
        return;
    if (cull == 2u && area <= 0.0f)
        return;
    float inv_area = 1.0f / area;

    float Wx = pgl_uf(u, RS_VIEWPORT_WH), Wy = pgl_uf(u, RS_VIEWPORT_WH + 1);
    int minx = (int)floorf(fminf(s0x, fminf(s1x, s2x)));
    int maxx = (int)ceilf(fmaxf(s0x, fmaxf(s1x, s2x)));
    int miny = (int)floorf(fminf(s0y, fminf(s1y, s2y)));
    int maxy = (int)ceilf(fmaxf(s0y, fmaxf(s1y, s2y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > (int)Wx - 1) maxx = (int)Wx - 1;
    if (maxy > (int)Wy - 1) maxy = (int)Wy - 1;
    if (minx > maxx || miny > maxy)
        return;

    float A0 = s2y - s1y, B0 = s2x - s1x, C0 = B0 * s1y - A0 * s1x;
    float A1 = s0y - s2y, B1 = s0x - s2x, C1 = B1 * s2y - A1 * s2x;
    float A2 = s1y - s0y, B2 = s1x - s0x, C2 = B2 * s0y - A2 * s0x;
    float sgn = (area < 0.0f) ? -1.0f : 1.0f;
    float cx = (float)minx + 0.5f;

    int stride = (int)u[RS_FB_STRIDE];
    unsigned blend = u[RS_BLEND_MODE];
    unsigned depth_write = u[RS_DEPTH_WRITE];

    for (int py = miny; py <= maxy; py++) {
        float fy = (float)py + 0.5f;
        float w0 = A0 * cx - B0 * fy + C0;
        float w1 = A1 * cx - B1 * fy + C1;
        float w2 = A2 * cx - B2 * fy + C2;
        for (int px = minx; px <= maxx; px++) {
            float e0 = w0, e1 = w1, e2 = w2;
            w0 += A0; w1 += A1; w2 += A2;   /* step before any continue */
            if (e0 * sgn < 0.0f || e1 * sgn < 0.0f || e2 * sgn < 0.0f)
                continue;
            float b0 = e0 * inv_area, b1 = e1 * inv_area, b2 = e2 * inv_area;

            float iw = b0 * iw0 + b1 * iw1 + b2 * iw2;
            if (iw <= 0.0f)
                continue;
            float rw = 1.0f / iw;
            float z = (b0 * z0 * iw0 + b1 * z1 * iw1 + b2 * z2 * iw2) * rw;

            unsigned d = (unsigned)(pgl_clamp01(z) * 16777215.0f);
            int idx = py * stride + px;
            if (depth_write != 0u) {
                if (d >= depth[idx])
                    continue;
                unsigned old = atomicMin(&depth[idx], d);
                if (d >= old)
                    continue;
            } else {
                if (d >= depth[idx])
                    continue;
            }

            float k0 = b0 * iw0 * rw, k1 = b1 * iw1 * rw, k2 = b2 * iw2 * rw;
            float r = k0 * v0[4] + k1 * v1[4] + k2 * v2[4];
            float g = k0 * v0[5] + k1 * v1[5] + k2 * v2[5];
            float b = k0 * v0[6] + k1 * v1[6] + k2 * v2[6];
            if (blend == 0u) {
                fb[idx] = pgl_pack(r, g, b);
            } else {
                float sa = pgl_clamp01(k0 * v0[7] + k1 * v1[7] + k2 * v2[7]);
                float sr = pgl_clamp01(r), sg = pgl_clamp01(g);
                float sb = pgl_clamp01(b);
                unsigned dw = fb[idx];
                for (;;) {
                    float dr = (float)((dw >> 16) & 255u) / 255.0f;
                    float dg = (float)((dw >> 8) & 255u) / 255.0f;
                    float db = (float)(dw & 255u) / 255.0f;
                    float orr, og, ob;
                    if (blend == 2u) {
                        orr = dr + sr * sa;
                        og = dg + sg * sa;
                        ob = db + sb * sa;
                    } else {
                        orr = sr * sa + dr * (1.0f - sa);
                        og = sg * sa + dg * (1.0f - sa);
                        ob = sb * sa + db * (1.0f - sa);
                    }
                    unsigned nw = pgl_pack(orr, og, ob);
                    unsigned old = atomicCAS(&fb[idx], dw, nw);
                    if (old == dw)
                        break;
                    dw = old;
                }
            }
        }
    }
}

/* ---- tile rasterizer (the path the daemon runs) -------------------------
 * One GPU thread per triangle sweeping its bounding box (pgl_raster above,
 * the shape of the GLSL kernels) is serial per triangle: a screen-filling
 * triangle keeps a single thread busy for tens of milliseconds.  The tile
 * rasterizer inverts the ownership, like the Adreno back end's pixel-owned
 * kernels: one thread per PIXEL, 16x16 pixels per block, each tile walking
 * the list of triangles binned to it.
 *
 *   pgl_tri_setup  one thread per triangle: cull, edge equations
 *                  pre-divided by the area, attributes pre-multiplied by
 *                  1/w, bounding box (clipped to the scissor); append the
 *                  triangle to every tile its box touches.
 *   pgl_bin_sort   one thread per tile: sort the tile's list by triangle
 *                  index, so pixels composite in submission order.
 *   pgl_raster_tile one thread per pixel: walk the tile's list in order.
 *                  A pixel has exactly one owner, so depth test, depth
 *                  write and blending are plain sequential code - the
 *                  result equals drawing the triangles one after another
 *                  (no atomics, no cross-triangle races).
 *
 * One kernel set serves the three raster flavours of geom_raster_cs.h; the
 * daemon selects the flavour and its state through the parameter block:
 *   BP_MODE_COLOR   geom_raster_cs_glsl: vertex colour, depth test LESS,
 *                   optional depth write / cull / blend
 *   BP_MODE_TEX     geom_raster_tex_cs_glsl: nearest texel (V flipped)
 *                   written verbatim, depth test + write
 *   BP_MODE_TEXCOL  geom_raster_texcol_cs_glsl: bilinear texel * vertex
 *                   colour, separate-alpha OVER blend, no depth, scissor
 *
 * A tile whose list overflowed its capacity scans every triangle of the
 * draw instead (correct, slower).  Rejected triangles get an empty box.
 * With BP_FLAG_CLEAR every pixel starts from the clear colour / far depth
 * and is written unconditionally: the frame clear costs no extra launch. */
#define PGL_TILE        16
#define TR_WORDS        40      /* floats per triangle record */
#define TR_EDGE         0       /* 3 x (a, b, c): bary_i = a*x + b*y + c */
#define TR_IW           9
#define TR_ZW           12
#define TR_ATTR         15      /* 6 attributes x 3 vertices, times 1/w */
#define TR_BOX          36      /* int minx, miny, maxx, maxy */
#define TR_MAX_ATTR     6

#define BP_TILES_X      0
#define BP_TILES_Y      1
#define BP_CAP          2       /* list entries per tile */
#define BP_FLAGS        3
#define BP_CLEAR_COLOR  4
#define BP_CLEAR_DEPTH  5
#define BP_MODE         6
#define BP_VSTRIDE      7       /* floats per vout record (8 or 12) */
#define BP_NATTR        8       /* attributes after vout[3] */
#define BP_CULL         9       /* uCullSign */
#define BP_BLEND        10      /* uBlendMode */
#define BP_DEPTH_WRITE  11
#define BP_TEX_W        12
#define BP_TEX_H        13
#define BP_SCISSOR      14      /* x, y, w, h; w or h <= 0 = off */
#define BP_WORDS        18
#define BP_FLAG_CLEAR   1u
/* the tile lists are in arrival order (pgl_bin_sort was skipped).  Only set
 * for depth-tested, depth-writing, unblended draws: there the surviving
 * fragment is the nearest one whatever the order, and an equal-depth tie
 * goes to the lower triangle index - what in-order compositing yields. */
#define BP_FLAG_UNORDERED 2u
#define BP_MODE_COLOR   0u
#define BP_MODE_TEX     1u
#define BP_MODE_TEXCOL  2u

PGL_KERNEL void pgl_tri_setup(float *trec, unsigned *bin_count,
                              unsigned *bin_list, const float *vout,
                              const unsigned *u, const unsigned *bp)
{
    int t = (int)(blockIdx.x * 256u + threadIdx.x);
    if (t >= (int)u[RS_TRI_COUNT])
        return;
    int tri = t + (int)u[RS_TRI_FIRST];
    int vs = (int)bp[BP_VSTRIDE];
    float *r = trec + t * TR_WORDS;
    int *box = (int *)(r + TR_BOX);
    box[0] = 1; box[1] = 1; box[2] = 0; box[3] = 0;   /* empty: rejected */

    const float *v0 = vout + (tri * 3 + 0) * vs;
    const float *v1 = vout + (tri * 3 + 1) * vs;
    const float *v2 = vout + (tri * 3 + 2) * vs;
    float iw0 = v0[3], iw1 = v1[3], iw2 = v2[3];
    if (iw0 <= 0.0f || iw1 <= 0.0f || iw2 <= 0.0f)
        return;
    unsigned cull = bp[BP_CULL];
    if (cull == 3u)
        return;

    float s0x = v0[0], s0y = v0[1];
    float s1x = v1[0], s1y = v1[1];
    float s2x = v2[0], s2y = v2[1];
    float area = (s0x - s1x) * (s2y - s1y) - (s0y - s1y) * (s2x - s1x);
    if (area == 0.0f)
        return;
    if (cull == 1u && area >= 0.0f)
        return;
    if (cull == 2u && area <= 0.0f)
        return;
    float inv_area = 1.0f / area;

    float Wx = pgl_uf(u, RS_VIEWPORT_WH), Wy = pgl_uf(u, RS_VIEWPORT_WH + 1);
    int minx = (int)floorf(fminf(s0x, fminf(s1x, s2x)));
    int maxx = (int)ceilf(fmaxf(s0x, fmaxf(s1x, s2x)));
    int miny = (int)floorf(fminf(s0y, fminf(s1y, s2y)));
    int maxy = (int)ceilf(fmaxf(s0y, fmaxf(s1y, s2y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > (int)Wx - 1) maxx = (int)Wx - 1;
    if (maxy > (int)Wy - 1) maxy = (int)Wy - 1;
    int scx = (int)bp[BP_SCISSOR], scy = (int)bp[BP_SCISSOR + 1];
    int scw = (int)bp[BP_SCISSOR + 2], sch = (int)bp[BP_SCISSOR + 3];
    if (scw > 0 && sch > 0) {
        if (minx < scx) minx = scx;
        if (miny < scy) miny = scy;
        if (maxx > scx + scw - 1) maxx = scx + scw - 1;
        if (maxy > scy + sch - 1) maxy = scy + sch - 1;
    }
    if (minx > maxx || miny > maxy)
        return;

    /* edgef(a,b,P) = A*P.x - B*P.y + C, pre-divided by the area */
    float A0 = s2y - s1y, B0 = s2x - s1x, C0 = B0 * s1y - A0 * s1x;
    float A1 = s0y - s2y, B1 = s0x - s2x, C1 = B1 * s2y - A1 * s2x;
    float A2 = s1y - s0y, B2 = s1x - s0x, C2 = B2 * s0y - A2 * s0x;
    r[TR_EDGE + 0] = A0 * inv_area;
    r[TR_EDGE + 1] = -B0 * inv_area;
    r[TR_EDGE + 2] = C0 * inv_area;
    r[TR_EDGE + 3] = A1 * inv_area;
    r[TR_EDGE + 4] = -B1 * inv_area;
    r[TR_EDGE + 5] = C1 * inv_area;
    r[TR_EDGE + 6] = A2 * inv_area;
    r[TR_EDGE + 7] = -B2 * inv_area;
    r[TR_EDGE + 8] = C2 * inv_area;
    r[TR_IW + 0] = iw0; r[TR_IW + 1] = iw1; r[TR_IW + 2] = iw2;
    r[TR_ZW + 0] = v0[2] * iw0; r[TR_ZW + 1] = v1[2] * iw1;
    r[TR_ZW + 2] = v2[2] * iw2;
    int na = (int)bp[BP_NATTR];
    for (int a = 0; a < na; a++) {
        r[TR_ATTR + a * 3 + 0] = v0[4 + a] * iw0;
        r[TR_ATTR + a * 3 + 1] = v1[4 + a] * iw1;
        r[TR_ATTR + a * 3 + 2] = v2[4 + a] * iw2;
    }
    box[0] = minx; box[1] = miny; box[2] = maxx; box[3] = maxy;

    int tiles_x = (int)bp[BP_TILES_X];
    unsigned cap = bp[BP_CAP];
    int tx0 = minx / PGL_TILE, tx1 = maxx / PGL_TILE;
    int ty0 = miny / PGL_TILE, ty1 = maxy / PGL_TILE;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            int tile = ty * tiles_x + tx;
            unsigned slot = atomicAdd(&bin_count[tile], 1u);
            if (slot < cap)
                bin_list[(unsigned)tile * cap + slot] = (unsigned)t;
        }
    }
}

PGL_KERNEL void pgl_bin_sort(const unsigned *bin_count, unsigned *bin_list,
                             const unsigned *bp)
{
    unsigned tile = blockIdx.x * blockDim.x + threadIdx.x;
    if (tile >= bp[BP_TILES_X] * bp[BP_TILES_Y])
        return;
    unsigned cap = bp[BP_CAP];
    unsigned n = bin_count[tile];
    if (n < 2u || n > cap)
        return;                 /* overflowed tiles scan in order anyway */
    unsigned *l = bin_list + tile * cap;
    for (unsigned i = 1; i < n; i++) {
        unsigned key = l[i];
        unsigned j = i;
        while (j > 0u && l[j - 1u] > key) {
            l[j] = l[j - 1u];
            j--;
        }
        l[j] = key;
    }
}

/* attribute a of the record, interpolated with barycentrics b and 1/(1/w) */
static __device__ float pgl_attr(const float *r, int a, float b0, float b1,
                                 float b2, float rw)
{
    const float *p = r + TR_ATTR + a * 3;
    return (b0 * p[0] + b1 * p[1] + b2 * p[2]) * rw;
}

static __device__ int pgl_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

PGL_KERNEL void pgl_raster_tile(unsigned *fb, unsigned *depth,
                                const float *trec, const unsigned *bin_count,
                                const unsigned *bin_list, const unsigned *tex,
                                const unsigned *u, const unsigned *bp)
{
    int x = (int)(blockIdx.x * PGL_TILE + threadIdx.x);
    int y = (int)(blockIdx.y * PGL_TILE + threadIdx.y);
    if (x >= (int)pgl_uf(u, RS_VIEWPORT_WH) ||
        y >= (int)pgl_uf(u, RS_VIEWPORT_WH + 1))
        return;
    unsigned tile = blockIdx.y * bp[BP_TILES_X] + blockIdx.x;
    unsigned cap = bp[BP_CAP];
    unsigned n = bin_count[tile];
    int clear = (bp[BP_FLAGS] & BP_FLAG_CLEAR) != 0u;
    if (n == 0u && !clear)
        return;

    unsigned mode = bp[BP_MODE];
    unsigned blend = bp[BP_BLEND];
    unsigned depth_write = bp[BP_DEPTH_WRITE];
    int use_depth = mode != BP_MODE_TEXCOL;
    int tw = (int)bp[BP_TEX_W], th = (int)bp[BP_TEX_H];
    int idx = y * (int)u[RS_FB_STRIDE] + x;
    unsigned d = 0u, col = 0u;
    int cdirty = clear, zdirty = clear && use_depth;
    if (clear) {
        col = bp[BP_CLEAR_COLOR];
        d = bp[BP_CLEAR_DEPTH];
    } else {
        if (use_depth)
            d = depth[idx];
        if (mode == BP_MODE_TEXCOL || blend != 0u)
            col = fb[idx];
    }

    int unordered = (bp[BP_FLAGS] & BP_FLAG_UNORDERED) != 0u;
    unsigned owner = 0xffffffffu;   /* triangle that set d; none = the buffer */
    int overflow = n > cap;
    unsigned total = overflow ? u[RS_TRI_COUNT] : n;
    const unsigned *list = bin_list + tile * cap;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    for (unsigned i = 0; i < total; i++) {
        unsigned t = overflow ? i : list[i];
        const float *r = trec + t * TR_WORDS;
        const int *box = (const int *)(r + TR_BOX);
        if (x < box[0] || y < box[1] || x > box[2] || y > box[3])
            continue;
        float b0 = r[TR_EDGE + 0] * fx + r[TR_EDGE + 1] * fy + r[TR_EDGE + 2];
        float b1 = r[TR_EDGE + 3] * fx + r[TR_EDGE + 4] * fy + r[TR_EDGE + 5];
        float b2 = r[TR_EDGE + 6] * fx + r[TR_EDGE + 7] * fy + r[TR_EDGE + 8];
        if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
            continue;
        float iw = b0 * r[TR_IW] + b1 * r[TR_IW + 1] + b2 * r[TR_IW + 2];
        if (iw <= 0.0f)
            continue;
        float rw = 1.0f / iw;
        if (use_depth) {
            float z = (b0 * r[TR_ZW] + b1 * r[TR_ZW + 1] +
                       b2 * r[TR_ZW + 2]) * rw;
            unsigned dd = (unsigned)(pgl_clamp01(z) * 16777215.0f);
            if (dd > d)
                continue;
            if (dd == d &&
                (!unordered || owner == 0xffffffffu || t >= owner))
                continue;
            if (depth_write != 0u) {
                d = dd;
                zdirty = 1;
                owner = t;
            }
        }
        if (mode == BP_MODE_COLOR) {
            float cr = pgl_attr(r, 0, b0, b1, b2, rw);
            float cg = pgl_attr(r, 1, b0, b1, b2, rw);
            float cb = pgl_attr(r, 2, b0, b1, b2, rw);
            if (blend == 0u) {
                col = pgl_pack(cr, cg, cb);
            } else {
                float sa = pgl_clamp01(pgl_attr(r, 3, b0, b1, b2, rw));
                float sr = pgl_clamp01(cr), sg = pgl_clamp01(cg);
                float sb = pgl_clamp01(cb);
                float dr = (float)((col >> 16) & 255u) / 255.0f;
                float dg = (float)((col >> 8) & 255u) / 255.0f;
                float db = (float)(col & 255u) / 255.0f;
                if (blend == 2u)
                    col = pgl_pack(dr + sr * sa, dg + sg * sa, db + sb * sa);
                else
                    col = pgl_pack(sr * sa + dr * (1.0f - sa),
                                   sg * sa + dg * (1.0f - sa),
                                   sb * sa + db * (1.0f - sa));
            }
        } else if (mode == BP_MODE_TEX) {
            /* NEAREST + CLAMP-to-edge, V flipped; texel written verbatim */
            float tu = pgl_attr(r, 0, b0, b1, b2, rw);
            float tv = pgl_attr(r, 1, b0, b1, b2, rw);
            int tx = pgl_clampi((int)(tu * (float)tw), 0, tw - 1);
            int ty = pgl_clampi((int)((1.0f - tv) * (float)th), 0, th - 1);
            col = tex[ty * tw + tx];
        } else {
            /* bilinear + CLAMP-to-edge (no V flip), texel * vertex colour,
               RGB: SRC_ALPHA / ONE_MINUS_SRC_ALPHA, A: ONE / ONE_MINUS_SRC_ALPHA */
            float gx = pgl_attr(r, 0, b0, b1, b2, rw) * (float)tw - 0.5f;
            float gy = pgl_attr(r, 1, b0, b1, b2, rw) * (float)th - 0.5f;
            int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
            float ax = gx - (float)x0, ay = gy - (float)y0;
            int xa = pgl_clampi(x0, 0, tw - 1), xb = pgl_clampi(x0 + 1, 0, tw - 1);
            int ya = pgl_clampi(y0, 0, th - 1), yb = pgl_clampi(y0 + 1, 0, th - 1);
            unsigned q00 = tex[ya * tw + xa], q10 = tex[ya * tw + xb];
            unsigned q01 = tex[yb * tw + xa], q11 = tex[yb * tw + xb];
            float src[4];
            for (int c = 0; c < 4; c++) {
                /* channel order r, g, b, a = bits 16, 8, 0, 24 */
                int sh = (c == 3) ? 24 : (16 - 8 * c);
                float t0 = (float)((q00 >> sh) & 255u) / 255.0f;
                float t1 = (float)((q10 >> sh) & 255u) / 255.0f;
                float t2 = (float)((q01 >> sh) & 255u) / 255.0f;
                float t3 = (float)((q11 >> sh) & 255u) / 255.0f;
                float top = t0 + (t1 - t0) * ax, bot = t2 + (t3 - t2) * ax;
                src[c] = (top + (bot - top) * ay) *
                         pgl_attr(r, 2 + c, b0, b1, b2, rw);
            }
            float sa = pgl_clamp01(src[3]);
            float dr = (float)((col >> 16) & 255u) / 255.0f;
            float dg = (float)((col >> 8) & 255u) / 255.0f;
            float db = (float)(col & 255u) / 255.0f;
            float da = (float)((col >> 24) & 255u) / 255.0f;
            unsigned a8 = (unsigned)(pgl_clamp01(sa + da * (1.0f - sa)) *
                                     255.0f + 0.5f);
            col = (pgl_pack(src[0] * sa + dr * (1.0f - sa),
                            src[1] * sa + dg * (1.0f - sa),
                            src[2] * sa + db * (1.0f - sa)) & 0x00ffffffu) |
                  (a8 << 24);
        }
        cdirty = 1;
    }
    if (cdirty)
        fb[idx] = col;
    if (zdirty)
        depth[idx] = d;
}
