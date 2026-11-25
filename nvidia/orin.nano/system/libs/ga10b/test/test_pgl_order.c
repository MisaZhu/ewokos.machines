/*
 * host test: an order-free draw (BP_FLAG_UNORDERED, pgl_bin_sort skipped)
 * must produce the frame in-order compositing produces, whatever order the
 * racing pgl_tri_setup threads appended to the tile lists in.
 *
 * The scene is built to tie: depths come from a handful of levels and every
 * triangle is flat, so overlapping triangles meet at exactly equal depth all
 * over the frame.  A negative control (shuffled lists, flag clear) has to
 * differ, or the scene would not be exercising the tie rule at all.
 *
 *   cc -I. -Iinclude -o /tmp/test_pgl_order test/test_pgl_order.c && \
 *       /tmp/test_pgl_order
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ga10b/ga10b_pgl.h>

#define PGL_SIM
#include "../kernels/pgl_geom.cu"

struct pgl_sim_dim3 pgl_sim_threadIdx, pgl_sim_blockIdx, pgl_sim_blockDim;

#define W      320u
#define H      240u
#define NPIX   (W * H)
#define TX     ((W + 15u) / 16u)
#define TY     ((H + 15u) / 16u)
#define TILES  (TX * TY)
#define NTRI   3000u
#define TEXN   32u

static uint32_t rng_state = 0x13572468u;
static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(rng() & 0xffffu) / 65535.0f;
}

static float vout[NTRI * 3u * 8u];
static float trec[NTRI * TR_WORDS];
static uint32_t tex[TEXN * TEXN];

static void scene(void) {
    for (uint32_t t = 0; t < NTRI; t++) {
        /* a few screen-filling triangles, the rest small */
        float span = t < 6u ? 400.0f : frand(4.0f, 60.0f);
        float cx = frand(0.0f, (float)W), cy = frand(0.0f, (float)H);
        float z = 0.1f * (float)(1u + rng() % 6u);
        for (uint32_t v = 0; v < 3u; v++) {
            float* p = vout + (t * 3u + v) * 8u;
            p[0] = cx + frand(-span, span);
            p[1] = cy + frand(-span, span);
            p[2] = z;
            p[3] = 1.0f;
            for (uint32_t a = 0; a < 4u; a++)
                p[4 + a] = frand(0.0f, 1.0f);
        }
    }
    for (uint32_t i = 0; i < TEXN * TEXN; i++)
        tex[i] = 0xff000000u | rng();
}

typedef struct {
    uint32_t fb[NPIX], z[NPIX];
    uint32_t cnt[TILES];
    uint32_t* list;
    uint32_t maxbin, overflowed;
} frame_t;

/* one draw the way the daemon sequences it; shuffle != 0 puts every tile
 * list in a random order first, flag_unordered chooses the kernel rule and
 * whether the sort pass runs */
