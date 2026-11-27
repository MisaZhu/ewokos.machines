#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bsp/bsp_vic.h>
#include <bsp/bsp_fb.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>

/*
 * VIC bring-up probe.  Steps are cumulative (each powers/maps its
 * prerequisites), so any step can be run cold.  Booting the falcon resets
 * the engine: do not run boot/method/2d/fb/bench while g2dd owns it.  Default step is "regs"
 * (power-on + three falcon register reads) -- the smallest-risk first
 * touch of the VIC aperture.  Run stages manually in order:
 *
 *   vicprobe regs      BPMP power/clock/reset + CPUCTL/IDLESTATE/DMACTL read
 *   vicprobe host1x    HV VM tables + syncpt protection + SMMU bypass check
 *   vicprobe syncpt    CPU_INCR round-trip on syncpt 512
 *   vicprobe channel   host1x-class pushbuffer, IMMEDIATE syncpt incr
 *   vicprobe boot [fw] falcon DMA load of vic.bin + STARTCPU + MMIO Nop
 *   vicprobe method    VIC-class pushbuffer: Nop + ENGINE_IDLE fences
 *   vicprobe 2d        M2 selftest: fill/copy/rotate jobs, pixel readback
 *   vicprobe fb        2d, then a VIC fill + copy into the top-left corner
 *                      of the scan-out framebuffer with CPU readback, and
 *                      whole-frame push timing (VIC vs CPU)
 *   vicprobe bench     2d, then VIC vs CPU timing of fill and 1:1 copy at
 *                      several sizes on uncached DMA memory
 *   vicprobe shot x y w h [n] [ms]
 *                      scan-out readback (no VIC access, safe next to g2dd):
 *                      n captures of the region ms apart, which of them
 *                      differ, and every distinct one as base64 RGB565
 *   vicprobe all     = 2d (full chain)
 */

#define FW_PATH "/drivers/orin.nano/firmware/vic.bin"

static uint32_t now_usec(void) {
    uint32_t low = 0;
    kernel_tic32(NULL, NULL, &low);
    return low;
}

#define FB_T_W     64
#define FB_T_H     64
#define FB_T_PITCH 256

/* Does the engine reach the scan-out carveout the same way it reaches
 * DMA-pool memory?  Paints a 64x64 corner; run without a desktop. */
static int fb_probe(void) {
    if (bsp_vic_2d_enable() != 0)
        return -1;
    if (bsp_fb_init(0, 0, 32) != 0)
        return -1;
    disp_info_t* fb = bsp_get_fbinfo();
    volatile uint32_t* px = (volatile uint32_t*)(ewokos_addr_t)fb->pointer;
    uint32_t stride = fb->pitch / 4u;
    printf("vicprobe: fb phy=%llx %ux%u pitch=%u depth=%u\n",
            (unsigned long long)fb->phy_base, fb->width, fb->height,
            fb->pitch, fb->depth);

    int r = bsp_vic_fill(fb->phy_base, fb->pitch, (int32_t)fb->width,
            (int32_t)fb->height, 0, 0, FB_T_W, FB_T_H, 0xff204060u);
    uint32_t bad = 0;
    for (int32_t y = 0; y < FB_T_H; y++)
        for (int32_t x = 0; x < FB_T_W; x++)
            if (px[y * stride + x] != 0xff204060u)
                bad++;
    printf("vicprobe: fb fill = %d bad=%u px0=%08x\n", r, bad, px[0]);
    if (r != 0 || bad != 0)
        return -1;

    ewokos_addr_t src_va = dma_alloc(FB_T_PITCH * FB_T_H);
    if (src_va == 0)
        return -1;
    uint32_t* src = (uint32_t*)src_va;
    for (int32_t y = 0; y < FB_T_H; y++)
        for (int32_t x = 0; x < FB_T_W; x++)
            src[y * (FB_T_PITCH / 4) + x] = 0xff000000u |
                    ((uint32_t)(x * 4) << 16) | ((uint32_t)(y * 4) << 8) |
                    (uint32_t)((x ^ y) & 0xff);
    r = bsp_vic_blit_xrgb(dma_phy_addr(src_va), FB_T_PITCH, FB_T_W, FB_T_H,
            0, 0, FB_T_W, FB_T_H,
            fb->phy_base, fb->pitch, (int32_t)fb->width, (int32_t)fb->height,
            0, 0, 0);
    bad = 0;
    for (int32_t y = 0; y < FB_T_H; y++)
        for (int32_t x = 0; x < FB_T_W; x++)
            if (((px[y * stride + x] ^ src[y * (FB_T_PITCH / 4) + x]) &
                    0x00ffffffu) != 0)
                bad++;
    printf("vicprobe: fb copy = %d bad=%u px(1,1)=%08x want=%08x\n", r, bad,
            px[stride + 1], src[FB_T_PITCH / 4 + 1]);
    dma_free(src_va);
    if (r != 0 || bad != 0)
        return -1;

    /* Whole-frame push, the op the desktop runs every frame: VIC from an
     * uncached contiguous canvas, CPU from the same canvas, CPU from an
     * ordinary cached one. */
    uint32_t frame = fb->pitch * fb->height;
    ewokos_addr_t nc_va = dma_alloc(frame);
    uint8_t* cached = (uint8_t*)malloc(frame);
    if (nc_va == 0 || cached == NULL) {
        printf("vicprobe: fb frame bench skipped (no memory)\n");
        return 0;
    }
    memset((void*)nc_va, 0x30, frame);
    memset(cached, 0x30, frame);
    uint32_t t0 = now_usec();
    for (int i = 0; i < 4 && r == 0; i++)
        r = bsp_vic_blit_xrgb(dma_phy_addr(nc_va), fb->pitch,
                (int32_t)fb->width, (int32_t)fb->height,
                0, 0, (int32_t)fb->width, (int32_t)fb->height,
                fb->phy_base, fb->pitch, (int32_t)fb->width,
                (int32_t)fb->height, 0, 0, 0);
    uint32_t t_vic = (now_usec() - t0) / 4u;
    t0 = now_usec();
    memcpy((void*)(ewokos_addr_t)fb->pointer, (void*)nc_va, frame);
    uint32_t t_nc = now_usec() - t0;
    t0 = now_usec();
    for (int i = 0; i < 4; i++)
        memcpy((void*)(ewokos_addr_t)fb->pointer, cached, frame);
    uint32_t t_cached = (now_usec() - t0) / 4u;
    printf("vicprobe: fb frame %ux%u push: vic=%u usec (r=%d) cpu-from-uncached=%u usec cpu-from-cached=%u usec\n",
            fb->width, fb->height, t_vic, r, t_nc, t_cached);
    dma_free(nc_va);
    free(cached);
    return r == 0 ? 0 : -1;
}

