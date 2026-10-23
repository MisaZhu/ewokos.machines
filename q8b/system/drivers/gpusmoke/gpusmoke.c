/* Bounded IPC readback test using an already-running g2dd. Never initializes
 * hardware. Direct API checks have no rendering fallback; --api-integration
 * explicitly tests the public graph library's software fallback separately. */
#include <g2dclient/g2dclient.h>
#include <graph/graph.h>
#include <graph/graph_ex.h>
#include <graph/graph_arch.h>
#include <graph/graph_g2d.h>
#include <ewoksys/kernel_tic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define W 64u
#define H 64u
#define PIXELS (W * H)
#define BYTES (PIXELS * 4u)
#define GUARD 0x13579bdfu

typedef struct {
    uint32_t *p;
    unsigned w, h, count;
    ewokos_addr_t phy;
    g2d_canvas_t canvas;
} surface_t;

typedef uint32_t (*expected_fn)(unsigned, unsigned, void *);

static int surface_new(surface_t *s, unsigned w, unsigned h) {
    int id;
    ewokos_addr_t pa;
    uint32_t bytes = w * h * 4u + 0x4000u;
    memset(s, 0, sizeof(*s));
    if(g2d_shm_alloc_phy(bytes, &id, &s->p, &pa) != 0) return 1;
    s->w = w; s->h = h; s->count = bytes / 4u;
    s->phy = pa;
    s->canvas = g2d_canvas(id, bytes, w, h, 1);
    for(unsigned i = 0; i < s->count; i++) s->p[i] = GUARD;
    return 0;
}

static int checked(int ret) {
    g2d_quiesce_on_fault(ret);
    return ret;
}

static uint32_t pattern(unsigned x, unsigned y, void *arg) {
    (void)arg;
    return 0xff000000u | (((3*x + 7*y) & 255u) << 16) |
           (((5*x + y) & 255u) << 8) | ((x + 11*y) & 255u);
}

static uint32_t solid(unsigned x, unsigned y, void *arg) {
    (void)x; (void)y;
    return *(uint32_t *)arg;
}

static uint32_t cropped(unsigned x, unsigned y, void *arg) {
    surface_t *s = arg;
    if(x >= 11 && x < s->w - 9 && y >= 17 && y < s->h - 13)
        return pattern(x - 11 + 3, y - 17 + 5, NULL);
    return 0xff204060u;
}

static uint32_t rotated(unsigned x, unsigned y, void *arg) {
    surface_t *s = arg;
    return pattern(s->w - 1 - x, s->h - 1 - y, NULL);
}

static int surface_check(const char *name, surface_t *s, expected_fn expected,
                         void *arg) {
    unsigned mismatch = 0, guards = 0;
    volatile uint32_t *p = s->p;
    for(unsigned y = 0; y < s->h; y++) {
        for(unsigned x = 0; x < s->w; x++) {
            uint32_t want = expected(x, y, arg), got = p[y * s->w + x];
            if(got != want && mismatch++ == 0)
                printf("%s first mismatch (%u,%u): %08x expected %08x\n",
                       name, x, y, got, want);
        }
    }
    for(unsigned i = s->w * s->h; i < s->count; i++)
        if(p[i] != GUARD) guards++;
    printf("%s %ux%u: %s (%u pixels, %u guards)\n", name, s->w, s->h,
           mismatch || guards ? "FAIL" : "PASS", mismatch, guards);
    return mismatch || guards;
}

/* Full-frame, cropped, rotated and scaled operations use aligned pitches
 * and direct IPC APIs, with independent CPU expectations only for readback.
 * Eight consecutive fills exercise changing fence values and stale caches. */
static int extended_case(unsigned w, unsigned h) {
    surface_t src, dst;
    g2d_fill_req_t fill;
    g2d_blit_req_t copy;
    g2d_rotate_req_t rotate;
    g2d_scale_to_req_t scale;
    uint32_t color;
    int ret;
    if(surface_new(&src, w, h)) return 1;
    if(surface_new(&dst, w, h)) {
        g2d_shm_free(src.p);
        return 1;
    }
    for(unsigned y = 0; y < h; y++)
        for(unsigned x = 0; x < w; x++) src.p[y*w+x] = pattern(x, y, NULL);
    for(unsigned frame = 0; frame < 8; frame++) {
        color = 0xff204060u + frame * 0x00010203u;
        g2d_fill_req_init(&fill, dst.canvas, g2d_rect(0, 0, w, h), color);
        ret = checked(g2d_fill_rect(&fill));
        if(ret) { printf("repeated fill: ret=%d\n", ret); return 1; }
        if(surface_check("repeated fill", &dst, solid, &color)) return 1;
    }
    color = 0xff204060u;
    g2d_fill_req_init(&fill, dst.canvas, g2d_rect(0, 0, w, h), color);
    ret = checked(g2d_fill_rect(&fill));
    if(ret) return 1;
    g2d_blit_req_init(&copy, dst.canvas, src.canvas,
                     g2d_rect(3, 5, w - 20, h - 30),
                     g2d_rect(11, 17, w - 20, h - 30), 0xff);
    ret = checked(g2d_blit(&copy));
    if(ret) { printf("cropped copy: ret=%d\n", ret); return 1; }
    if(surface_check("cropped copy", &dst, cropped, &dst)) return 1;
    g2d_rotate_req_init(&rotate, src.canvas, dst.canvas, 180);
    ret = checked(g2d_rotate(&rotate));
    if(ret) { printf("rotate180: ret=%d\n", ret); return 1; }
    if(surface_check("rotate180", &dst, rotated, &src) ||
       surface_check("source unchanged", &src, pattern, NULL)) return 1;

    /* Replace a previously sampled source on the CPU: the next blit must
     * invalidate its old texture cache. A uniform image has an exact
     * scaling oracle independent of bilinear coordinate rounding. */
    color = 0xffa17c39u;
    for(unsigned i = 0; i < w*h; i++) src.p[i] = color;
    for(unsigned i = 0; i < dst.count; i++) dst.p[i] = GUARD;
    dst.w = (w / 2u) & ~15u; dst.h = h / 2u;
    dst.canvas.w = dst.w; dst.canvas.h = dst.h;
    g2d_scale_to_req_init(&scale, src.canvas, dst.canvas);
    ret = checked(g2d_scale_to(&scale));
    if(ret) { printf("scale down: ret=%d\n", ret); return 1; }
    if(surface_check("scale down uniform", &dst, solid, &color) ||
       surface_check("scale source unchanged", &src, solid, &color)) return 1;
    g2d_shm_free(src.p);
    g2d_shm_free(dst.p);
    return 0;
}

