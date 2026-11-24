/*
 * portablegl geometry kernels on GA10B: fill + vertex transform + raster of
 * a synthetic scene, compared with the CPU reference built from the same
 * kernel source (kernels/pgl_geom.cu compiled as C via pgl_sim.h).
 *
 * GPU and CPU differ in float contraction (nvcc fuses mul+add) and the GPU
 * resolves overlapping triangles concurrently, so the comparison is by
 * tolerance: per-channel colour delta, depth delta, and a mismatch budget.
 */
#include <stdlib.h>
#include <string.h>
#include <ewoksys/klog.h>

#include "ga10b.h"

#define PGL_SIM
#include "kernels/pgl_geom.cu"

struct pgl_sim_dim3 pgl_sim_threadIdx, pgl_sim_blockIdx, pgl_sim_blockDim;

#define T_W          512u
#define T_H          384u
#define T_SMALL      1000u           /* scattered small triangles */
#define T_TRIS       (12u + T_SMALL) /* + one cube */
#define T_VERTS      (T_TRIS * 3u)
#define T_CLEAR      0xff102030u
#define T_DEPTH_FAR  0xffffffffu
#define T_COL_TOL    2
#define T_DEPTH_TOL  64u             /* of 24-bit unorm */
#define T_BUDGET_PM  10u             /* mismatching pixels, per mille */

static uint32_t lcg_state = 0x1234567u;
static float lcg(void) {             /* [0,1) */
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (float)(lcg_state >> 8) * (1.0f / 16777216.0f);
}

/* perspective(60deg, w/h, 0.5, 20) * translate(0,0,-4) * rotate, column major */
static void scene_mvp(float* m) {
    const float f = 1.7320508f, aspect = (float)T_W / (float)T_H;
    const float n = 0.5f, fa = 20.0f;
    const float c = 0.8660254f, s = 0.5f;      /* 30 deg about Y */
    const float cx = 0.9396926f, sx = 0.3420201f; /* 20 deg about X */
    float r[16] = {
        c, sx * s, -cx * s, 0,
        0, cx, sx, 0,
        s, -sx * c, cx * c, 0,
        0, 0, -4.0f, 1,
    };
    float p[16] = {
        f / aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (fa + n) / (n - fa), -1,
        0, 0, 2.0f * fa * n / (n - fa), 0,
    };
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += p[k * 4 + row] * r[col * 4 + k];
            m[col * 4 + row] = v;
        }
}

static void scene_build(float* pos, float* col) {
    static const float cv[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 },
    };
    static const uint8_t ci[36] = {
        0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,
        3, 6, 2, 3, 7, 6,  0, 4, 7, 0, 7, 3,  1, 2, 6, 1, 6, 5,
    };
    uint32_t v = 0;
    for (uint32_t i = 0; i < 36; i++, v++) {
        const float* p = cv[ci[i]];
        pos[v * 3 + 0] = p[0];
        pos[v * 3 + 1] = p[1];
        pos[v * 3 + 2] = p[2];
        col[v * 3 + 0] = p[0] * 0.5f + 0.5f;
        col[v * 3 + 1] = p[1] * 0.5f + 0.5f;
        col[v * 3 + 2] = p[2] * 0.5f + 0.5f;
    }
    for (uint32_t t = 0; t < T_SMALL; t++) {
        float cx = lcg() * 5.0f - 2.5f, cy = lcg() * 4.0f - 2.0f;
        float cz = lcg() * 5.0f - 2.5f;
        for (uint32_t k = 0; k < 3; k++, v++) {
            pos[v * 3 + 0] = cx + lcg() * 0.3f - 0.15f;
            pos[v * 3 + 1] = cy + lcg() * 0.3f - 0.15f;
            pos[v * 3 + 2] = cz + lcg() * 0.3f - 0.15f;
            col[v * 3 + 0] = lcg();
            col[v * 3 + 1] = lcg();
            col[v * 3 + 2] = lcg();
        }
    }
}