#define BN_MAX   1024
#define BN_PITCH (BN_MAX * 4)

/* Where does a VIC job start beating the CPU?  Both sides work on the
 * same uncached DMA memory the desktop canvases use. */
static int bench(void) {
    if (bsp_vic_2d_enable() != 0)
        return -1;
    ewokos_addr_t a = dma_alloc(BN_PITCH * BN_MAX);
    ewokos_addr_t b = dma_alloc(BN_PITCH * BN_MAX);
    if (a == 0 || b == 0)
        return -1;
    uint64_t pa = dma_phy_addr(a), pb = dma_phy_addr(b);
    uint32_t* va = (uint32_t*)a;
    uint32_t* vb = (uint32_t*)b;
    memset(va, 0x40, BN_PITCH * BN_MAX);
    memset(vb, 0x80, BN_PITCH * BN_MAX);
    static const int32_t sizes[] = {16, 32, 64, 128, 256, 512, 1024};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int32_t n = sizes[i];
        uint32_t iters = n <= 128 ? 200 : (n <= 512 ? 40 : 10);
        int r = 0;
        uint32_t t0 = now_usec();
        for (uint32_t k = 0; k < iters && r == 0; k++)
            r = bsp_vic_fill(pb, BN_PITCH, BN_MAX, BN_MAX, 0, 0, n, n,
                    0xff102030u + k);
        uint32_t vic_fill = (now_usec() - t0) / iters;
        t0 = now_usec();
        for (uint32_t k = 0; k < iters; k++)
            for (int32_t y = 0; y < n; y++) {
                uint32_t* row = vb + y * BN_MAX;
                for (int32_t x = 0; x < n; x++)
                    row[x] = 0xff102030u + k;
            }
        uint32_t cpu_fill = (now_usec() - t0) / iters;
        t0 = now_usec();
        for (uint32_t k = 0; k < iters && r == 0; k++)
            r = bsp_vic_blit(pa, BN_PITCH, BN_MAX, BN_MAX, 0, 0, n, n,
                    pb, BN_PITCH, BN_MAX, BN_MAX, 0, 0, 0);
        uint32_t vic_copy = (now_usec() - t0) / iters;
        t0 = now_usec();
        for (uint32_t k = 0; k < iters; k++)
            for (int32_t y = 0; y < n; y++)
                memcpy(vb + y * BN_MAX, va + y * BN_MAX, (size_t)n * 4u);
        uint32_t cpu_copy = (now_usec() - t0) / iters;
        printf("vicprobe: bench %4dx%-4d fill vic=%u cpu=%u usec | copy vic=%u cpu=%u usec (r=%d)\n",
                n, n, vic_fill, cpu_fill, vic_copy, cpu_copy, r);
        if (r != 0)
            break;
    }
    dma_free(a);
    dma_free(b);
    return 0;
}

#define SHOT_MAX_N    64
#define SHOT_MAX_UNIQ 6