static int verify(const char *name, volatile uint32_t *p, int rect) {
    unsigned mismatch = 0;
    for(unsigned y = 0; y < H; y++) {
        for(unsigned x = 0; x < W; x++) {
            uint32_t want = rect && x >= 7 && x < 26 && y >= 9 && y < 22
                ? 0xffa17c39u : 0xff204060u;
            uint32_t got = p[y * W + x];
            if(got != want && mismatch++ == 0)
                printf("%s first mismatch (%u,%u): %08x expected %08x\n",
                       name, x, y, got, want);
        }
    }
    for(unsigned i = PIXELS; i < 2 * PIXELS; i++) {
        if(p[i] != GUARD) mismatch++;
    }
    printf("%s: %s (%u pixel/guard mismatches)\n", name,
           mismatch ? "FAIL" : "PASS", mismatch);
    return mismatch ? 1 : 0;
}

#include "api_tests.h"

int main(int argc, char **argv) {
    if(argc == 2 && strncmp(argv[1], "--api", 5) == 0)
        return api_tests(argv[1]);
    int clear_only = argc == 2 && strcmp(argv[1], "--clear") == 0;
    int extended = argc == 2 && strcmp(argv[1], "--extended") == 0;
    int src_id, dst_id, ret;
    uint32_t *src = NULL, *dst = NULL;
    ewokos_addr_t src_pa, dst_pa;
    g2d_pgl_req_t clear;
    g2d_fill_req_t fill;
    g2d_blit_req_t blit;
    if(argc > 1 && !clear_only && !extended) {
        printf("usage: gpusmoke [--clear|--extended|--apis|--api-blur|--api-blur-stress|--api-raster|--api-scale|--api-physical|--api-alpha|--api-cpu-alpha|--api-integration]\n");
        return 1;
    }
    if(has_g2d() != 0 ||
       g2d_shm_alloc_phy(2 * BYTES, &src_id, &src, &src_pa) != 0 ||
       g2d_shm_alloc_phy(2 * BYTES, &dst_id, &dst, &dst_pa) != 0) {
        printf("gpusmoke: device/contiguous allocation unavailable\n");
        g2d_shm_free(src);
        g2d_shm_free(dst);
        return 1;
    }
    g2d_canvas_t sc = g2d_canvas(src_id, 2 * BYTES, W, H, 1);
    g2d_canvas_t dc = g2d_canvas(dst_id, 2 * BYTES, W, H, 1);
    for(unsigned i = 0; i < 2 * PIXELS; i++) src[i] = dst[i] = GUARD;
    printf("gpusmoke: 64x64, guarded contiguous surfaces, IPC only\n");
    g2d_pgl_clear_req_init(&clear, sc, G2D_PGL_FMT_RGBA8, W * 4,
                          0xff204060u, 0);
    ret = checked(g2d_pgl_clear(&clear));
    printf("clear: ret=%d\n", ret);
    if(ret || verify("clear readback", src, 0)) goto failed;
    if(!clear_only) {
        g2d_fill_req_init(&fill, sc, g2d_rect(7, 9, 19, 13), 0xffa17c39u);
        ret = checked(g2d_fill_rect(&fill));
        printf("fill: ret=%d\n", ret);
        if(ret || verify("fill readback", src, 1)) goto failed;
        g2d_blit_req_init(&blit, dc, sc, g2d_rect(0, 0, W, H),
                         g2d_rect(0, 0, W, H), 0xff);
        ret = checked(g2d_blit(&blit));
        printf("copy: ret=%d\n", ret);
        if(ret || verify("copy readback", dst, 1) ||
           verify("copy source unchanged", src, 1)) goto failed;
    }
    g2d_shm_free(src);
    g2d_shm_free(dst);
    if(extended && (extended_case(64, 64) || extended_case(320, 240) ||
                    extended_case(720, 720))) goto failed;
    printf("gpusmoke: PASS\n");
    return 0;
failed:
    /* On a submitted timeout the daemon may still own these surfaces.
     * Do not send more work or deliberately reuse their storage. */
    printf("gpusmoke: FAIL; stop submissions and inspect serial log\n");
    return 1;
}