static void draw(frame_t* f, uint32_t raster_kernel, uint32_t cap,
                 int shuffle, int flag_unordered) {
    uint32_t ru[GA10B_PGL_RTC_DWORDS] = { 0 };
    uint32_t bp[GA10B_PGL_BP_WORDS];
    const float wh[2] = { (float)W, (float)H };
    memcpy(&ru[GA10B_PGL_RS_VIEWPORT_WH], wh, 8);
    ru[GA10B_PGL_RS_FB_STRIDE] = W;
    ru[GA10B_PGL_RS_TRI_COUNT] = NTRI;
    if (raster_kernel == GA10B_PGL_K_RASTER) {
        ru[GA10B_PGL_RS_DEPTH_WRITE] = 1;
    } else {
        ru[GA10B_PGL_RT_TEX_W] = TEXN;
        ru[GA10B_PGL_RT_TEX_H] = TEXN;
    }
    if (ga10b_pgl_fill_bp(bp, raster_kernel, ru, TX, TY, cap, 1,
                          0xff102030u) != 0 ||
        !(bp[GA10B_PGL_BP_FLAGS] & GA10B_PGL_BP_FLAG_UNORDERED)) {
        printf("FAIL: draw state is not order-free\n");
        exit(1);
    }
    if (!flag_unordered)
        bp[GA10B_PGL_BP_FLAGS] &= ~GA10B_PGL_BP_FLAG_UNORDERED;

    memset(f->cnt, 0, sizeof(f->cnt));
    memset(f->fb, 0x55, sizeof(f->fb));
    memset(f->z, 0x55, sizeof(f->z));
    f->list = realloc(f->list, (size_t)TILES * cap * 4u);
    pgl_sim_blockDim.x = 256;
    for (uint32_t t = 0; t < NTRI; t++) {
        pgl_sim_blockIdx.x = t / 256u;
        pgl_sim_threadIdx.x = t % 256u;
        pgl_tri_setup(trec, f->cnt, f->list, vout, ru, bp);
    }
    f->maxbin = f->overflowed = 0;
    for (uint32_t t = 0; t < TILES; t++) {
        uint32_t n = f->cnt[t];
        if (n > f->maxbin)
            f->maxbin = n;
        if (n > cap) {
            f->overflowed++;
            continue;
        }
        uint32_t* l = f->list + t * cap;
        for (uint32_t i = n; shuffle && i > 1u; i--) {
            uint32_t j = rng() % i, tmp = l[i - 1u];
            l[i - 1u] = l[j];
            l[j] = tmp;
        }
    }
    if (!flag_unordered && !shuffle)
        for (uint32_t t = 0; t < TILES; t++) {
            pgl_sim_blockIdx.x = t / 256u;
            pgl_sim_threadIdx.x = t % 256u;
            pgl_bin_sort(f->cnt, f->list, bp);
        }
    for (uint32_t by = 0; by < TY; by++)
        for (uint32_t bx = 0; bx < TX; bx++)
            for (uint32_t i = 0; i < 256u; i++) {
                pgl_sim_blockIdx.x = bx;
                pgl_sim_blockIdx.y = by;
                pgl_sim_threadIdx.x = i % 16u;
                pgl_sim_threadIdx.y = i / 16u;
                pgl_raster_tile(f->fb, f->z, trec, f->cnt, f->list, tex, ru,
                                bp);
            }
}

static uint32_t diff(const frame_t* a, const frame_t* b) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < NPIX; i++)
        n += a->fb[i] != b->fb[i] || a->z[i] != b->z[i];
    return n;
}

static frame_t ref, got;

int main(void) {
    static const struct { uint32_t kernel; const char* name; } modes[] = {
        { GA10B_PGL_K_RASTER, "colour" },
        { GA10B_PGL_K_RASTER_TEX, "textured" },
    };
    int fails = 0;
    scene();
    for (uint32_t m = 0; m < 2u; m++) {
        /* reference: sorted lists, the in-order rule, nothing overflows */
        draw(&ref, modes[m].kernel, NTRI, 0, 0);
        uint32_t covered = 0;
        for (uint32_t i = 0; i < NPIX; i++)
            covered += ref.fb[i] != 0xff102030u;

        /* caps: nothing overflows / some tiles overflow / all busy ones do */
        const uint32_t caps[3] = { NTRI, ref.maxbin - ref.maxbin / 8u, 8u };
        for (uint32_t c = 0; c < 3u; c++) {
            for (uint32_t round = 0; round < 4u; round++) {
                draw(&got, modes[m].kernel, caps[c], 1, 1);
                uint32_t bad = diff(&ref, &got);
                if (bad != 0u)
                    fails++;
                if (bad != 0u || round == 0u)
                    printf("%s %-9s cap=%-4u shuffled+unordered: %u pixels "
                           "differ (fullest bin %u, %u/%u tiles overflow)\n",
                           bad ? "FAIL" : "ok  ", modes[m].name, caps[c], bad,
                           got.maxbin, got.overflowed, TILES);
            }
        }
        /* control: the old rule on shuffled lists must NOT match */
        draw(&got, modes[m].kernel, NTRI, 1, 0);
        uint32_t ctl = diff(&ref, &got);
        printf("%s %-9s control (shuffled, in-order rule): %u pixels differ, "
               "%u covered\n", ctl ? "ok  " : "FAIL", modes[m].name, ctl,
               covered);
        if (ctl == 0u || covered < NPIX / 2u)
            fails++;
    }
    printf("test_pgl_order: %s\n", fails ? "FAIL" : "ALL PASS");
    return fails != 0;
}
