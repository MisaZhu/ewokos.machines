#ifndef EFI_FDT_FB_H
#define EFI_FDT_FB_H

#include <stdint.h>
#include <string.h>

/* Minimal flattened-device-tree helpers shared by the loader (model +
 * simple-framebuffer handoff) and the host-side parser unit test.  Pure
 * byte math, no UEFI/OS deps. */

static uint32_t fdt_be32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int fdt_str_eq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int fdt_str_prefix(const char* s, const char* pre) {
    while (*pre) {
        if (*s != *pre)
            return 0;
        s++;
        pre++;
    }
    return 1;
}

/* Linear framebuffer handed off via a /simple-framebuffer DT node (the
 * "simplefb" method; NVIDIA UEFI uses either this or efifb/GOP).  Decodes
 * reg/width/height/stride/format into the same fields the GOP path fills. */
typedef struct {
    uint64_t addr, size;
    uint32_t width, height, stride;
    uint32_t bpp, format;   /* format: 0=RGB memory order, 1=BGR */
    int found;
} simplefb_t;

static void fdt_simplefb(uint64_t dtb, simplefb_t* fb) {
    fb->addr = fb->size = 0;
    fb->width = fb->height = fb->stride = 0;
    fb->bpp = 32;
    fb->format = 1;
    fb->found = 0;
    if (!dtb)
        return;
    const uint8_t* f = (const uint8_t*)(uintptr_t)dtb;
    if (fdt_be32(f) != 0xd00dfeed)
        return;
    uint32_t totalsize = fdt_be32(f + 4);
    if (totalsize < 40 || totalsize > 16u * 1024 * 1024)
        return;
    uint32_t off_struct = fdt_be32(f + 8);
    uint32_t off_str = fdt_be32(f + 12);
    uint32_t size_str = fdt_be32(f + 32);
    if (off_struct + 4 > totalsize || off_str > totalsize ||
            size_str > totalsize || off_str + size_str > totalsize)
        return;
    const uint8_t* strblk = f + off_str;
    const uint8_t* st = f + off_struct;
    uint32_t p = 0;
    int depth = 0, target = -1;
    char namebuf[48];
    while (p + 4 <= totalsize - off_struct) {
        uint32_t tok = fdt_be32(st + p);
        p += 4;
        if (tok == 1) {                       /* FDT_BEGIN_NODE */
            uint32_t q = p;
            while (q + 1 < totalsize - off_struct && st[q] != 0)
                q++;
            uint32_t nlen = q - p;
            if (nlen >= sizeof(namebuf))
                nlen = sizeof(namebuf) - 1;
            memcpy(namebuf, st + p, nlen);
            namebuf[nlen] = 0;
            p = (q + 1 + 3) & ~3u;
            depth++;
            if (target < 0 &&
                    (fdt_str_prefix(namebuf, "simple-framebuffer") ||
                     fdt_str_prefix(namebuf, "framebuffer")))
                target = depth;
        } else if (tok == 3) {                /* FDT_PROP */
            if (p + 8 > totalsize - off_struct)
                break;
            uint32_t len = fdt_be32(st + p);
            uint32_t nameoff = fdt_be32(st + p + 4);
            uint64_t data = p + 8;
            p = (uint32_t)(data + len);
            p = (p + 3) & ~3u;
            if (target < 0 || depth != target || nameoff >= size_str ||
                    !strblk[nameoff])
                continue;
            const char* pn = (const char*)(strblk + nameoff);
            const uint8_t* pd = st + data;
            if (fdt_str_eq(pn, "reg")) {
                if (len >= 16) {
                    fb->addr = ((uint64_t)fdt_be32(pd) << 32) | fdt_be32(pd + 4);
                    fb->size = ((uint64_t)fdt_be32(pd + 8) << 32) |
                            fdt_be32(pd + 12);
                } else if (len >= 8) {
                    fb->addr = fdt_be32(pd);
                    fb->size = fdt_be32(pd + 4);
                }
            } else if (fdt_str_eq(pn, "width") && len >= 4) {
                fb->width = fdt_be32(pd);
            } else if (fdt_str_eq(pn, "height") && len >= 4) {
                fb->height = fdt_be32(pd);
            } else if (fdt_str_eq(pn, "stride") && len >= 4) {
                fb->stride = fdt_be32(pd);
            } else if (fdt_str_eq(pn, "format")) {
                char fmt[24];
                uint32_t n = len;
                if (n > 0 && pd[n - 1] == 0)
                    n--;
                if (n >= sizeof(fmt))
                    n = sizeof(fmt) - 1;
                memcpy(fmt, pd, n);
                fmt[n] = 0;
                if (fdt_str_prefix(fmt, "r5g6b5")) {
                    fb->bpp = 16;
                    fb->format = 0;
                } else {
                    int ri = -1, bi = -1;
                    for (uint32_t i = 0; i < n; i++) {
                        if (fmt[i] == 'r' && ri < 0) ri = (int)i;
                        if (fmt[i] == 'b' && bi < 0) bi = (int)i;
                    }
                    fb->bpp = (fmt[0] == 'x' || fmt[0] == 'a') ? 32 : 24;
                    /* component listed first = higher bits = later byte in
                     * little-endian memory: r-before-b -> B,G,R (format 1). */
                    if (ri >= 0 && bi >= 0)
                        fb->format = (ri < bi) ? 1 : 0;
                }
            }
        } else if (tok == 2) {                /* FDT_END_NODE */
            if (target >= 0 && depth == target) {
                fb->found = (fb->addr != 0 && fb->width != 0 &&
                        fb->height != 0);
                return;
            }
            depth--;
        } else {                              /* FDT_END / garbage */
            break;
        }
    }
}

#endif /* EFI_FDT_FB_H */