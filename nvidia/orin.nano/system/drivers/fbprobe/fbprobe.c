/*
 * Orin Nano live framebuffer test.  The loader now hands off the PRECISE linear
 * scan-out FB phys read from the closed display provider's iface[+96] (the dest
 * the GOP Blt BufferToVideo engine writes per-pixel, linear y*pitch+x*4 --
 * confirmed in the provider disasm).  UEFI already programmed the scan-out and
 * it survives ExitBootServices, so kernel pixel writes to this FB appear on the
 * panel directly -- no Blt shim, no DCE RPC, no MMIO (firewall-safe).
 *
 * This probe maps the FB, reads back the firmware splash (proof we hit the real
 * scan-out FB, not the block-linear FBPT carveout), draws full-screen SMPTE color
 * bars, then reads back to confirm the writes are coherent.  Result via klog.
 *   fbprobe
 */
#include <bsp/bsp_fb.h>
#include <stdint.h>
#include <string.h>
#include <ewoksys/klog.h>

/* provider Blt buffer / scan-out FB pixel layout: B,G,R,X in memory (format 1) */
#define BGRX(r, g, b) ((uint32_t)(b) | ((uint32_t)(g) << 8) | ((uint32_t)(r) << 16))

static void dump_row(const char* tag, volatile uint32_t* fb, uint32_t pitch,
                     uint32_t y, uint32_t x0) {
    volatile uint32_t* row = (volatile uint32_t*)((uint8_t*)fb + (uint64_t)y * pitch);
    klog("fbprobe: %s y=%u x=%u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            tag, y, x0, row[x0], row[x0 + 1], row[x0 + 2], row[x0 + 3],
            row[x0 + 4], row[x0 + 5], row[x0 + 6], row[x0 + 7]);
}

int main(void) {
    klog("fbprobe: start (precise linear scan-out FB)\n");

    if (bsp_fb_init(0, 0, 0) != 0) {
        klog("fbprobe: no framebuffer (attach a display so UEFI builds GOP)\n");
        return 1;
    }
    disp_info_t* fb = bsp_get_fbinfo();
    klog("fbprobe: fb pa=0x%llx va=0x%llx %ux%u bpp=%u pitch=%u size=%u\n",
            (unsigned long long)fb->phy_base, (unsigned long long)fb->pointer,
            fb->width, fb->height, fb->depth, fb->pitch, fb->size);

    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)fb->pointer;
    uint32_t w = fb->width, h = fb->height, pitch = fb->pitch;
    if (w == 0 || h == 0 || pitch < w * 4) {
        klog("fbprobe: bad geometry\n");
        return 1;
    }

    /* Read back the firmware splash BEFORE writing: real pixel data here proves
     * we mapped the live linear scan-out FB (the FBPT carveout would show page
     * tables / magic, not a smooth image). */
    dump_row("before", p, pitch, 0, 0);
    dump_row("before", p, pitch, h / 2, w / 2);

    /* Draw 8 vertical SMPTE color bars across the full screen. */
    static const uint32_t bars[8] = {
        BGRX(255, 255, 255),  /* white   */
        BGRX(255, 255, 0),    /* yellow  */
        BGRX(0, 255, 255),    /* cyan    */
        BGRX(0, 255, 0),      /* green   */
        BGRX(255, 0, 255),    /* magenta */
        BGRX(255, 0, 0),      /* red     */
        BGRX(0, 0, 255),      /* blue    */
        BGRX(0, 0, 0),        /* black   */
    };
    uint32_t bar_w = w / 8;
    for (uint32_t y = 0; y < h; y++) {
        volatile uint32_t* row = (volatile uint32_t*)((uint8_t*)p + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t b = x / bar_w;
            if (b > 7)
                b = 7;
            row[x] = bars[b];
        }
    }
    /* A white square at top-left of the red bar to mark orientation. */
    for (uint32_t y = 10; y < 60 && y < h; y++) {
        volatile uint32_t* row = (volatile uint32_t*)((uint8_t*)p + (uint64_t)y * pitch);
        for (uint32_t x = bar_w * 5 + 10; x < bar_w * 5 + 60 && x < w; x++)
            row[x] = BGRX(255, 255, 255);
    }

    /* Read back AFTER writing: must match the bars (coherent CPU writes). */
    dump_row("after ", p, pitch, 0, 0);
    dump_row("after ", p, pitch, h / 2, bar_w * 2 + 4);
    dump_row("after ", p, pitch, h / 2, bar_w * 5 + 4);

    uint32_t expect0 = bars[0];
    uint32_t got0 = p[0];
    uint32_t gotmid = ((volatile uint32_t*)((uint8_t*)p + (uint64_t)(h / 2) * pitch))[bar_w * 2 + 4];
    if (got0 == expect0 && gotmid == bars[2]) {
        klog("fbprobe: DISPLAY LIVE -- bars written+readback coherent on linear scan-out FB pa=0x%llx\n",
                (unsigned long long)fb->phy_base);
    } else {
        klog("fbprobe: readback MISMATCH got0=%08x exp0=%08x gotmid=%08x expmid=%08x\n",
                got0, expect0, gotmid, bars[2]);
    }

    /* Stability: delay, then re-read the centre.  If it still holds the bar
     * colour, nothing is clearing the FB post-EBS and the image persists on
     * the scan-out.  If it reverted to 0, something still owns/clears it. */
    for (volatile uint64_t d = 0; d < 600000000ULL; d++)
        __asm__ volatile("" ::: "memory");
    uint32_t stable_mid = ((volatile uint32_t*)((uint8_t*)p + (uint64_t)(h / 2) * pitch))[bar_w * 2 + 4];
    uint32_t stable0 = p[0];
    klog("fbprobe: stability after delay y0=%08x(exp %08x) mid=%08x(exp %08x) %s\n",
            stable0, bars[0], stable_mid, bars[2],
            (stable0 == bars[0] && stable_mid == bars[2]) ? "STABLE" : "CLEARED");
    return 0;
}