static void ref_run(float* vout, const float* pos, const float* col,
                    uint32_t* fb, uint32_t* depth, const uint32_t* xu,
                    const uint32_t* ru) {
    for (uint32_t i = 0; i < T_W * T_H; i++) {
        fb[i] = T_CLEAR;
        depth[i] = T_DEPTH_FAR;
    }
    pgl_sim_blockDim.x = 64;
    for (uint32_t v = 0; v < T_VERTS; v++) {
        pgl_sim_blockIdx.x = v / 64u;
        pgl_sim_threadIdx.x = v % 64u;
        pgl_xform_color(vout, pos, col, xu);
    }
    pgl_sim_blockDim.x = 256;
    for (uint32_t t = 0; t < T_TRIS; t++) {
        pgl_sim_blockIdx.x = t / 256u;
        pgl_sim_threadIdx.x = t % 256u;
        pgl_raster(fb, vout, depth, ru);
    }
}

static int absdiff(int a, int b) { return a > b ? a - b : b - a; }

int ga10b_pgl_selftest(ga10b_chan_t* ch, const char* cubin_path) {
    uint32_t fsize = 0;
    void* file = ga10b_file_read(cubin_path, &fsize);
    if (file == NULL) {
        klog("ga10b: cannot read cubin %s\n", cubin_path);
        return -1;
    }
    ga10b_module_t* m = ga10b_module_load(ch->mm, file, fsize);
    free(file);
    if (m == NULL)
        return -1;
    int k_fill = ga10b_module_kernel(m, "pgl_fill");
    int k_xform = ga10b_module_kernel(m, "pgl_xform_color");
    int k_raster = ga10b_module_kernel(m, "pgl_raster");
    if (k_fill < 0 || k_xform < 0 || k_raster < 0) {
        klog("ga10b: pgl kernels missing in %s\n", cubin_path);
        return -1;
    }

    const uint32_t npix = T_W * T_H;
    void *fbv, *zv, *posv, *colv, *voutv, *xuv, *ruv;
    uint64_t g_fb, g_z, g_pos, g_col, g_vout, g_xu, g_ru;
    if (ga10b_buf_alloc(ch->mm, npix * 4u, &fbv, &g_fb, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, npix * 4u, &zv, &g_z, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, T_VERTS * 12u, &posv, &g_pos, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, T_VERTS * 12u, &colv, &g_col, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, T_VERTS * 32u, &voutv, &g_vout, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, 4096, &xuv, &g_xu, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, 4096, &ruv, &g_ru, 0) != 0)
        return -1;
    uint32_t* fb = (uint32_t*)fbv;
    uint32_t* depth = (uint32_t*)zv;
    uint32_t* xu = (uint32_t*)xuv;
    uint32_t* ru = (uint32_t*)ruv;

    scene_build((float*)posv, (float*)colv);
    scene_mvp((float*)xu + XF_MVP);
    ((float*)xu)[XF_VIEWPORT_WH] = (float)T_W;
    ((float*)xu)[XF_VIEWPORT_WH + 1] = (float)T_H;
    xu[XF_VERT_COUNT] = T_VERTS;
    xu[XF_COL_ALPHA] = 0;
    ((float*)ru)[RS_VIEWPORT_WH] = (float)T_W;
    ((float*)ru)[RS_VIEWPORT_WH + 1] = (float)T_H;
    ru[RS_FB_STRIDE] = T_W;
    ru[RS_TRI_FIRST] = 0;
    ru[RS_TRI_COUNT] = T_TRIS;
    ru[RS_BLEND_MODE] = 0;
    ru[RS_DEPTH_WRITE] = 1;
    ru[RS_CULL_SIGN] = 0;
    memset(fb, 0x55, npix * 4u);
    memset(depth, 0x55, npix * 4u);
    __asm__ volatile("dsb sy" ::: "memory");

    /* GPU: clear colour, clear depth, transform, raster */
    const uint32_t b256[3] = { 256, 1, 1 }, b64[3] = { 64, 1, 1 };
    const uint32_t g_fill[3] = { (npix + 255u) / 256u, 1, 1 };
    const uint32_t g_xf[3] = { (T_VERTS + 63u) / 64u, 1, 1 };
    const uint32_t g_rs[3] = { (T_TRIS + 255u) / 256u, 1, 1 };
    uint32_t v_clear = T_CLEAR, v_far = T_DEPTH_FAR, v_n = npix;
    void* a_fill_fb[3] = { &g_fb, &v_clear, &v_n };
    void* a_fill_z[3] = { &g_z, &v_far, &v_n };
    void* a_xf[4] = { &g_vout, &g_pos, &g_col, &g_xu };
    void* a_rs[4] = { &g_fb, &g_vout, &g_z, &g_ru };
    if (ga10b_kernel_launch(ch, m, k_fill, g_fill, b256, a_fill_fb) != 0 ||
        ga10b_kernel_launch(ch, m, k_fill, g_fill, b256, a_fill_z) != 0 ||
        ga10b_kernel_launch(ch, m, k_xform, g_xf, b64, a_xf) != 0 ||
        ga10b_kernel_launch(ch, m, k_raster, g_rs, b256, a_rs) != 0) {
        klog("ga10b: pgl launch failed\n");
        ga10b_gr_launch_postmortem();
        return -1;
    }
    if (!ga10b.hw)
        return 0;

    /* CPU reference */
    float* r_vout = malloc(T_VERTS * 32u);
    uint32_t* r_fb = malloc(npix * 4u);
    uint32_t* r_z = malloc(npix * 4u);
    if (r_vout == NULL || r_fb == NULL || r_z == NULL)
        return -1;
    ref_run(r_vout, (const float*)posv, (const float*)colv, r_fb, r_z, xu, ru);

    uint32_t vbad = 0;
    const float* g_v = (const float*)voutv;
    for (uint32_t i = 0; i < T_VERTS * 8u; i++) {
        float d = g_v[i] - r_vout[i];
        float mag = r_vout[i] < 0 ? -r_vout[i] : r_vout[i];
        if (d < 0) d = -d;
        if (d > 1e-3f + mag * 1e-4f)
            vbad++;
    }
    uint32_t covered = 0, cbad = 0, zbad = 0, exact = 0, uncleared = 0;
    for (uint32_t i = 0; i < npix; i++) {
        uint32_t g = fb[i], r = r_fb[i];
        if (g == 0x55555555u)
            uncleared++;
        if (r != T_CLEAR)
            covered++;
        if (g == r && depth[i] == r_z[i])
            exact++;
        if (absdiff((int)(g >> 16 & 255u), (int)(r >> 16 & 255u)) > T_COL_TOL ||
            absdiff((int)(g >> 8 & 255u), (int)(r >> 8 & 255u)) > T_COL_TOL ||
            absdiff((int)(g & 255u), (int)(r & 255u)) > T_COL_TOL ||
            (g >> 24) != (r >> 24))
            cbad++;
        uint32_t dz = depth[i] > r_z[i] ? depth[i] - r_z[i] : r_z[i] - depth[i];
        if (dz > T_DEPTH_TOL)
            zbad++;
    }
    klog("ga10b: pgl %ux%u tris=%u verts=%u covered=%u exact=%u "
         "vout_bad=%u colour_bad=%u depth_bad=%u uncleared=%u\n", T_W, T_H,
         T_TRIS, T_VERTS, covered, exact, vbad, cbad, zbad, uncleared);
    klog("ga10b: pgl sample fb[center]=%08x ref=%08x z=%08x ref=%08x\n",
         fb[(T_H / 2u) * T_W + T_W / 2u], r_fb[(T_H / 2u) * T_W + T_W / 2u],
         depth[(T_H / 2u) * T_W + T_W / 2u], r_z[(T_H / 2u) * T_W + T_W / 2u]);
    uint32_t budget = npix * T_BUDGET_PM / 1000u;
    int ok = vbad == 0 && uncleared == 0 && covered > npix / 20u &&
             cbad <= budget && zbad <= budget;
    free(r_vout);
    free(r_fb);
    free(r_z);
    if (!ok) {
        klog("ga10b: pgl raster FAIL (budget %u px)\n", budget);
        ga10b_gr_launch_postmortem();
        return -1;
    }
    klog("ga10b: pgl raster PASS (fill + xform + raster vs CPU reference)\n");
    return 0;
}