static void b64_line(const uint8_t* p, uint32_t n) {
    static const char tb[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[80];
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[o++] = tb[(v >> 18) & 63];
        out[o++] = tb[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tb[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tb[v & 63] : '=';
    }
    out[o] = 0;
    printf("S:%s\n", out);
}

static int shot(int argc, char* argv[]) {
    if (argc < 6) {
        printf("usage: vicprobe shot x y w h [n] [ms]\n");
        return -1;
    }
    int32_t x = atoi(argv[2]), y = atoi(argv[3]);
    int32_t w = atoi(argv[4]), h = atoi(argv[5]);
    int32_t n = argc > 6 ? atoi(argv[6]) : 1;
    int32_t ms = argc > 7 ? atoi(argv[7]) : 50;
    if (bsp_fb_init(0, 0, 32) != 0)
        return -1;
    disp_info_t* fb = bsp_get_fbinfo();
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || n <= 0 || n > SHOT_MAX_N ||
            (uint32_t)(x + w) > fb->width || (uint32_t)(y + h) > fb->height)
        return -1;
    const uint8_t* base = (const uint8_t*)(ewokos_addr_t)fb->pointer;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u;
    uint8_t* uniq[SHOT_MAX_UNIQ];
    uint8_t* cur = (uint8_t*)malloc(bytes);
    char seq[SHOT_MAX_N + 1];
    int nu = 0;
    if (cur == NULL)
        return -1;
    for (int32_t i = 0; i < n; i++) {
        for (int32_t r = 0; r < h; r++) {
            const uint32_t* row = (const uint32_t*)(base +
                    (uint32_t)(y + r) * fb->pitch) + x;
            uint8_t* o = cur + (uint32_t)r * (uint32_t)w * 2u;
            for (int32_t c = 0; c < w; c++) {
                uint32_t px = row[c];
                uint32_t v = ((px >> 8) & 0xf800u) | ((px >> 5) & 0x07e0u) |
                        ((px >> 3) & 0x001fu);
                o[c * 2] = (uint8_t)(v >> 8);
                o[c * 2 + 1] = (uint8_t)v;
            }
        }
        int id = -1;
        for (int k = 0; k < nu && id < 0; k++)
            if (memcmp(uniq[k], cur, bytes) == 0)
                id = k;
        if (id < 0 && nu < SHOT_MAX_UNIQ) {
            uniq[nu] = cur;
            id = nu++;
            cur = (uint8_t*)malloc(bytes);
            if (cur == NULL)
                break;
        }
        seq[i] = id < 0 ? '?' : (char)('0' + id);
        seq[i + 1] = 0;
        if (i + 1 < n)
            usleep((uint32_t)ms * 1000u);
    }
    printf("SHOT-SEQ %s\n", seq);
    for (int k = 0; k < nu; k++) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < bytes; i++)
            sum += uniq[k][i];
        printf("SHOT-BEGIN %d %d %d\n", k, w, h);
        for (uint32_t i = 0; i < bytes; i += 54)
            b64_line(uniq[k] + i, bytes - i < 54 ? bytes - i : 54);
        printf("SHOT-END %d %u\n", k, sum);
    }
    return 0;
}

int main(int argc, char* argv[]) {
    const char* step = argc > 1 ? argv[1] : "regs";
    int r;
    if (strcmp(step, "power") == 0)
        r = bsp_vic_power_on();
    else if (strcmp(step, "regs") == 0)
        r = bsp_vic_regs_probe();
    else if (strcmp(step, "host1x") == 0)
        r = bsp_vic_host1x_init();
    else if (strcmp(step, "syncpt") == 0)
        r = bsp_vic_syncpt_test();
    else if (strcmp(step, "channel") == 0)
        r = bsp_vic_channel_test();
    else if (strcmp(step, "boot") == 0)
        r = bsp_vic_falcon_boot(argc > 2 ? argv[2] : FW_PATH);
    else if (strcmp(step, "method") == 0)
        r = bsp_vic_method_test();
    else if (strcmp(step, "bench") == 0)
        r = bench();
    else if (strcmp(step, "shot") == 0)
        r = shot(argc, argv);
    else if (strcmp(step, "2d") == 0 || strcmp(step, "all") == 0 ||
            strcmp(step, "fb") == 0) {
        if (strcmp(step, "fb") == 0) {
            r = fb_probe();
        } else {
            r = bsp_vic_2d_enable();
            printf("vicprobe: 2d rotate = %d\n", bsp_vic_2d_can_rotate());
        }
    } else {
        printf("usage: vicprobe [power|regs|host1x|syncpt|channel|boot [fw]|method|2d|fb|bench|shot x y w h [n] [ms]|all]\n");
        return 1;
    }
    printf("vicprobe %s = %d\n", step, r);
    return r == 0 ? 0 : 1;
}
