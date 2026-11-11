/*
 * BOOTAA64.EFI — EwokOS Q8B UEFI bootloader.
 *
 * The Radxa Dragon Q8B has no U-Boot: its SPI NOR EDK2 firmware loads this
 * PE32+ application (Boot From File / UEFI Shell / PXE).  We then:
 *
 *   1. locate the GOP framebuffer and record it for user space,
 *   2. load KERNEL.BIN and a small INITRAMFS.IMG bootstrap image either from a
 *      SimpleFileSystem volume (ESP/USB), EFI_LOAD_FILE (PXE TFTP), or a GRUB
 *      initrd exposed as EFI_LOAD_FILE2; init then selects SD or USB rootfs,
 *   3. reserve and fill the kernel at Q8B_KERNEL_PHYS,
 *   4. collect the final UEFI memory map, ExitBootServices,
 *   5. publish the bootinfo page at Q8B_BOOTINFO_PHYS and jump to the kernel.
 *
 * Calling convention: AArch64 UEFI is AAPCS64, identical to the compiler's
 * normal convention, so no assembly shim is required.
 */
#include "efi.h"

/* Must match machines/qcom/8gen3/kernel/config.mk, mkos.lds.S and bsp/hw_arch.h.
 *
 * Q8B_QEMU_TEST builds the same loader for a QEMU virt smoke test: only
 * the DRAM base and the fixed bootinfo/kernel physical addresses change,
 * so the SFS -> ExitBootServices -> jump path can be exercised without a
 * board.  The mock payload is loader/test/mock_kernel.S. */
#ifdef Q8B_QEMU_TEST
#define Q8B_RAM_BASE        0x40000000ULL
#define Q8B_KERNEL_PHYS     0x80080000ULL
#define Q8B_BOOTINFO_PHYS   0x80070000ULL
#else
#define Q8B_RAM_BASE        0x80000000ULL
#define Q8B_KERNEL_PHYS     0x90080000ULL
#define Q8B_BOOTINFO_PHYS   0x90070000ULL
#endif
#define Q8B_KERNEL_RESERVE  (4ULL * 1024 * 1024)
#define Q8B_KERNEL_MTFTP_MAX (4ULL * 1024 * 1024)
#define Q8B_INITRAMFS_MTFTP_MAX (32ULL * 1024 * 1024)
#define Q8B_ROOTFS_MTFTP_MAX (160ULL * 1024 * 1024)
#define Q8B_BOOTINFO_SIZE   0x10000ULL
#define Q8B_USABLE_LIMIT    (8ULL * 1024 * 1024 * 1024)

#define Q8B_BOOTINFO_MAGIC   0x42385145u
#define Q8B_BOOTINFO_VERSION 1
#define Q8B_RD_FLAG_BOOT_ONLY (1u << 0)

#define Q8B_BOOT_SOURCE_UNKNOWN 0u
#define Q8B_BOOT_SOURCE_SD      1u
#define Q8B_BOOT_SOURCE_USB     2u
#define Q8B_BOOT_SOURCE_NVME    3u
#define Q8B_BOOT_SOURCE_NET     4u

#define Q8B_MAX_RESV 32

typedef struct {
    uint64_t base;
    uint64_t size;
} q8b_resv_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    uint64_t ram_base;
    uint64_t ram_size;

    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t fb_format;
    uint32_t _pad_fb;

    uint64_t rd_base;
    uint64_t rd_size;
    uint32_t rd_flags;
    uint32_t boot_source;

    uint64_t kernel_phys;
    uint64_t kernel_size;

    uint64_t memmap_addr;
    uint32_t memmap_count;
    uint32_t memmap_dsize;

    uint32_t resv_count;
    uint32_t serial_flags;   /* bit0: kernel may use UART17; bit1: loader used EFI Serial I/O */
    q8b_resv_t resv[Q8B_MAX_RESV];

    uint64_t dtb_addr;
    uint64_t dtb_size;

    /* Device model from the firmware DTB /model, handed to the kernel so it
     * can publish the real board name in sys_info.machine (MACHINE_MAX). */
    char model[32];
} q8b_bootinfo_t;

static EFI_SYSTEM_TABLE* gST;
static EFI_BOOT_SERVICES* gBS;
static EFI_HANDLE gImage;
static EFI_LOADED_IMAGE_PROTOCOL* gImageInfo;
static int g_con_ok;
static EFI_SERIAL_IO_PROTOCOL* g_serial;
static int g_serial_mode;      /* 0=none, 1=EFI Serial I/O, 2=UART17 direct */
static int g_uart17_ok;        /* raw UART17 TX probe completed a command */
static uint32_t g_boot_source = Q8B_BOOT_SOURCE_UNKNOWN;
/* A small, dependency-free GOP console used while the loader is alive.  The
 * kernel receives the same framebuffer through bootinfo, but it is useful to
 * see whether UEFI reached the file-loading stages before ExitBootServices. */
static volatile uint32_t* g_fb;
static uint32_t g_fb_w, g_fb_h;      /* logical (upright panel) resolution */
static uint32_t g_fb_lay_w, g_fb_lay_h; /* memory layout columns/rows */
static uint32_t g_fb_scanpx;         /* pixels per physical scanline */
static uint32_t g_fb_rot;            /* scanout rotation, quarter turns 0..3 */
static uint32_t g_fb_format;
static uint32_t g_fb_rmask, g_fb_gmask, g_fb_bmask;
static int g_fb_debug;
/* Keep the boot-only RAMFS outside the board's DMA32 window when possible.
 * Q8B reserves large contiguous DMA/SHM pools below 4 GiB; placing a
 * 16-MB initramfs in the middle of that range can fragment the only usable
 * 768-MB interval and make pool reservation fail before init runs. */
static int g_alloc_high;

/* ------------------------------------------------------------------ */
/* tiny freestanding runtime                                          */
/* ------------------------------------------------------------------ */
void* memcpy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--)
        *dd++ = *ss++;
    return d;
}

void* memset(void* d, int c, uint64_t n) {
    uint8_t* dd = (uint8_t*)d;
    while (n--)
        *dd++ = (uint8_t)c;
    return d;
}

static uint64_t strlen_(const char* s) {
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int memcmp_(const void* a, const void* b, uint64_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint64_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

__attribute__((noreturn)) static void halt_forever(void) {
    for (;;)
        __asm__ volatile("wfi");
}

/* ------------------------------------------------------------------ */
/* serial log (EFI Serial I/O, fallback to the Q8B GENI UART17)       */
/* ------------------------------------------------------------------ */
#ifndef Q8B_QEMU_TEST
#define Q8B_UART17_PHYS 0x00884000ULL

static inline void uart_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(Q8B_UART17_PHYS + (uint64_t)off) = v;
}

static inline uint32_t uart_rd(uint32_t off) {
    return *(volatile uint32_t*)(Q8B_UART17_PHYS + (uint64_t)off);
}

static int uart_poll(uint32_t off, uint32_t mask, int set) {
    for (volatile uint32_t i = 0; i < 200000; i++) {
        uint32_t v = uart_rd(off);
        if (((v & mask) != 0) == (set != 0))
            return 0;
    }
    return -1;
}

/* Drive one GENI UART17 TX command.  Returns 0 when the write completed
 * (M_CMD_DONE seen), -1 when the PE never advanced (abort + timeout).
 * Every device access stays bounded: on boards without a wired/clocked
 * UART17 (MateBook gaokun3, TW220 tablets) the polls simply time out. */
static int uart17_putc(char c) {
    uart_wr(0x80c, 2);
    uart_wr(0x618, (1u << 0) | (1u << 5) | (1u << 30));
    uart_wr(0x270, 1);
    uart_wr(0x600, 1u << 27);  /* UART_START_TX */
    if (uart_poll(0x610, 1u << 30, 1) != 0) {
        uart_wr(0x604, 1u << 1);
        uart_wr(0x618, 1u << 5);
        return -1;
    }
    /* BIOS 可能用 1 byte/word packing（0x260 == 0），原来的 c<<24 会
     * 只把 0x00 送进 TX FIFO，串口表现为空格/NUL。按实际 packing 选字节。 */
    uint32_t pack = uart_rd(0x260);
    uint32_t word = (pack & 0x0F) ? (((uint32_t)(uint8_t)c) << 24)
                                  : ((uint32_t)(uint8_t)c);
    uart_wr(0x700, word);
    uart_wr(0x618, 1u << 30);
    if (uart_poll(0x610, 1u << 0, 1) != 0) {
        uart_wr(0x604, 1u << 1);
        uart_wr(0x618, 1u << 5);
        return -1;
    }
    uart_wr(0x618, 1u << 0);
    return 0;
}

/* One bounded TX probe.  0 = UART17 completes commands (Q8B wiring), -1 =
 * the SE never answered (tablet without a wired UART17 debug header). */
static int uart17_tx_probe(void) {
    const char probe[] = "\r\n";
    int n = 0;
    for (const char* p = probe; *p; p++) {
        if (uart17_putc(*p) != 0)
            break;
        n++;
    }
    return n > 0 ? 0 : -1;
}
#endif /* !Q8B_QEMU_TEST */

static void serial_init(void) {
    g_serial = 0;
    g_serial_mode = 0;
    g_uart17_ok = 0;
#ifdef Q8B_NO_SERIAL
    /* TEMP (gaokun3 / TW220 bring-up): no serial console at all.  Skips the
     * EFI Serial I/O path, the UART17 TX probe and the post-EBS raw write,
     * so the loader never touches 0x884000 and the framebuffer console is
     * the only output.  Rebuild without NO_SERIAL to restore it. */
    return;
#endif
#ifndef Q8B_QEMU_TEST
    /* UEFI BIOS已经初始化好 UART17。loader 不再碰任何 UART 配置，
     * 只用 EFI Serial I/O 的 Write（或 UART17 的 TX 命令）输出字符串。 */
#ifdef Q8B_SERIAL_FORCE_UART17
    g_serial_mode = 2;       /* raw TX only, no init */
    g_uart17_ok = uart17_tx_probe() == 0;
    return;
#endif
    static const EFI_GUID serial_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    if (!EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&serial_guid, 0,
                    (void**)&g_serial)) && g_serial) {
        g_serial_mode = 1;
        /* On Q8B the firmware console IS the GENI debug UART (uart17); the
         * kernel drives the same SE after ExitBootServices, so tell it the
         * UART17 path is usable.  No extra probe: avoid disturbing the SE
         * the firmware serial driver owns. */
        g_uart17_ok = 1;
        return;              /* no SetAttributes: BIOS already did it */
    }
    /* No EFI Serial I/O: only trust the Q8B GENI UART17 when the SE really
     * completes a TX command.  Tablets (MateBook gaokun3, TW220) have no wired
     * UART17 debug header: their QUP2 SE is idle and never answers, so
     * report "no serial" instead of silently poking 0x884000 all boot. */
    if (uart17_tx_probe() == 0) {
        g_serial_mode = 2;       /* raw TX only, no init */
        g_uart17_ok = 1;
    }
#endif
}


static void serial_raw(const char* b, uint64_t n) {
    if (g_serial_mode == 1 && g_serial) {
        while (n) {
            UINTN chunk = n;
            EFI_STATUS st = g_serial->Write(g_serial, &chunk, (void*)b);
            if (EFI_ERROR(st) || chunk == 0)
                break;
            b += chunk;
            n -= chunk;
        }
    }
#ifndef Q8B_QEMU_TEST
    else if (g_serial_mode == 2) {
        for (uint64_t i = 0; i < n; i++)
            uart17_putc(b[i]);
    }
#endif
}

static void serial_write_ascii(const char* s) {
    if (g_serial_mode == 0)
        return;
    char buf[128];
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = *s;
        if (n >= sizeof(buf) - 2) {
            serial_raw(buf, n);
            n = 0;
        }
    }
    if (n)
        serial_raw(buf, n);
}

/* ------------------------------------------------------------------ */
/* console                                                            */
/* ------------------------------------------------------------------ */
static void con_puts(const char* ascii);
static void con_hex(uint64_t v);
static void con_dec(uint64_t v);

static void fb_body_puts(const char* s);

static void con_puts(const char* ascii) {
    /* Mirror every console line onto the framebuffer body console when the
     * direct display console is live (primary screen on tablets, which have
     * no usable serial). */
    if (g_fb_debug)
        fb_body_puts(ascii);
    /* On the real board the UEFI ConOut is also wired to the same debug
     * UART, so writing both duplicates every line.  With a serial backend
     * (EFI Serial I/O or UART17 direct) use it exclusively; QEMU has no
     * serial backend (g_serial_mode==0) and keeps the ConOut path. */
    if (g_serial_mode != 0) {
        serial_write_ascii(ascii);
        return;
    }
#ifndef Q8B_QEMU_TEST
    /* Headless-tablet case (gaokun3 / TW220 have no usable serial): once the
     * direct framebuffer console is live it owns the screen; keep ConOut
     * only until then (same policy as the gaokun3 loader). */
    if (g_fb_debug)
        return;
#endif
    if (!g_con_ok)
        return;
    CHAR16 w[256];
    uint64_t i = 0, j = 0;
    for (; ascii[i] && j < 250; i++) {
        if (ascii[i] == '\n')
            w[j++] = '\r';
        w[j++] = (CHAR16)ascii[i];
    }
    w[j] = 0;
    if (EFI_ERROR(gST->ConOut->OutputString(gST->ConOut, w)))
        g_con_ok = 0;
}

static void con_hex(uint64_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = h[(v >> (60 - i * 4)) & 0xF];
    buf[18] = 0;
    con_puts(buf);
}

static void con_dec(uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0)
        buf[--i] = '0';
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    con_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* tiny GOP debug console                                             */
/* ------------------------------------------------------------------ */
static uint32_t fb_mask_value(uint32_t value, uint32_t mask) {
    uint32_t shift = 0;
    uint32_t width = 0;
    if (mask == 0)
        return 0;
    while (((mask >> shift) & 1u) == 0 && shift < 32)
        shift++;
    while (shift + width < 32 && ((mask >> (shift + width)) & 1u) != 0)
        width++;
    if (width >= 8)
        return (value << shift) & mask;
    return (((value * ((1u << width) - 1u)) / 255u) << shift) & mask;
}

static uint32_t fb_color(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xffu;
    uint32_t g = (rgb >> 8) & 0xffu;
    uint32_t b = rgb & 0xffu;
    if (g_fb_format == 0)       /* PixelRedGreenBlueReserved: bytes R G B */
        return (r << 16) | (g << 8) | b;
    if (g_fb_format == 1)       /* PixelBlueGreenRedReserved: bytes B G R */
        return (b << 16) | (g << 8) | r;
    return fb_mask_value(r, g_fb_rmask) |
           fb_mask_value(g, g_fb_gmask) |
           fb_mask_value(b, g_fb_bmask);
}

/* Map an upright (logical) screen coordinate to a physical framebuffer
 * pixel.  g_fb_rot is the quarter-turn the panel scanout applies, so
 * logical (0,0) is the visible top-left corner on rotated tablets
 * (MateBook gaokun3 / TW220 panels).  Same convention as the gaokun3 loader's
 * gaokun3_pixel_offset(); x/y are bounded by the logical g_fb_w/g_fb_h. */
static void fb_xy(uint32_t x, uint32_t y, uint32_t* px, uint32_t* py) {
    switch (g_fb_rot) {
    case 1: *px = g_fb_lay_w - 1 - y; *py = x; break;          /* 90 CW */
    case 2: *px = g_fb_lay_w - 1 - x; *py = g_fb_lay_h - 1 - y; break;
    case 3: *px = y; *py = g_fb_lay_h - 1 - x; break;          /* 270 CW */
    default: *px = x; *py = y; break;
    }
}

static void fb_poke(uint32_t x, uint32_t y, uint32_t packed) {
    uint32_t px, py;
    fb_xy(x, y, &px, &py);
    if (px >= g_fb_scanpx || py >= g_fb_lay_h)
        return;
    g_fb[(uint64_t)py * g_fb_scanpx + px] = packed;
}

static void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint32_t rgb) {
    if (!g_fb_debug || x >= g_fb_w || y >= g_fb_h)
        return;
    if (w > g_fb_w - x) w = g_fb_w - x;
    if (h > g_fb_h - y) h = g_fb_h - y;
    uint32_t pixel = fb_color(rgb);
    if (g_fb_rot == 0) {
        /* unrotated fast path */
        for (uint32_t yy = 0; yy < h; yy++) {
            volatile uint32_t* row = (volatile uint32_t*)((uint8_t*)g_fb +
                    (uint64_t)(y + yy) * g_fb_scanpx * 4 + (uint64_t)x * 4);
            for (uint32_t xx = 0; xx < w; xx++)
                row[xx] = pixel;
        }
        return;
    }
    for (uint32_t yy = 0; yy < h; yy++)
        for (uint32_t xx = 0; xx < w; xx++)
            fb_poke(x + xx, y + yy, pixel);
}

static uint8_t fb_glyph_row(char c, uint32_t row) {
    static const uint8_t alpha[26][7] = {
        {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
        {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f}, {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
        {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f}, {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
        {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, {0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
        {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
        {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
        {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x1b,0x11}, {0x11,0x0a,0x04,0x04,0x0a,0x11,0x11},
        {0x11,0x0a,0x04,0x04,0x04,0x04,0x04}, {0x1f,0x02,0x04,0x08,0x10,0x10,0x1f}
    };
    static const uint8_t digits[10][7] = {
        {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
        {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
        {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
        {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}
    };
    if (row >= 7)
        return 0;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return alpha[(uint32_t)(c - 'A')][row];
    if (c >= '0' && c <= '9') return digits[(uint32_t)(c - '0')][row];
    if (c == '-') return row == 3 ? 0x1f : 0;
    if (c == ':') return (row == 2 || row == 4) ? 0x04 : 0;
    if (c == '.') return row == 6 ? 0x04 : 0;
    if (c == '/') return (row == 1 || row == 2 || row == 3 || row == 4 || row == 5) ?
            (uint8_t)(1u << (5u - row)) : 0;
    if (c == '+') return row == 3 ? 0x1f : (row == 2 || row == 4 ? 0x04 : 0);
    return 0;
}

static void fb_text(uint32_t x, uint32_t y, const char* text, uint32_t rgb) {
    const uint32_t scale = 2;
    uint32_t color = fb_color(rgb);
    for (; *text; text++, x += 12 * scale / 2) {
        char c = *text;
        for (uint32_t gy = 0; gy < 7; gy++) {
            uint8_t bits = fb_glyph_row(c, gy);
            for (uint32_t gx = 0; gx < 5; gx++) {
                if ((bits & (1u << (4u - gx))) == 0)
                    continue;
                for (uint32_t sy = 0; sy < scale; sy++)
                    for (uint32_t sx = 0; sx < scale; sx++)
                        if (x + gx * scale + sx < g_fb_w &&
                                y + gy * scale + sy < g_fb_h)
                            fb_poke(x + gx * scale + sx, y + gy * scale + sy,
                                    color);
            }
        }
    }
}

/* width/height/scanpx come from the GOP mode (pixels per scanline in
 * memory).  rot is the verified scanout rotation: 0 = as-reported Q8B,
 * 1/3 = portrait panels (gaokun3 / TW220).  Logical console coordinates are
 * upright and bounded by the derived g_fb_w/g_fb_h. */
static void fb_debug_setup(uint64_t addr, uint32_t width, uint32_t height,
                           uint32_t scanpx, uint32_t format,
                           const uint32_t* masks, uint32_t rot) {
    g_fb = (volatile uint32_t*)(uintptr_t)addr;
    if (rot > 3)
        rot = 0;
    g_fb_rot = rot;
    g_fb_lay_w = (rot & 1) ? height : width;      /* memory columns */
    g_fb_lay_h = (rot & 1) ? width : height;      /* memory rows */
    g_fb_scanpx = (rot & 1) ? height : scanpx;    /* px per scanline */
    g_fb_w = (rot & 1) ? g_fb_lay_h : g_fb_lay_w; /* logical width */
    g_fb_h = (rot & 1) ? g_fb_lay_w : g_fb_lay_h; /* logical height */
    g_fb_format = format;
    g_fb_rmask = masks ? masks[0] : 0x00ff0000;
    g_fb_gmask = masks ? masks[1] : 0x0000ff00;
    g_fb_bmask = masks ? masks[2] : 0x000000ff;
    g_fb_debug = addr != 0 && g_fb_w >= 320 && g_fb_h >= 120 &&
                 g_fb_scanpx >= g_fb_lay_w;
    if (!g_fb_debug)
        return;
    /* Whole-screen reset: cover the firmware console's own text so it does
     * not stay superimposed under the scrolling loader log.  Pure black
     * background for the loader output. */
    fb_fill(0, 0, g_fb_w, g_fb_h, 0x000000);
}

/* ------------------------------------------------------------------ */
/* scrolling body console (mirror of con_puts on the framebuffer)     */
/* ------------------------------------------------------------------ */
/* The scrolling log is the only on-screen console: every con_puts line
 * is rendered here, so tablets without a serial port still show the full
 * loader log with no separate fixed-position status text. */
#define FB_BODY_ROWS 16
#define FB_BODY_COLS 78
#define FB_BODY_X    8
#define FB_BODY_Y    8u
#define FB_BODY_PAD  18

static char g_fb_body[FB_BODY_ROWS][FB_BODY_COLS + 1];
static uint32_t g_fb_body_n;
static char g_fb_cur[FB_BODY_COLS + 1];
static uint32_t g_fb_cur_n;

static void fb_body_push(const char* s) {
    uint32_t n = 0;
    if (g_fb_body_n == FB_BODY_ROWS) {
        for (uint32_t r = 1; r < FB_BODY_ROWS; r++)
            memcpy(g_fb_body[r - 1], g_fb_body[r], FB_BODY_COLS + 1);
        g_fb_body_n = FB_BODY_ROWS - 1;
    }
    for (; *s && n < FB_BODY_COLS; s++)
        g_fb_body[g_fb_body_n][n++] = *s;
    g_fb_body[g_fb_body_n][n] = 0;
    g_fb_body_n++;
}

static void fb_body_redraw(void) {
    uint32_t w = g_fb_w > FB_BODY_X + FB_BODY_COLS * 12 + 8 ?
                 FB_BODY_COLS * 12 : g_fb_w - FB_BODY_X - 8;
    uint32_t h = g_fb_h > FB_BODY_Y + FB_BODY_ROWS * FB_BODY_PAD ?
                 FB_BODY_ROWS * FB_BODY_PAD : g_fb_h - FB_BODY_Y;
    if (w == 0 || h == 0)
        return;
    fb_fill(FB_BODY_X, FB_BODY_Y, w, h, 0x000000);
    for (uint32_t r = 0; r < g_fb_body_n; r++)
        fb_text(FB_BODY_X + 4, FB_BODY_Y + 2 + r * FB_BODY_PAD,
                g_fb_body[r], 0xd8e4ee);
}

/* Accumulate characters across con_puts() calls; a line is committed to the
 * scrolling ring when '\n' arrives (or when it exceeds the column width). */
static void fb_body_puts(const char* s) {
    int changed = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '\n') {
            g_fb_cur[g_fb_cur_n] = 0;
            fb_body_push(g_fb_cur);
            g_fb_cur_n = 0;
            changed = 1;
            continue;
        }
        if (c == '\r')
            continue;
        if (g_fb_cur_n >= FB_BODY_COLS) {
            g_fb_cur[g_fb_cur_n] = 0;
            fb_body_push(g_fb_cur);
            g_fb_cur_n = 0;
        }
        g_fb_cur[g_fb_cur_n++] = c;
        changed = 1;
    }
    if (changed)
        fb_body_redraw();
}

/* ------------------------------------------------------------------ */
/* robust GOP discovery (Q8B / MateBook gaokun3 / Ntmer TW220)           */
/* ------------------------------------------------------------------ */
/* Resolution description handed from the loader to the fb console and
 * bootinfo.  width/height are the GOP-reported mode; rot is the verified
 * scanout rotation; scanpx is pixels per physical scanline. */
struct q8b_fb {
    uint64_t base;
    uint32_t width, height;
    uint32_t scanpx;
    uint32_t format;
    uint32_t rot;
    uint32_t masks[4];
};

static int get_memory_map(void);

/* Forward tentative declarations; the authoritative definitions live in the
 * "bootinfo / memory map" section below (C allows repeated static tentative
 * definitions to merge).  The reserved-FB probe reads the map before EBS. */
static EFI_MEMORY_DESCRIPTOR* g_memmap;
static UINTN g_memmap_size;
static UINTN g_memmap_dsize;

/* Documented tablet framebuffer: both the gaokun3 and the TW220 reserve
 * [0xc6200000,0xc8600000) (36 MiB) for scanout, and their first GOP
 * reports FrameBufferBase == 0 (firmware proxy hides the address). */
#define Q8B_RESV_FB_BASE  0xc6200000ULL
#define Q8B_RESV_FB_SIZE  0x02400000ULL   /* 36 MiB */
#define Q8B_RESV_FB_TILE  8u
#define Q8B_RESV_FB_TX    48u
#define Q8B_RESV_FB_TY    64u

static int q8b_mapped_identity(uint64_t base, uint64_t size) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    for (uint64_t p = base; p < base + size; p += 4096) {
        uint64_t par;
        if (el == 8)
            __asm__ volatile("at s1e2r, %0; isb" :: "r"(p) : "memory");
        else
            __asm__ volatile("at s1e1r, %0; isb" :: "r"(p) : "memory");
        __asm__ volatile("mrs %0, par_el1" : "=r"(par));
        if ((par & 1) || ((par ^ p) & 0x000ffffffffff000ULL) != 0)
            return 0;
    }
    return 1;
}

/* Tablets hide the framebuffer address: the first GOP is a 2560x1600 BGR
 * 0xFA0000-byte proxy with FrameBufferBase == 0 (see machines/gaokun3-probe). */
static int q8b_gop_tablet_signature(const EFI_GOP_MODE* m) {
    return m && m->Info && m->FrameBufferBase == 0 &&
           m->Info->HorizontalResolution == 2560 &&
           m->Info->VerticalResolution == 1600 &&
           m->Info->PixelsPerScanLine == 2560 &&
           m->Info->PixelFormat == 1 &&
           m->FrameBufferSize == 0xfa0000;
}

static int q8b_same_pixels(const uint32_t* a, const uint32_t* b) {
    for (unsigned i = 0; i < Q8B_RESV_FB_TILE * Q8B_RESV_FB_TILE; ++i)
        if ((a[i] ^ b[i]) & 0xffffff)   /* ignore alpha */
            return 0;
    return 1;
}

/* Physical pixel offset for an upright (logical) tile coordinate on a
 * layout of mem_w columns / mem_h rows / stride px per scanline. */
static uint64_t q8b_fb_offset(uint32_t rot, uint32_t mem_w, uint32_t mem_h,
                              uint32_t stride, uint32_t x, uint32_t y) {
    uint32_t px, py;
    switch (rot) {
    case 1: px = mem_w - 1 - y; py = x; break;
    case 2: px = mem_w - 1 - x; py = mem_h - 1 - y; break;
    case 3: px = y; py = mem_h - 1 - x; break;
    default: px = x; py = y; break;
    }
    return (uint64_t)py * stride + px;
}

static int q8b_resv_fb_memory_matches(uint32_t rot, uint32_t mem_w,
                                      uint32_t mem_h, uint32_t stride,
                                      const uint32_t* pattern) {
    for (unsigned y = 0; y < Q8B_RESV_FB_TILE; ++y)
        for (unsigned x = 0; x < Q8B_RESV_FB_TILE; ++x) {
            uint64_t off = q8b_fb_offset(rot, mem_w, mem_h, stride,
                                         Q8B_RESV_FB_TX + x, Q8B_RESV_FB_TY + y);
            uint64_t p = Q8B_RESV_FB_BASE + off * 4;
            /* The DPU wrote the tile: clean+invalidate the line so the CPU
             * observes the scanout memory instead of a stale cached copy. */
            __asm__ volatile("dc civac, %0; dsb sy" :: "r"(p) : "memory");
            uint32_t actual = *(volatile uint32_t*)(uintptr_t)p;
            if ((actual ^ pattern[y * Q8B_RESV_FB_TILE + x]) & 0xffffff)
                return 0;
        }
    return 1;
}


static int q8b_gop_adopt(const EFI_GOP_MODE* m, struct q8b_fb* fb) {
    if (!m || !m->Info || m->FrameBufferBase == 0)
        return 0;
    fb->base = m->FrameBufferBase;
    fb->width = m->Info->HorizontalResolution;
    fb->height = m->Info->VerticalResolution;
    fb->scanpx = m->Info->PixelsPerScanLine;
    fb->format = m->Info->PixelFormat;
    fb->rot = 0;
    fb->masks[0] = m->Info->PixelInformation[0];
    fb->masks[1] = m->Info->PixelInformation[1];
    fb->masks[2] = m->Info->PixelInformation[2];
    fb->masks[3] = m->Info->PixelInformation[3];
    return fb->base != 0 && fb->width >= 320 && fb->height >= 120 &&
           fb->scanpx >= fb->width && fb->format <= 2;
}

/* Two-pattern BLT verification of the documented tablet reserved
 * framebuffer, then rotation detection by physical read-back.  Never
 * writes unknown memory: every write/restore goes through GOP Blt. */
static int q8b_resv_fb_verify(EFI_GRAPHICS_OUTPUT_PROTOCOL* g,
                              EFI_GOP_MODE* m, struct q8b_fb* fb) {
    if (get_memory_map() != 0)
        return 0;
    int reserved = 0;
    for (uint64_t off = 0; off < g_memmap_size; off += g_memmap_dsize) {
        const EFI_MEMORY_DESCRIPTOR* d = (const void*)((uint8_t*)g_memmap + off);
        if (d->Type == EfiReservedMemoryType &&
            d->PhysicalStart == Q8B_RESV_FB_BASE &&
            d->NumberOfPages * EFI_PAGE_SIZE == Q8B_RESV_FB_SIZE &&
            !(d->Attribute & 0x2000))
            reserved = 1;
    }
    if (!reserved)
        return 0;

    uint64_t bytes = (uint64_t)m->Info->HorizontalResolution *
                     m->Info->VerticalResolution * 4;
    if (Q8B_RESV_FB_BASE & 4095 || bytes == 0 || bytes > Q8B_RESV_FB_SIZE)
        return 0;
    if (!q8b_mapped_identity(Q8B_RESV_FB_BASE, bytes))
        return 0;

    uint32_t original[Q8B_RESV_FB_TILE * Q8B_RESV_FB_TILE];
    uint32_t pattern[Q8B_RESV_FB_TILE * Q8B_RESV_FB_TILE];
    uint32_t readback[Q8B_RESV_FB_TILE * Q8B_RESV_FB_TILE];
    EFI_STATUS status = g->Blt(g, original, EFI_GOP_BLT_VIDEO_TO_BLT_BUFFER,
                               Q8B_RESV_FB_TX, Q8B_RESV_FB_TY,
                               0, 0, Q8B_RESV_FB_TILE, Q8B_RESV_FB_TILE, 0);
    if (EFI_ERROR(status))
        return 0;
    unsigned matches = 15;
    for (unsigned pass = 0; pass < 2 && matches; ++pass) {
        for (unsigned y = 0; y < Q8B_RESV_FB_TILE; ++y)
            for (unsigned x = 0; x < Q8B_RESV_FB_TILE; ++x)
                pattern[y * Q8B_RESV_FB_TILE + x] =
                    ((31 + x * 23 + pass * 41) << 16) |
                    ((47 + y * 19 + pass * 37) << 8) |
                    (13 + x * 7 + y * 11 + pass * 53);
        status = g->Blt(g, pattern, EFI_GOP_BLT_BUFFER_TO_VIDEO,
                        0, 0, Q8B_RESV_FB_TX, Q8B_RESV_FB_TY,
                        Q8B_RESV_FB_TILE, Q8B_RESV_FB_TILE, 0);
        if (EFI_ERROR(status)) {
            matches = 0;
            break;
        }
        gBS->Stall(20000);
        status = g->Blt(g, readback, EFI_GOP_BLT_VIDEO_TO_BLT_BUFFER,
                        Q8B_RESV_FB_TX, Q8B_RESV_FB_TY,
                        0, 0, Q8B_RESV_FB_TILE, Q8B_RESV_FB_TILE, 0);
        if (EFI_ERROR(status) || !q8b_same_pixels(pattern, readback)) {
            matches = 0;
            break;
        }
        for (unsigned r = 0; r < 4; ++r) {
            if (!(matches & (1u << r)))
                continue;
            uint32_t mem_w = (r & 1) ? m->Info->VerticalResolution
                                     : m->Info->HorizontalResolution;
            uint32_t mem_h = (r & 1) ? m->Info->HorizontalResolution
                                     : m->Info->VerticalResolution;
            uint32_t stride = (r & 1) ? m->Info->VerticalResolution
                                      : m->Info->HorizontalResolution;
            if (!q8b_resv_fb_memory_matches(r, mem_w, mem_h, stride, pattern))
                matches &= ~(1u << r);
        }
    }
    /* No console output between capture and restore: console scrolling can
     * change the challenge tile.  Restore even after a failed test. */
    status = g->Blt(g, original, EFI_GOP_BLT_BUFFER_TO_VIDEO,
                    0, 0, Q8B_RESV_FB_TX, Q8B_RESV_FB_TY,
                    Q8B_RESV_FB_TILE, Q8B_RESV_FB_TILE, 0);
    if (EFI_ERROR(status) || !matches || (matches & (matches - 1)))
        return 0;

    /* matches[r] only told us which scan-out mapping produced the pattern;
     * it is used to derive the REAL buffer layout (columns x rows x stride),
     * not to rotate anything.  The loader reports the raw physical geometry
     * and leaves presentation rotation to user space. */
    unsigned match_rot = 0;
    for (unsigned r = 0; r < 4; ++r)
        if (matches == (1u << r))
            match_rot = r;
    fb->base = Q8B_RESV_FB_BASE;
    fb->scanpx = (match_rot & 1) ? m->Info->VerticalResolution
                                 : m->Info->HorizontalResolution;
    fb->width = fb->scanpx;                 /* physical columns  */
    fb->height = (match_rot & 1) ? m->Info->HorizontalResolution
                                 : m->Info->VerticalResolution; /* rows */
    fb->rot = 0;                            /* no rotation applied */
    fb->format = m->Info->PixelFormat;
    fb->masks[0] = m->Info->PixelInformation[0];
    fb->masks[1] = m->Info->PixelInformation[1];
    fb->masks[2] = m->Info->PixelInformation[2];
    fb->masks[3] = m->Info->PixelInformation[3];
    return 1;
}

/* Find a usable linear framebuffer on Q8B (base != 0 GOP) and on the
 * tablets (base-0 proxy, reserved FB verified via BLT).  Every GOP
 * instance is logged so a platform that matches none of these still
 * leaves a diagnostic trail. */
static int q8b_gop_discover(struct q8b_fb* fb) {
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    struct q8b_fb best;
    int have_best = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* tablet = 0;
    memset(&best, 0, sizeof(best));

    /* 1) LocateProtocol: cheapest; on Q8B this is the real GOP. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = 0;
    EFI_STATUS status = gBS->LocateProtocol((EFI_GUID*)&gop_guid, 0,
                                            (void**)&gop);
    if (!EFI_ERROR(status) && gop) {
        if (q8b_gop_tablet_signature(gop->Mode)) {
            tablet = gop;
        } else if (q8b_gop_adopt(gop->Mode, &best)) {
            have_best = 1;
        }
    }

    /* 2) Console device's GOP: the tablet console works while the first
     *    instance is a base-0 proxy (verified gaokun3 behaviour). */
    if (gST->ConsoleOutHandle) {
        gop = 0;
        status = gBS->HandleProtocol(gST->ConsoleOutHandle, (EFI_GUID*)&gop_guid,
                                     (void**)&gop);
        if (!EFI_ERROR(status) && gop) {
            if (q8b_gop_tablet_signature(gop->Mode)) {
                tablet = gop;
            } else if (!have_best && q8b_gop_adopt(gop->Mode, &best)) {
                have_best = 1;
            }
        }
    }

    /* 3) Enumerate every installed GOP instance. */
    UINTN count = 0;
    EFI_HANDLE* handles = 0;
    status = gBS->LocateHandleBuffer(2 /* ByProtocol */, (EFI_GUID*)&gop_guid,
                                     0, &count, &handles);
    if (!EFI_ERROR(status) && handles && count) {
        for (UINTN i = 0; i < count; i++) {
            gop = 0;
            if (EFI_ERROR(gBS->HandleProtocol(handles[i], (EFI_GUID*)&gop_guid,
                                              (void**)&gop)) || !gop)
                continue;
            if (q8b_gop_tablet_signature(gop->Mode)) {
                tablet = gop;
            } else if (!have_best && q8b_gop_adopt(gop->Mode, &best)) {
                have_best = 1;
            }
        }
        gBS->FreePool(handles);
    }

    if (have_best) {
        *fb = best;
        return 1;
    }
    if (tablet && q8b_resv_fb_verify(tablet, tablet->Mode, fb))
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* flat device-tree model string (platform identification)            */
/* ------------------------------------------------------------------ */
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

/* Copy the root "/model" property from the firmware DTB if present. */
static void fdt_model(uint64_t dtb, char* out, uint64_t cap) {
    out[0] = 0;
    if (!dtb || !cap)
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
    while (p + 4 <= totalsize - off_struct) {
        uint32_t tok = fdt_be32(st + p);
        p += 4;
        if (tok == 1) {                       /* FDT_BEGIN_NODE */
            uint32_t q = p;
            while (q + 1 < totalsize - off_struct && st[q] != 0)
                q++;
            p = q + 1;
            p = (p + 3) & ~3u;
        } else if (tok == 3) {                /* FDT_PROP */
            if (p + 8 > totalsize - off_struct)
                break;
            uint32_t len = fdt_be32(st + p);
            uint32_t nameoff = fdt_be32(st + p + 4);
            uint64_t data = p + 8;
            p = (uint32_t)(data + len);
            p = (p + 3) & ~3u;
            if (nameoff < size_str && strblk[nameoff] &&
                    fdt_str_eq((const char*)(strblk + nameoff), "model")) {
                uint64_t n = len;
                if (n > 0 && st[data + n - 1] == 0)
                    n--;
                if (n >= cap)
                    n = cap - 1;
                memcpy(out, st + data, n);
                out[n] = 0;
                for (uint64_t i = 0; i < n; i++)
                    if ((uint8_t)out[i] < 0x20 || (uint8_t)out[i] > 0x7e)
                        out[i] = ' ';
                return;
            }
        } else if (tok == 2) {                /* FDT_END_NODE */
            continue;
        } else {                              /* FDT_END / garbage */
            break;
        }
    }
}

/* Product name from the firmware's SMBIOS Type-1 (System Information)
 * record: the canonical device model (e.g. "MateBook gaokun3", "TW220").
 * Used when the firmware DTB has no /model. */
static void smbios_product_name(char* out, uint64_t cap) {
    static const EFI_GUID smbios_guid = EFI_SMBIOS_PROTOCOL_GUID;
    EFI_SMBIOS_PROTOCOL* smb = 0;
    out[0] = 0;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&smbios_guid, 0,
                    (void**)&smb)) || !smb || !smb->GetNext)
        return;
    uint8_t type = 1;              /* SMBIOS_TYPE_SYSTEM_INFORMATION */
    void* rec = 0;
    if (EFI_ERROR(smb->GetNext(smb, &type, &rec, 0)) || !rec)
        return;
    const uint8_t* r = (const uint8_t*)rec;
    uint8_t len = r[1];
    uint8_t idx = (len > 5) ? r[5] : 0;   /* Product Name string index */
    if (idx == 0)
        return;
    const char* p = (const char*)(r + len);   /* SMBIOS string set */
    for (uint8_t i = 1; i < idx && *p; i++) {
        while (*p)
            p++;
        p++;
    }
    if (*p == 0)
        return;
    uint64_t n = 0;
    for (; p[n] && n + 1 < cap; n++)
        out[n] = ((uint8_t)p[n] < 0x20 || (uint8_t)p[n] > 0x7e) ? ' ' : p[n];
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
static int guid_eq(const EFI_GUID* a, const EFI_GUID* b) {
    return memcmp_(a, b, sizeof(EFI_GUID)) == 0;
}

static EFI_PHYSICAL_ADDRESS alloc_pages_at(EFI_PHYSICAL_ADDRESS at, UINTN pages) {
    EFI_PHYSICAL_ADDRESS p = at;
    EFI_STATUS st = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
            pages, &p);
    if (EFI_ERROR(st))
        return 0;
    return p;
}

static EFI_PHYSICAL_ADDRESS alloc_pages(UINTN pages) {
    EFI_PHYSICAL_ADDRESS p = 0;
    int type = EfiLoaderData;
    int mode = AllocateAnyPages;
    if (g_alloc_high) {
        /* AllocateAddress is deterministic on the Q8B firmware.  The
         * 6-GiB slot is inside the second DRAM bank and outside DMA32; if a
         * future firmware reserves it, fall back to AllocateMaxAddress. */
        p = 0x180000000ULL;
        EFI_STATUS high_st = gBS->AllocatePages(AllocateAddress, type,
                pages, &p);
        if (!EFI_ERROR(high_st))
            return p;
        /* A failed probe of an absent RAMFS can leave the firmware's
         * conversion bookkeeping holding the first slot.  Try disjoint
         * high-address slots before falling back below 4 GiB. */
        p = 0x1a0000000ULL;
        high_st = gBS->AllocatePages(AllocateAddress, type, pages, &p);
        if (!EFI_ERROR(high_st))
            return p;
        p = 0x1c0000000ULL;
        high_st = gBS->AllocatePages(AllocateAddress, type, pages, &p);
        if (!EFI_ERROR(high_st))
            return p;
        p = (Q8B_RAM_BASE + Q8B_USABLE_LIMIT - EFI_PAGE_SIZE) &
                ~(EFI_PAGE_SIZE - 1);
        mode = AllocateMaxAddress;
    }
    EFI_STATUS st = gBS->AllocatePages(mode, type, pages, &p);
    if (EFI_ERROR(st))
        return 0;
    return p;
}

static CHAR16* ascii_to_w(const char* s, CHAR16* out, uint64_t cap) {
    uint64_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        out[i] = (CHAR16)(uint8_t)s[i];
    out[i] = 0;
    return out;
}

/* Query the size of a file via EFI_LOAD_FILE / LOAD_FILE2. */
static EFI_STATUS load_file_query(void* lf, EFI_DEVICE_PATH_PROTOCOL* dp,
                                  UINTN* size) {
    EFI_LOAD_FILE_PROTOCOL* p = (EFI_LOAD_FILE_PROTOCOL*)lf;
    *size = 0;
    return p->LoadFile(lf, dp, 0, size, 0);
}

static void* load_file_read(void* lf, EFI_DEVICE_PATH_PROTOCOL* dp,
                            uint64_t* out_size) {
    UINTN size = 0;
    EFI_STATUS st = load_file_query(lf, dp, &size);

    if (st != EFI_BUFFER_TOO_SMALL || size == 0) {
        /* Some implementations do not report the size on a NULL buffer;
         * fall back to a generous cap and let the protocol shrink it. */
        size = (UINTN)(256ULL * 1024 * 1024);
        if (st == EFI_SUCCESS || st == EFI_INVALID_PARAMETER ||
                st == EFI_BUFFER_TOO_SMALL)
            ;
        else
            return 0;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        UINTN alloc = size;
        UINTN pages = (alloc + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
        EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
        if (buf == 0)
            return 0;
        uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
        UINTN use = alloc;
        EFI_LOAD_FILE_PROTOCOL* p = (EFI_LOAD_FILE_PROTOCOL*)lf;
        st = p->LoadFile(lf, dp, 0, &use, (void*)(uintptr_t)abuf);
        if (!EFI_ERROR(st)) {
            *out_size = use;
            return (void*)(uintptr_t)abuf;
        }
        if (st == EFI_BUFFER_TOO_SMALL && use > alloc) {
            size = use;   /* grow and retry */
            continue;
        }
        return 0;
    }
    return 0;
}

/* Build a one-node FilePath device path ("\q8b\KERNEL.BIN"). */
static FILEPATH_DEVICE_PATH* make_filepath(const char* path, CHAR16* storage,
                                           uint64_t cap) {
    FILEPATH_DEVICE_PATH* fp = (FILEPATH_DEVICE_PATH*)storage;
    uint64_t n = strlen_(path);
    if (n + 1 > cap / 2)
        return 0;
    fp->Header.Type = 4;
    fp->Header.SubType = 4;
    fp->Header.Length = (uint16_t)(sizeof(EFI_DEVICE_PATH_PROTOCOL) +
            (n + 1) * 2);
    ascii_to_w(path, fp->PathName, cap / 2);
    return fp;
}

/* Device-path classification lets the loader preserve the source that
 * launched BOOTAA64.EFI, then use the board's deterministic fallback order.
 * A partition handle carries its complete parent path, so this works for an
 * ESP on SD, USB, or NVMe without relying on controller-specific handles. */
static uint32_t device_source(EFI_HANDLE handle) {
    static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL* dp = 0;
    uint32_t source = Q8B_BOOT_SOURCE_UNKNOWN;
    if (handle == 0 || EFI_ERROR(gBS->HandleProtocol(handle, &dp_guid,
                    (void**)&dp)) || dp == 0)
        return source;

    for (int guard = 0; guard < 128 && dp->Length >= 4; guard++) {
        if (dp->Type == 0x7f)
            break;
        if (dp->Type == 3) { /* messaging device path */
            switch (dp->SubType) {
            case 0x05: /* USB */
                source = Q8B_BOOT_SOURCE_USB;
                break;
            case 0x17: /* NVMe */
                source = Q8B_BOOT_SOURCE_NVME;
                break;
            case 0x1a: /* SD */
                source = Q8B_BOOT_SOURCE_SD;
                break;
            case 0x0c: /* IPv4 */
            case 0x0d: /* IPv6 */
                source = Q8B_BOOT_SOURCE_NET;
                break;
            default:
                break;
            }
        }
        dp = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)dp + dp->Length);
    }
    return source;
}

static const char* source_name(uint32_t source) {
    switch (source) {
    case Q8B_BOOT_SOURCE_SD: return "sd";
    case Q8B_BOOT_SOURCE_USB: return "usb";
    case Q8B_BOOT_SOURCE_NVME: return "nvme";
    case Q8B_BOOT_SOURCE_NET: return "net";
    default: return "unknown";
    }
}

static int source_rank(uint32_t source) {
    switch (source) {
    case Q8B_BOOT_SOURCE_SD: return 0;
    case Q8B_BOOT_SOURCE_USB: return 1;
    case Q8B_BOOT_SOURCE_NVME: return 2;
    case Q8B_BOOT_SOURCE_UNKNOWN: return 3;
    default: return 4;
    }
}

static void* read_file_sfs_handle(EFI_HANDLE handle, const char* path,
                                  uint64_t* out_size) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    CHAR16 wpath[256];
    ascii_to_w(path, wpath, sizeof(wpath) / sizeof(wpath[0]));
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = 0;
    if (handle == 0 || EFI_ERROR(gBS->HandleProtocol(handle, &sfs_guid,
                    (void**)&sfs)) || sfs == 0)
        return 0;
    EFI_FILE_PROTOCOL* root = 0;
    if (EFI_ERROR(sfs->OpenVolume(sfs, (void**)&root)) || root == 0)
        return 0;
    EFI_FILE_PROTOCOL* f = 0;
    if (EFI_ERROR(root->Open(root, &f, wpath, EFI_FILE_MODE_READ, 0)) || f == 0)
        return 0;

    static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    uint8_t info[256];
    UINTN info_size = sizeof(info);
    if (EFI_ERROR(f->GetInfo(f, &fi_guid, &info_size, info))) {
        f->Close(f);
        return 0;
    }
    uint64_t fsize = ((EFI_FILE_INFO_HEAD*)info)->FileSize;

    /* The Q8B kernel uses 16KB pages; every physical range it maps through
     * the page tables must be 16KB aligned, while UEFI only guarantees 4KB. */
    UINTN pages = (fsize + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
    if (buf == 0) {
        f->Close(f);
        return 0;
    }
    uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
    UINTN rsz = fsize;
    EFI_STATUS st = f->Read(f, &rsz, (void*)(uintptr_t)abuf);
    f->Close(f);
    if (EFI_ERROR(st) || rsz != fsize)
        return 0;
    *out_size = fsize;
    return (void*)(uintptr_t)abuf;
}

/* SFS: current image volume first, then SD -> USB -> NVMe -> unknown. */
static void* read_file_sfs(const char* path, uint64_t* out_size) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE* handles = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &sfs_guid, 0, &n, &handles)))
        return 0;

    EFI_HANDLE current = gImageInfo ? gImageInfo->DeviceHandle : 0;
    if (current != 0) {
        void* p = read_file_sfs_handle(current, path, out_size);
        if (p) {
            con_puts("[uefi]   self SFS (");
            con_puts(source_name(g_boot_source));
            con_puts(") ok, ");
            con_dec(*out_size);
            con_puts(" bytes\n");
            gBS->FreePool(handles);
            return p;
        }
    }

    for (int wanted = 0; wanted <= 3; wanted++) {
        for (UINTN i = 0; i < n; i++) {
            EFI_HANDLE h = handles[i];
            if (h == current || source_rank(device_source(h)) != wanted)
                continue;
            void* p = read_file_sfs_handle(h, path, out_size);
            if (p) {
                con_puts("[uefi]   SFS ");
                con_puts(source_name(device_source(h)));
                con_puts(" ok, ");
                con_dec(*out_size);
                con_puts(" bytes\n");
                gBS->FreePool(handles);
                return p;
            }
        }
    }
    gBS->FreePool(handles);
    return 0;
}

/* Ask UEFI to connect storage drivers before looking for a local filesystem.
 * In the PXE path the BDS may never connect the external SD controller, even
 * though the same card is available when SD is selected as the boot device. */
static int connect_blockio_controllers(void) {
    static EFI_GUID blockio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE* handles = 0;
    int connected = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &blockio_guid, 0, &n,
                    &handles)))
        return 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_STATUS st = gBS->ConnectController(handles[i], 0, 0, 1);
        if (!EFI_ERROR(st))
            connected++;
    }
    return connected;
}

/* Touch the local EFI filesystem once before ExitBootServices.  A PXE-first
 * boot normally never asks UEFI's SD BlockIo/SFS stack to access the card,
 * which leaves SDCC2 in a state the post-EBS kernel probe cannot recover
 * from on this firmware.  This is deliberately read-only and independent of
 * the PXE kernel buffer: it only opens and closes each local SFS volume. */
static int probe_sfs_media(void) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE* handles = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &sfs_guid, 0, &n, &handles)))
        return -1;

    int opened = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &sfs_guid,
                        (void**)&sfs)) || sfs == 0)
            continue;

        EFI_FILE_PROTOCOL* root = 0;
        if (EFI_ERROR(sfs->OpenVolume(sfs, (void**)&root)) || root == 0)
            continue;
        opened = 1;

        /* OpenVolume is enough to make the firmware bind and initialize its
         * BlockIo media.  Do not call root->GetInfo/root->Open here: this
         * board's DXE has a faulty root-directory path on some SFS handles
         * and can fault before returning an EFI_STATUS. */
        root->Close(root);
    }
    return opened ? 0 : -1;
}

/* One LoadFile handle + a FilePath node. */
static void* try_loadfile_handle(EFI_HANDLE handle, const char* path,
                                 uint64_t* out_size) {
    static EFI_GUID lf_guid = EFI_LOAD_FILE_PROTOCOL_GUID;
    EFI_LOAD_FILE_PROTOCOL* lf = 0;

    if (handle == 0)
        return 0;
    if (EFI_ERROR(gBS->HandleProtocol(handle, &lf_guid, (void**)&lf)))
        return 0;

    CHAR16 wstorage[512];
    FILEPATH_DEVICE_PATH* dp = make_filepath(path, wstorage,
            sizeof(wstorage));
    if (dp == 0)
        return 0;

    return load_file_read(lf, &dp->Header, out_size);
}

/* PXE: EFI_LOAD_FILE.  Try the loaded image's device first, then every
 * handle that publishes the protocol.  On a direct PXE NBP this is the NIC;
 * under GRUB chainload the loaded-image handle may be a virtual file handle,
 * so the all-handles fallback is what keeps the network path alive. */
static void* read_file_loadfile(const char* path, uint64_t* out_size) {
    static EFI_GUID lf_guid = EFI_LOAD_FILE_PROTOCOL_GUID;

    if (gImageInfo && gImageInfo->DeviceHandle) {
        void* p = try_loadfile_handle(gImageInfo->DeviceHandle, path,
                out_size);
        if (p)
            return p;
    }

    UINTN n = 0;
    EFI_HANDLE* handles = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &lf_guid, 0, &n, &handles)))
        return 0;
    for (UINTN i = 0; i < n; i++) {
        void* p = try_loadfile_handle(handles[i], path, out_size);
        if (p)
            return p;
    }
    return 0;
}

/* GRUB/systemd-boot initrd exposed through the Linux EFI LoadFile2 media.
 * Follow Linux's efi_load_initrd_dev_path(): build the vendor-media device
 * path, LocateDevicePath() it, then issue LoadFile2 on the resulting handle. */
static void* read_initrd_loadfile2(uint64_t* out_size) {
    static const EFI_GUID lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    static const struct {
        VENDOR_DEVICE_PATH vendor;
        EFI_DEVICE_PATH_PROTOCOL end;
    } __attribute__((packed)) dp = {
        { { 4, 6, sizeof(VENDOR_DEVICE_PATH) }, LINUX_EFI_INITRD_MEDIA_GUID },
        { 0x7F, 0xFF, 4 }
    };

    EFI_DEVICE_PATH_PROTOCOL* dpwalk =
        (EFI_DEVICE_PATH_PROTOCOL*)(uintptr_t)&dp;
    EFI_HANDLE handle = 0;
    EFI_LOAD_FILE2_PROTOCOL* lf2 = 0;

    if (EFI_ERROR(gBS->LocateDevicePath((EFI_GUID*)&lf2_guid, &dpwalk,
                    &handle)))
        return 0;
    if (EFI_ERROR(gBS->HandleProtocol(handle, (EFI_GUID*)&lf2_guid,
                    (void**)&lf2)))
        return 0;

    return load_file_read(lf2, (EFI_DEVICE_PATH_PROTOCOL*)(uintptr_t)&dp,
            out_size);
}

/*
 * Return one of the candidate paths as a literal.  Deliberately not a
 * static array of pointers: a non-PIE link would store those as absolute
 * addresses in .data, which the position-independent PE loader cannot
 * relocate (we emit an empty .reloc by design).
 */
static const char* path_name(int which, int i) {
    if (which == 0) {           /* kernel */
        switch (i) {
        case 0: return "\\q8b\\KERNEL.BIN";
        case 1: return "\\KERNEL.BIN";
        case 2: return "\\EFI\\BOOT\\KERNEL.BIN";
        }
    } else if (which == 1) {    /* legacy full rootfs */
        switch (i) {
        case 0: return "\\q8b\\ROOTFS.IMG";
        case 1: return "\\ROOTFS.IMG";
        case 2: return "\\EFI\\BOOT\\ROOTFS.IMG";
        }
    } else {                    /* boot-only ramfs */
        switch (i) {
        case 0: return "\\q8b\\INITRAMFS.IMG";
        case 1: return "\\INITRAMFS.IMG";
        case 2: return "\\EFI\\BOOT\\INITRAMFS.IMG";
        }
    }
    return 0;
}

/* Fetch a file with the firmware's EFI_MTFTP4 protocol.  Unlike the PXE
 * LoadFile/PXE-BC Mtftp paths on this firmware, MTFTP4 takes a plain CHAR8
 * filename and is the reliable way to pull extra files in a PXE NBP. */
static void* read_file_mtftp4(const char* path, uint64_t max_size,
                              uint64_t* out_size) {
    static EFI_GUID mt_guid = EFI_MTFTP4_PROTOCOL_GUID;
    EFI_MTFTP4_PROTOCOL* mt = 0;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&mt_guid, 0,
                    (void**)&mt)) || mt == 0) {
        con_puts("[uefi] no MTFTP4 protocol\n");
        return 0;
    }

    EFI_MTFTP4_CONFIG_DATA cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.UseDefaultSetting = 0;
    cfg.StationIp.Addr[0] = 192;
    cfg.StationIp.Addr[1] = 168;
    cfg.StationIp.Addr[2] = 8;
    cfg.StationIp.Addr[3] = 50;
    cfg.SubnetMask.Addr[0] = 255;
    cfg.SubnetMask.Addr[1] = 255;
    cfg.SubnetMask.Addr[2] = 255;
    cfg.SubnetMask.Addr[3] = 0;
    cfg.GatewayIp.Addr[0] = 192;
    cfg.GatewayIp.Addr[1] = 168;
    cfg.GatewayIp.Addr[2] = 8;
    cfg.GatewayIp.Addr[3] = 1;
    cfg.ServerIp.Addr[0] = 192;
    cfg.ServerIp.Addr[1] = 168;
    cfg.ServerIp.Addr[2] = 8;
    cfg.ServerIp.Addr[3] = 1;
    cfg.LocalPort = 0;
    cfg.InitialServerPort = 69;
    cfg.TryCount = 3;
    cfg.TimeoutValue = 3;
    EFI_STATUS st = mt->Configure(mt, &cfg);
    con_puts("[uefi] mtftp4 cfg ");
    con_puts(path);
    con_puts(" st=");
    con_hex(st);
    con_puts("\n");

    UINTN pages = (max_size + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
    if (buf == 0)
        return 0;
    uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;

    EFI_MTFTP4_TOKEN tok;
    memset(&tok, 0, sizeof(tok));
    tok.Status = EFI_NOT_READY;
    tok.Event = 0;                 /* NULL: ReadFile waits synchronously */
    tok.Filename = (CHAR8*)path;
    tok.ModeStr = (CHAR8*)"octet";
    tok.BufferSize = max_size;
    tok.Buffer = (void*)(uintptr_t)abuf;
    tok.Context = 0;

    /* Keep the firmware's default 512-byte block.  A correctly formed
     * blksize option was measured slower on this EDK2 MTFTP4 DXE. */
    tok.OptionCount = 0;
    tok.OptionList = 0;

    st = mt->ReadFile(mt, &tok);
    con_puts("[uefi] mtftp4 read ");
    con_puts(path);
    con_puts(" st=");
    con_hex(st);
    con_puts(" status=");
    con_hex(tok.Status);
    con_puts(" n=");
    con_dec(tok.BufferSize);
    con_puts("\n");
    if (EFI_ERROR(tok.Status) || tok.BufferSize == 0)
        return 0;
    *out_size = tok.BufferSize;
    return (void*)(uintptr_t)abuf;
}

/* A TFTP path (forward slashes) for the same candidate file. */
static const char* pxe_path_name(int which, int i) {
    if (which == 0) {
        switch (i) {
        case 0: return "q8b/KERNEL.BIN";
        case 1: return "KERNEL.BIN";
        case 2: return "EFI/BOOT/KERNEL.BIN";
        }
    } else if (which == 1) {
        switch (i) {
        case 0: return "q8b/ROOTFS.IMG";
        case 1: return "ROOTFS.IMG";
        case 2: return "EFI/BOOT/ROOTFS.IMG";
        }
    } else {
        switch (i) {
        case 0: return "q8b/INITRAMFS.IMG";
        case 1: return "INITRAMFS.IMG";
        case 2: return "EFI/BOOT/INITRAMFS.IMG";
        }
    }
    return 0;
}

/* Fetch a file with the firmware's PXE base code.  The direct PXE NBP's
 * LoadedImage handle only knows the NBP itself on this firmware, so the
 * network files have to go through EFI_PXE_BASE_CODE_PROTOCOL::Mtftp. */
static void* read_file_pxe(const char* path, uint64_t* out_size) {
    static EFI_GUID pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    EFI_PXE_BASE_CODE_PROTOCOL* pxe = 0;
    EFI_IP_ADDRESS server;
    CHAR16 wpath[256];
    uint64_t fsize = 0;

    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&pxe_guid, 0,
                    (void**)&pxe)) || pxe == 0) {
        con_puts("[uefi] no PXE base code protocol\n");
        return 0;
    }

    memset(&server, 0, sizeof(server));
    server.Addr[0] = 192;
    server.Addr[1] = 168;
    server.Addr[2] = 8;
    server.Addr[3] = 1;
    ascii_to_w(path, wpath, sizeof(wpath) / sizeof(wpath[0]));

    EFI_STATUS st = pxe->Mtftp(pxe, EFI_PXE_BASE_CODE_TFTP_GET_FILE_SIZE,
            0, 0, &fsize, 0, &server, wpath, 0);
    con_puts("[uefi] pxe size ");
    con_puts(path);
    con_puts(" st=");
    con_hex(st);
    con_puts(" n=");
    con_dec(fsize);
    con_puts("\n");
    if (EFI_ERROR(st) || fsize == 0)
        return 0;

    UINTN pages = (fsize + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
    if (buf == 0)
        return 0;
    uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
    uint64_t bsize = fsize;
    st = pxe->Mtftp(pxe, EFI_PXE_BASE_CODE_TFTP_READ_FILE,
            (void*)(uintptr_t)abuf, 0, &bsize, 1468, &server, wpath, 0);
    con_puts("[uefi] pxe read ");
    con_puts(path);
    con_puts(" st=");
    con_hex(st);
    con_puts(" n=");
    con_dec(bsize);
    con_puts("\n");
    if (EFI_ERROR(st))
        return 0;
    *out_size = bsize;
    return (void*)(uintptr_t)abuf;
}

/* ------------------------------------------------------------------ */
/* Fast TFTP client over the PXE base-code UDP interface.              */
/*                                                                     */
/* The firmware's EFI_MTFTP4 DXE moves one 512-byte block per ACK and   */
/* tops out around 100-250KB/s on this board, which makes a 112MB       */
/* RAM-disk rootfs impractical.  The PXE UDP path below speaks TFTP     */
/* directly with blksize=1468 and windowsize=16 (dnsmasq 2.93 supports  */
/* both), so a window is ACKed once instead of every packet.            */
/* ------------------------------------------------------------------ */
#ifndef Q8B_TFTP_FAST
#define Q8B_TFTP_FAST 0
#endif

#define TFTP_RRQ   1
#define TFTP_DATA  3
#define TFTP_ACK   4
#define TFTP_ERROR 5
#define TFTP_OACK  6

static uint16_t be16_(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16_(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint8_t* s0_(uint8_t* p, const char* s) {
    while (*s)
        *p++ = (uint8_t)*s++;
    *p++ = 0;
    return p;
}

static int str_eq_(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t dec_(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10U + (uint32_t)(*s++ - '0');
    return v;
}

static EFI_PXE_BASE_CODE_PROTOCOL* pxe_get(void) {
    static EFI_GUID guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    void* p = 0;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&guid, 0, &p)) || p == 0)
        return 0;
    return (EFI_PXE_BASE_CODE_PROTOCOL*)p;
}

static int tftp_send(EFI_PXE_BASE_CODE_PROTOCOL* pxe, const uint8_t* data,
                     uint32_t len, EFI_IP_ADDRESS* dst, uint16_t dst_port,
                     uint16_t* src_port) {
    UINTN blen = len;
    EFI_STATUS st = pxe->UdpWrite(pxe, 0, dst, &dst_port, 0, 0, src_port,
            0, 0, &blen, (void*)(uintptr_t)data);
    return EFI_ERROR(st) ? -1 : 0;
}

static int tftp_recv(EFI_PXE_BASE_CODE_PROTOCOL* pxe, uint8_t* buf,
                     uint32_t cap, uint32_t* out_len, EFI_IP_ADDRESS* src,
                     uint16_t* src_port, int any_src_port) {
    UINTN len = cap;
    uint16_t flags = EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_DEST_IP |
            EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_DEST_PORT;
    if (any_src_port)
        flags |= EFI_PXE_BASE_CODE_UDP_OPFLAGS_ANY_SRC_PORT;
    EFI_STATUS st = pxe->UdpRead(pxe, flags, 0, 0, src, src_port,
            0, 0, &len, buf);
    if (EFI_ERROR(st))
        return -1;
    *out_len = (uint32_t)len;
    return 0;
}

static int tftp_ack(EFI_PXE_BASE_CODE_PROTOCOL* pxe, EFI_IP_ADDRESS* server,
                    uint16_t server_port, uint16_t* local_port,
                    uint16_t block) {
    uint8_t ack[4];
    wr16_(ack, TFTP_ACK);
    wr16_(ack + 2, block);
    return tftp_send(pxe, ack, sizeof(ack), server, server_port, local_port);
}

static void* read_file_pxe_udp(const char* path, uint64_t max_size,
                               uint64_t* out_size) {
#if Q8B_TFTP_FAST
    EFI_PXE_BASE_CODE_PROTOCOL* pxe = pxe_get();
    if (pxe == 0 || pxe->UdpWrite == 0 || pxe->UdpRead == 0)
        return 0;

    /* A PXE NBP normally inherits a started base code, but call Start and
     * pin the station address so UdpWrite does not block on an unconfigured
     * or stale access point. */
    {
        EFI_STATUS st = ((EFI_STATUS (*)(void*, BOOLEAN))pxe->Start)(pxe, 0);
        con_puts("[uefi] udp tftp start st=");
        con_hex(st);
        con_puts("\n");
        EFI_IP_ADDRESS ip;
        EFI_IP_ADDRESS mask;
        memset(&ip, 0, sizeof(ip));
        memset(&mask, 0, sizeof(mask));
        ip.Addr[0] = 192; ip.Addr[1] = 168; ip.Addr[2] = 8; ip.Addr[3] = 50;
        mask.Addr[0] = 255; mask.Addr[1] = 255; mask.Addr[2] = 255;
        mask.Addr[3] = 0;
        st = pxe->SetStationIp(pxe, &ip, &mask);
        con_puts("[uefi] udp tftp setip st=");
        con_hex(st);
        con_puts("\n");
        BOOLEAN autoarp = 1;
        st = pxe->SetParameters(pxe, &autoarp, 0, 0, 0, 0);
        con_puts("[uefi] udp tftp arp st=");
        con_hex(st);
        con_puts("\n");
    }

    UINTN pages = (max_size + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
    if (buf == 0)
        return 0;
    uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
    uint8_t* out = (uint8_t*)(uintptr_t)abuf;

    EFI_IP_ADDRESS server;
    memset(&server, 0, sizeof(server));
    server.Addr[0] = 192;
    server.Addr[1] = 168;
    server.Addr[2] = 8;
    server.Addr[3] = 1;

    uint8_t pkt[2048];
    uint8_t* q = pkt;
    wr16_(q, TFTP_RRQ);
    q += 2;
    q = s0_(q, path);
    q = s0_(q, "octet");
    q = s0_(q, "blksize");
    q = s0_(q, "1468");
    q = s0_(q, "windowsize");
    q = s0_(q, "16");
    q = s0_(q, "tsize");
    q = s0_(q, "0");

    uint16_t local_port = 0x4d2;   /* 1234 */
    uint16_t server_port = 69;
    {
        EFI_STATUS st;
        UINTN blen = (UINTN)(q - pkt);
        st = pxe->UdpWrite(pxe, 0, &server, &server_port, 0, 0,
                &local_port, 0, 0, &blen, pkt);
        con_puts("[uefi] udp tftp rrq st=");
        con_hex(st);
        con_puts("\n");
        if (EFI_ERROR(st)) {
            con_puts("[uefi] udp tftp send failed\n");
            return 0;
        }
    }

    uint32_t blksize = 512;
    uint32_t window = 1;
    uint32_t tsize = 0;
    uint32_t seq = 0;              /* DATA blocks already stored */
    uint16_t last_ack = 0;
    uint16_t expected = 1;
    uint64_t total = 0;
    int got_oack = 0;
    int any_src_port = 1;
    int retries = 0;
    EFI_IP_ADDRESS src;
    uint16_t src_port = 0;

    for (;;) {
        uint32_t len = 0;
        if (tftp_recv(pxe, pkt, sizeof(pkt), &len, &src, &src_port,
                    any_src_port) != 0) {
            if (++retries > 8) {
                con_puts("[uefi] udp tftp timeout\n");
                return 0;
            }
            if (seq == 0 && !got_oack)
                (void)tftp_send(pxe, pkt, (uint32_t)(q - pkt), &server,
                        server_port, &local_port);
            else
                (void)tftp_ack(pxe, &server, server_port, &local_port,
                        last_ack);
            continue;
        }
        any_src_port = 0;
        server_port = src_port;
        retries = 0;
        if (len < 4)
            continue;

        uint16_t op = be16_(pkt);
        if (op == TFTP_ERROR) {
            con_puts("[uefi] udp tftp server error\n");
            return 0;
        }
        if (op == TFTP_OACK) {
            uint8_t* p = pkt + 2;
            uint8_t* end = pkt + len;
            while (p < end && *p) {
                char* name = (char*)p;
                while (p < end && *p)
                    p++;
                p++;
                char* val = (char*)p;
                while (p < end && *p)
                    p++;
                p++;
                if (str_eq_(name, "blksize"))
                    blksize = dec_(val);
                else if (str_eq_(name, "windowsize"))
                    window = dec_(val);
                else if (str_eq_(name, "tsize"))
                    tsize = dec_(val);
            }
            if (blksize < 8 || blksize > 1468)
                blksize = 512;
            if (window == 0)
                window = 1;
            got_oack = 1;
            (void)tftp_ack(pxe, &server, server_port, &local_port, 0);
            continue;
        }
        if (op != TFTP_DATA)
            continue;

        uint16_t blk = be16_(pkt + 2);
        uint32_t dlen = len - 4;
        if (blk != expected) {
            /* Duplicate or gap: acknowledge the last contiguous block so
             * the server retransmits from there. */
            (void)tftp_ack(pxe, &server, server_port, &local_port, last_ack);
            continue;
        }
        uint64_t off = (uint64_t)seq * blksize;
        if (off + dlen > max_size) {
            con_puts("[uefi] udp tftp too large\n");
            return 0;
        }
        memcpy(out + off, pkt + 4, dlen);
        total += dlen;
        seq++;
        last_ack = blk;
        expected = (uint16_t)(expected + 1);

        if (dlen < blksize || (tsize != 0 && total >= tsize) ||
                (window != 0 && (seq % window) == 0)) {
            (void)tftp_ack(pxe, &server, server_port, &local_port, blk);
        }
        if (dlen < blksize || (tsize != 0 && total >= tsize)) {
            *out_size = total;
            con_puts("[uefi] udp tftp ");
            con_puts(path);
            con_puts(" blk=");
            con_dec(blksize);
            con_puts(" win=");
            con_dec(window);
            con_puts(" n=");
            con_dec(total);
            con_puts("\n");
            return (void*)(uintptr_t)abuf;
        }
    }
#else
    (void)path; (void)max_size; (void)out_size;
    return 0;
#endif
}

static void* read_network(int which, uint64_t* size) {
    uint64_t max_size = which == 0 ? Q8B_KERNEL_MTFTP_MAX :
            (which == 2 ? Q8B_INITRAMFS_MTFTP_MAX : Q8B_ROOTFS_MTFTP_MAX);

    /* The optional fast UDP client is still the first network method, but
     * network itself is now the last source unless the loader was started by
     * PXE/HTTP. */
    {
        const char* ppath = pxe_path_name(which, 0);
        if (ppath != 0) {
            void* p = read_file_pxe_udp(ppath, max_size, size);
            if (p) {
                con_puts("[uefi]   UDP TFTP ok, ");
                con_dec(*size);
                con_puts(" bytes\n");
                return p;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        const char* ppath = pxe_path_name(which, i);
        if (ppath == 0)
            break;
        void* p = read_file_mtftp4(ppath, max_size, size);
        if (p) {
            con_puts("[uefi]   MTFTP4 ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
    }
    for (int i = 0; i < 3; i++) {
        const char* path = path_name(which, i);
        if (path == 0)
            break;
        void* p = read_file_loadfile(path, size);
        if (p) {
            con_puts("[uefi]   LoadFile ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
        const char* ppath = pxe_path_name(which, i);
        if (ppath) {
            p = read_file_pxe(ppath, size);
            if (p) {
                con_puts("[uefi]   PXE Mtftp ok, ");
                con_dec(*size);
                con_puts(" bytes\n");
                return p;
            }
        }
    }
    return 0;
}

static void* read_first(int which, uint64_t* size) {
    con_puts("[uefi] source order: self=");
    con_puts(source_name(g_boot_source));
    con_puts(" -> sd -> usb -> nvme -> net\n");

    /* A PXE/HTTP boot has no local ESP of its own.  Preserve that source's
     * precedence, then fall through to local SFS volumes if the network
     * object is absent. */
    if (g_boot_source == Q8B_BOOT_SOURCE_NET) {
        void* p = read_network(which, size);
        if (p)
            return p;
    }

    for (int i = 0; i < 3; i++) {
        const char* path = path_name(which, i);
        if (path == 0)
            break;
        con_puts("[uefi] try local ");
        con_puts(path);
        con_puts("\n");
        void* p = read_file_sfs(path, size);
        if (p)
            return p;
    }

    /* The network backend is always the final fallback for local boots. */
    if (g_boot_source != Q8B_BOOT_SOURCE_NET)
    {
        void* p = read_network(which, size);
        if (p)
            return p;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* bootinfo / memory map                                              */
/* ------------------------------------------------------------------ */
static EFI_MEMORY_DESCRIPTOR* g_memmap;
static UINTN g_memmap_size;
static UINTN g_memmap_dsize;
static UINTN g_memmap_key;
static uint32_t g_memmap_ver;

static int get_memory_map(void) {
    UINTN size = 0;
    EFI_STATUS st;

    st = gBS->GetMemoryMap(&size, 0, &g_memmap_key, &g_memmap_dsize,
            &g_memmap_ver);
    if (st != EFI_BUFFER_TOO_SMALL && EFI_ERROR(st))
        return -1;

    for (int i = 0; i < 8; i++) {
        size += 2 * EFI_PAGE_SIZE;   /* room for allocations made by us */
        UINTN pages = (size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
        EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
        if (buf == 0)
            return -1;
        g_memmap = (EFI_MEMORY_DESCRIPTOR*)(uintptr_t)buf;
        UINTN sz = pages * EFI_PAGE_SIZE;
        st = gBS->GetMemoryMap(&sz, g_memmap, &g_memmap_key,
                &g_memmap_dsize, &g_memmap_ver);
        if (!EFI_ERROR(st)) {
            g_memmap_size = sz;
            return 0;
        }
        if (st != EFI_BUFFER_TOO_SMALL)
            return -1;
        size = sz;
    }
    return -1;
}

static int mem_usable(uint32_t t) {
    return t == EfiLoaderCode || t == EfiLoaderData ||
           t == EfiBootServicesCode || t == EfiBootServicesData ||
           t == EfiConventionalMemory || t == EfiPalCode;
}

static uint64_t memory_ram_size(void) {
    uint64_t max_end = 0;
    uint32_t count = (uint32_t)(g_memmap_size / g_memmap_dsize);

    for (uint32_t i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR* d =
            (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)g_memmap + i * g_memmap_dsize);
        uint64_t start = d->PhysicalStart;
        uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
        if (start < Q8B_RAM_BASE || start >= (1ULL << 40))
            continue;
        if (mem_usable(d->Type) && end > max_end)
            max_end = end;
    }
    if (max_end <= Q8B_RAM_BASE)
        return 4ULL * 1024 * 1024 * 1024;
    return max_end - Q8B_RAM_BASE;
}


static void __attribute__((unused)) dump_memmap_around(uint64_t target) {
    uint32_t count = (uint32_t)(g_memmap_size / g_memmap_dsize);
    con_puts("[uefi] memmap near ");
    con_hex(target);
    con_puts(" (type start pages):\n");
    for (uint32_t i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR* d =
            (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)g_memmap + i * g_memmap_dsize);
        uint64_t start = d->PhysicalStart;
        uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
        if (end + 0x2000000 < target || start > target + 0x2000000)
            continue;
        con_puts("  ");
        con_dec(d->Type);
        con_puts(" ");
        con_hex(start);
        con_puts(" ");
        con_dec(d->NumberOfPages);
        con_puts("\n");
    }
}
static void cache_clean(void* start, uint64_t size) {
    uint64_t line;
    __asm__ volatile("mrs %0, CTR_EL0" : "=r"(line));
    /* DminLine is bits [19:16], log2 of words */
    uint64_t dmin = 4u << ((line >> 16) & 0xF);
    uint64_t addr = (uint64_t)start & ~(dmin - 1);
    uint64_t end = (uint64_t)start + size;

    for (; addr < end; addr += dmin)
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cache_sync_icache(void) {
    __asm__ volatile("ic iallu" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}


/*
 * Enter the kernel with the MMU and caches off.
 *
 * UEFI maps firmware-owned DRAM as read-only/non-executable; copying the
 * kernel to its fixed link address after EBS makes the data writable, but
 * the jump itself still hits the stale NX attribute.  Linux's EFI stub has
 * the same problem and clears it by dropping to the physical entry point
 * with the MMU off.  UEFI identity-maps DRAM, so the stub's remaining code,
 * stack and the kernel physical addresses stay valid.
 */
static void __attribute__((noreturn)) jump_no_mmu(uint64_t entry) {
    register uint64_t x1 __asm__("x1") = entry;
    __asm__ volatile(
        "msr daifset, #0xf\n"
        "mrs x0, sctlr_el1\n"
        "bic x0, x0, #1\n"          /* M: MMU off */
        "bic x0, x0, #(1 << 2)\n"   /* C: D-cache off */
        "bic x0, x0, #(1 << 12)\n"  /* I: I-cache off */
        "msr sctlr_el1, x0\n"
        "isb\n"
        "dsb sy\n"
        "br x1\n"
        :: "r"(x1) : "x0", "memory");
    for (;;)
        __asm__ volatile("wfi");
}


/* ------------------------------------------------------------------ */
/* PCIe inventory                                                     */
/* ------------------------------------------------------------------ */

/* Read one aligned dword of PCI configuration space through the root
 * bridge's Pci.Read().  Address encoding is EFI_PCI_ADDRESS:
 * bus[31:24], dev[23:16], func[15:8], reg[7:0]. */
static int pci_cfg32(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* rb, uint8_t bus,
        uint8_t dev, uint8_t fn, uint8_t reg, uint32_t* out) {
    uint64_t addr = ((uint64_t)bus << 24) | ((uint64_t)dev << 16) |
                    ((uint64_t)fn << 8) | (uint64_t)(reg & 0xfc);
    *out = 0xffffffffu;
    return EFI_ERROR(rb->Pci.Read(rb, 2 /* EfiPciWidthUint32 */, addr, 1,
                out)) ? -1 : 0;
}

static void pci_dump_device(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* rb,
        uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t vd, cls, cmd, hdr;
    if (pci_cfg32(rb, bus, dev, fn, 0x00, &vd) != 0)
        return;
    if (vd == 0 || vd == 0xffffffffu)
        return;
    if (pci_cfg32(rb, bus, dev, fn, 0x08, &cls) != 0)
        return;
    if (pci_cfg32(rb, bus, dev, fn, 0x04, &cmd) != 0)
        return;
    if (pci_cfg32(rb, bus, dev, fn, 0x0c, &hdr) != 0)
        return;

    con_puts("[pci] ");
    con_dec(bus);
    con_puts(":");
    con_dec(dev);
    con_puts(".");
    con_dec(fn);
    con_puts(" id=");
    con_hex(vd);
    con_puts(" class=");
    con_hex(cls >> 8);
    con_puts(" cmd=");
    con_hex(cmd & 0xffff);

    if (((cls >> 8) & 0xffff) == 0x0604) {
        uint32_t buses = 0;
        pci_cfg32(rb, bus, dev, fn, 0x18, &buses);
        con_puts(" buses=");
        con_hex(buses);
    } else {
        uint32_t bar;
        for (uint8_t i = 0; i < 6; i++) {
            if (pci_cfg32(rb, bus, dev, fn, (uint8_t)(0x10 + i * 4), &bar) != 0)
                break;
            if (bar == 0 || bar == 0xffffffffu)
                continue;
            con_puts(" bar");
            con_dec(i);
            con_puts("=");
            con_hex(bar);
            if ((bar & 0x6) == 0x4) {   /* 64-bit MMIO BAR pair */
                uint32_t hi;
                if (pci_cfg32(rb, bus, dev, fn, (uint8_t)(0x14 + i * 4),
                            &hi) == 0) {
                    con_puts(":");
                    con_hex(hi);
                }
                i++;                    /* skip the upper half */
            }
        }
    }
    con_puts("\n");
}

/* Per-device inventory through EFI_PCI_IO_PROTOCOL.  The Q8B firmware
 * publishes this on every controller it enumerated even when the root
 * bridge handles are not exposed. */
static int pci_inventory_pciio(void) {
    static const EFI_GUID io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = 0;
    EFI_STATUS st = gBS->LocateHandleBuffer(2 /* ByProtocol */,
            (EFI_GUID*)&io_guid, 0, &count, &handles);
    if (EFI_ERROR(st) || handles == 0 || count == 0) {
        con_puts("[pci] no PCI I/O handles\n");
        return 0;
    }
    con_puts("[pci] pciio devices=");
    con_dec(count);
    con_puts("\n");

    for (UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL* io = 0;
        UINTN seg = 0, bus = 0, dev = 0, fn = 0;
        uint32_t vd = 0, cls = 0, cmd = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], (EFI_GUID*)&io_guid,
                        (void**)&io)) || io == 0 || io->GetLocation == 0)
            continue;
        if (EFI_ERROR(io->GetLocation(io, &seg, &bus, &dev, &fn)))
            continue;
        if (seg != 0 || bus > 0xff)
            continue;
        if (EFI_ERROR(io->Pci.Read(io, 2, 0x00, 1, &vd)) ||
                vd == 0 || vd == 0xffffffffu)
            continue;
        io->Pci.Read(io, 2, 0x08, 1, &cls);
        io->Pci.Read(io, 2, 0x04, 1, &cmd);

        con_puts("[pci] ");
        con_dec((uint64_t)seg);
        con_puts(":");
        con_dec((uint64_t)bus);
        con_puts(":");
        con_dec((uint64_t)dev);
        con_puts(".");
        con_dec((uint64_t)fn);
        con_puts(" id=");
        con_hex(vd);
        con_puts(" class=");
        con_hex(cls >> 8);
        con_puts(" cmd=");
        con_hex(cmd & 0xffff);

        if (((cls >> 8) & 0xffff) == 0x0604) {
            uint32_t buses = 0;
            io->Pci.Read(io, 2, 0x18, 1, &buses);
            con_puts(" buses=");
            con_hex(buses);
        } else {
            for (uint8_t bi = 0; bi < 6; bi++) {
                uint32_t bar = 0;
                if (EFI_ERROR(io->Pci.Read(io, 2,
                                (uint32_t)(0x10 + bi * 4), 1, &bar)))
                    break;
                if (bar == 0 || bar == 0xffffffffu)
                    continue;
                con_puts(" bar");
                con_dec(bi);
                con_puts("=");
                con_hex(bar);
                if ((bar & 0x6) == 0x4) {
                    uint32_t hi = 0;
                    if (!EFI_ERROR(io->Pci.Read(io, 2,
                                    (uint32_t)(0x14 + bi * 4), 1, &hi))) {
                        con_puts(":");
                        con_hex(hi);
                    }
                    bi++;
                }
            }
        }
        con_puts("\n");
    }
    gBS->FreePool(handles);
    return 1;
}

/* Enumerate every PCI root bridge the firmware published and print the
 * devices behind it.  This runs before ExitBootServices, so it uses the
 * firmware's own PCI configuration route (no raw ECAM pokes) and shows
 * exactly which controllers the platform enabled for us. */
static void pci_inventory(void) {
    static const EFI_GUID rb_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;

    if (pci_inventory_pciio())
        return;

    UINTN count = 0;
    EFI_HANDLE* handles = 0;
    EFI_STATUS st = gBS->LocateHandleBuffer(2 /* ByProtocol */,
            (EFI_GUID*)&rb_guid, 0, &count, &handles);
    if (EFI_ERROR(st) || handles == 0 || count == 0) {
        con_puts("[pci] no root bridge handles\n");
        return;
    }

    con_puts("[pci] root bridges=");
    con_dec(count);
    con_puts("\n");

    for (UINTN i = 0; i < count; i++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* rb = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], (EFI_GUID*)&rb_guid,
                        (void**)&rb)) || rb == 0)
            continue;
        con_puts("[pci] root");
        con_dec(i);
        con_puts(" seg=");
        con_dec(rb->SegmentNumber);

        /* The resource list fingerprints the root bridge: its MMIO windows
         * mirror the SoC DTS ranges (pcie2a/3b/3a). */
        if (rb->Configuration) {
            void* resources = 0;
            if (!EFI_ERROR(rb->Configuration(rb, &resources)) &&
                    resources != 0) {
                uint8_t* p = (uint8_t*)resources;
                for (int guard = 0; guard < 16; guard++) {
                    uint8_t desc = p[0];
                    if (desc == 0x79)
                        break;
                    if (desc != 0x8a)
                        break;
                    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR* d =
                        (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR*)p;
                    con_puts(d->ResType == 2 ? " bus=" : " mem=");
                    con_hex(d->AddrRangeMin);
                    con_puts("..");
                    con_hex(d->AddrRangeMax);
                    p += d->Len + 3;
                }
            }
        }
        con_puts("\n");

        for (uint8_t bus = 0; bus < 8; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                uint32_t vd = 0;
                if (pci_cfg32(rb, bus, dev, 0, 0x00, &vd) != 0)
                    continue;
                if (vd == 0 || vd == 0xffffffffu)
                    continue;
                pci_dump_device(rb, bus, dev, 0);
                uint32_t hdr = 0;
                if (pci_cfg32(rb, bus, dev, 0, 0x0c, &hdr) == 0 &&
                        (hdr >> 16) & 0x80) {
                    for (uint8_t fn = 1; fn < 8; fn++)
                        pci_dump_device(rb, bus, dev, fn);
                }
            }
        }
    }
    gBS->FreePool(handles);
}


/* ------------------------------------------------------------------ */
/* EFI entry                                                          */
/* ------------------------------------------------------------------ */
uint64_t efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static const EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;

    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImage = ImageHandle;
    g_con_ok = 1;
    serial_init();
    con_puts("[uefi] serial log: ");
    con_puts(g_serial_mode == 1 ? "EFI Serial I/O" :
             g_serial_mode == 2 ? (g_uart17_ok ? "UART17 direct (TX probe OK)"
                                               : "UART17 direct (forced, TX probe failed)")
                                  : "none (no EFI serial; UART17 silent)");
    con_puts(g_uart17_ok ? "; kernel uart17: on\n" : "; kernel uart17: off\n");

    gBS->SetWatchdogTimer(0, 0, 0, 0);

#ifndef Q8B_QEMU_TEST
    /* Q8B APSS watchdog (qcom,apss-wdt-sc8280xp @ 0x17c10000).
     * This block uses the qcom,kpss-wdt layout: RST=0x4, EN=0x8,
     * STS=0xc, BARK=0x10 and BITE=0x14.  The SBL/UEFI can leave it
     * armed; the PMIC then records a PSHOLD warm reset after ~45 seconds.
     * Stop it before ExitBootServices while UEFI MMIO is still mapped. */
    {
        volatile uint32_t* wdt = (volatile uint32_t*)(uintptr_t)0x17c10000u;
        uint32_t en = wdt[2];
        uint32_t status = wdt[3];
        uint32_t bark = wdt[4];
        uint32_t bite = wdt[5];
        wdt[2] = 0;          /* WDT_EN: disable */
        wdt[1] = 1;          /* WDT_RST: clear/pet */
        __asm__ volatile("dsb sy" ::: "memory");
        con_puts("[uefi] APSS WDT en=");
        con_hex(en);
        con_puts(" status=");
        con_hex(status);
        con_puts(" bite=");
        con_hex(bite);
        con_puts(" bark=");
        con_hex(bark);
        con_puts(" -> disabled\n");
    }
#endif

    con_puts("\n");
    con_puts("=== EwokOS Q8B UEFI loader ===\n");
    con_puts("[uefi] firmware rev ");
    con_hex(SystemTable->Revision);
    con_puts("\n");

    if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, (EFI_GUID*)&li_guid,
                    (void**)&gImageInfo)))
        gImageInfo = 0;
    g_boot_source = gImageInfo ? device_source(gImageInfo->DeviceHandle) :
            Q8B_BOOT_SOURCE_UNKNOWN;
    if (g_boot_source == Q8B_BOOT_SOURCE_UNKNOWN && gImageInfo &&
            gImageInfo->DeviceHandle) {
        /* Some PXE implementations omit a network node from the device path
         * but expose EFI_LOAD_FILE on the image device itself. */
        static EFI_GUID lf_guid = EFI_LOAD_FILE_PROTOCOL_GUID;
        void* lf = 0;
        if (!EFI_ERROR(gBS->HandleProtocol(gImageInfo->DeviceHandle,
                        &lf_guid, &lf)) && lf != 0)
            g_boot_source = Q8B_BOOT_SOURCE_NET;
    }
    con_puts("[uefi] boot source=");
    con_puts(source_name(g_boot_source));
    con_puts("\n");

    /* ---- framebuffer: robust multi-platform GOP discovery ----
     * Q8B: LocateProtocol GOP has a real base.  gaokun3 / TW220: every GOP is
     * a base-0 proxy, the linear FB is verified in the reserved 36-MiB
     * region at 0xc6200000 by two BLT patterns + rotation read-back. */
    uint64_t fb_addr = 0, fb_pitch = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_bpp = 32, fb_format = 0;
    struct q8b_fb qfb;
    if (q8b_gop_discover(&qfb)) {
        fb_addr = qfb.base;
        fb_w = qfb.width;
        fb_h = qfb.height;
        fb_pitch = (uint64_t)qfb.scanpx * 4;
        fb_format = qfb.format;
        if (fb_format == 2) {
            /* PixelBitMask: derive the depth from the mask widths. */
            uint32_t mask = qfb.masks[0] | qfb.masks[1] | qfb.masks[2];
            uint32_t bits = 0;
            while (mask) {
                bits += mask & 1;
                mask >>= 1;
            }
            if (bits > 0 && bits <= 32)
                fb_bpp = bits;
        }
        con_puts("[uefi] gop adopted ");
        con_dec(fb_w);
        con_puts("x");
        con_dec(fb_h);
        con_puts(" bpp=");
        con_dec(fb_bpp);
        con_puts(" pitch=");
        con_dec(fb_pitch);
        con_puts(" @");
        con_hex(fb_addr);
        con_puts("\n");
        /* The debug console draws in the panel's real scan-out orientation
         * (no rotation): the loader never rotates the screen. */
        fb_debug_setup(fb_addr, fb_w, fb_h, qfb.scanpx, fb_format,
                       qfb.masks, 0);
    } else {
        con_puts("[uefi] no usable linear framebuffer (serial-only)\n");
    }

    /* ---- device tree (informational for now) ---- */
    uint64_t dtb_addr = 0, dtb_size = 0;
    char board_model[32] = {0};
    if (SystemTable->ConfigurationTable) {
        for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE* ct = &SystemTable->ConfigurationTable[i];
            if (guid_eq(&ct->VendorGuid, (EFI_GUID*)&dtb_guid)) {
                dtb_addr = (uint64_t)(uintptr_t)ct->VendorTable;
                if (dtb_addr) {
                    uint32_t totalsize = fdt_be32((const void*)(uintptr_t)
                            (dtb_addr + 4));
                    if (totalsize > 0 && totalsize < 4 * 1024 * 1024)
                        dtb_size = totalsize;
                }
                break;
            }
        }
    }
    if (dtb_addr) {
        con_puts("[uefi] dtb @");
        con_hex(dtb_addr);
        con_puts(" size=");
        con_dec(dtb_size);
        con_puts("\n");
    }
    fdt_model(dtb_addr, board_model, sizeof(board_model));
    if (!board_model[0])
        smbios_product_name(board_model, sizeof(board_model));
    con_puts("[uefi] platform model: ");
    con_puts(board_model[0] ? board_model : "unknown (no DTB model)");
    con_puts("\n");

    /* ---- PCIe inventory (before EBS, uses firmware config route) ---- */
    pci_inventory();

    /* The bootinfo page is written after EBS too.  It sits just below the
     * kernel window and is not covered by the kernel reservation, so claim
     * it explicitly; otherwise the firmware may leave it read-only. */
    EFI_PHYSICAL_ADDRESS bdst = alloc_pages_at(Q8B_BOOTINFO_PHYS,
            Q8B_BOOTINFO_SIZE / EFI_PAGE_SIZE);
    if (bdst == Q8B_BOOTINFO_PHYS) {
        con_puts("[uefi] bootinfo window reserved at ");
        con_hex(Q8B_BOOTINFO_PHYS);
        con_puts("\n");
    } else {
        con_puts("[uefi] bootinfo window not reservable\n");
    }

    /* Reserve the kernel window before any large file allocation so the
     * 128MB rootfs buffer cannot be placed on top of it.  At 0x90080000
     * this normally succeeds; if it does not, the post-EBS overlap check
     * below still refuses to overwrite the stub or a live buffer. */
    EFI_PHYSICAL_ADDRESS kdst = alloc_pages_at(Q8B_KERNEL_PHYS,
            Q8B_KERNEL_RESERVE / EFI_PAGE_SIZE);
    if (kdst == Q8B_KERNEL_PHYS) {
        con_puts("[uefi] kernel window reserved at ");
        con_hex(Q8B_KERNEL_PHYS);
        con_puts("\n");
    } else {
        con_puts("[uefi] kernel window not reservable; will check overlap\n");
        if (get_memory_map() == 0)
            dump_memmap_around(Q8B_KERNEL_PHYS);
        /* print large descriptors above the kernel image so a free window
         * can be picked from the serial log */
        if (g_memmap) {
            uint32_t c = (uint32_t)(g_memmap_size / g_memmap_dsize);
            con_puts("[uefi] memmap 0x84000000..0xC0000000:\n");
            for (uint32_t di = 0; di < c; di++) {
                EFI_MEMORY_DESCRIPTOR* d =
                    (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)g_memmap +
                        di * g_memmap_dsize);
                uint64_t ds = d->PhysicalStart;
                uint64_t de = ds + d->NumberOfPages * EFI_PAGE_SIZE;
                if (de <= 0x84000000ULL || ds >= 0xC0000000ULL ||
                        d->NumberOfPages < 0x100)
                    continue;
                con_puts("  ");
                con_dec(d->Type);
                con_puts(" ");
                con_hex(ds);
                con_puts(" ");
                con_dec(d->NumberOfPages);
                con_puts("\n");
            }
        }
    }

    /* ---- kernel + rootfs ---- */
    uint64_t ksize = 0, rsize = 0;
    void* kbuf = read_first(0, &ksize);
    if (kbuf == 0) {
        con_puts("[uefi] FATAL: KERNEL.BIN not found\n");
        halt_forever();
    }
    if (ksize > Q8B_KERNEL_RESERVE) {
        con_puts("[uefi] FATAL: kernel too large\n");
        halt_forever();
    }

#if defined(Q8B_MTFTP_FIRST) && defined(Q8B_SD_ROOTFS)
    /* PXE supplied the kernel, so explicitly let UEFI touch the local ESP
     * before EBS.  The returned PXE buffer remains the boot image. */
    con_puts("[uefi] connecting local BlockIo controllers\n");
    con_puts("[uefi] BlockIo connected=");
    con_dec(connect_blockio_controllers());
    con_puts("\n");
    con_puts("[uefi] probing local SFS before SD handoff\n");
    if (probe_sfs_media() == 0)
        con_puts("[uefi] local SFS probe ok\n");
    else
        con_puts("[uefi] local SFS probe unavailable\n");
#endif

    void* rbuf = 0;
    uint32_t rd_flags = 0;
    g_alloc_high = 1;
    rbuf = read_first(2, &rsize);
    g_alloc_high = 0;
    if (rbuf != 0) {
        rd_flags = Q8B_RD_FLAG_BOOT_ONLY;
    }
#if !defined(Q8B_SD_ROOTFS) && !defined(Q8B_KERNEL_ONLY)
    if (rbuf == 0) {
        /* Keep a complete PXE/ESP rootfs out of the low DMA32 area too.
         * Besides preserving the contiguous device pools, this avoids a
         * firmware memory-map split at the allocation boundary on Q8B. */
        g_alloc_high = 1;
        rbuf = read_first(1, &rsize);
        g_alloc_high = 0;
        if (rbuf == 0) {
            con_puts("[uefi] rootfs not found, trying EFI LoadFile2 initrd\n");
            rbuf = read_initrd_loadfile2(&rsize);
        }
    }
#endif
    if (rbuf == 0) {
#if defined(Q8B_SD_ROOTFS) || defined(Q8B_KERNEL_ONLY)
        con_puts("[uefi] no INITRAMFS.IMG; kernel-only SD fallback\n");
#else
        con_puts("[uefi] FATAL: INITRAMFS.IMG/ROOTFS.IMG not found\n");
        halt_forever();
#endif
    }
    if (rbuf != 0) {
    con_puts(rd_flags ? "boot ramfs " : "rootfs ");
    con_dec(rsize / (1024 * 1024));
    con_puts(" MB @");
    con_hex((uint64_t)(uintptr_t)rbuf);
    con_puts("\n");
    }

    /* Ensure the fixed kernel window does not overlap the stub or the
     * already-loaded file buffers. */
    {
        uint64_t dst = Q8B_KERNEL_PHYS;
        uint64_t dst_end = dst + Q8B_KERNEL_RESERVE;
        uint64_t ib = gImageInfo ?
            (uint64_t)(uintptr_t)gImageInfo->ImageBase : 0;
        uint64_t ie = ib + (gImageInfo ? gImageInfo->ImageSize : 0);
        uint64_t kb = (uint64_t)(uintptr_t)kbuf;
        uint64_t ke = kb + ksize;
        uint64_t rb = (uint64_t)(uintptr_t)rbuf;
        uint64_t re = rb + rsize;
        if ((ib < dst_end && ie > dst) || (kb < dst_end && ke > dst) ||
                (rb < dst_end && re > dst)) {
            con_puts("[uefi] FATAL: kernel load address overlaps stub/image "
                     "buffer (stub ");
            con_hex(ib);
            con_puts("..");
            con_hex(ie);
            con_puts(")\n");
            halt_forever();
        }
    }

    /* All memory writes happen while Boot Services and the firmware's
     * exception handler are still alive. */
    memcpy((void*)(uintptr_t)Q8B_KERNEL_PHYS, kbuf, ksize);

    /* ---- final memory map (must be the last Boot Service call before EBS) ---- */
    if (get_memory_map() != 0) {
        con_puts("[uefi] FATAL: GetMemoryMap failed\n");
        halt_forever();
    }

    uint64_t ram_size = memory_ram_size();

    /* Copy the final memory map into the bootinfo page (physical memory). */
    q8b_bootinfo_t* bi = (q8b_bootinfo_t*)(uintptr_t)Q8B_BOOTINFO_PHYS;
    uint8_t* map_dst = (uint8_t*)(uintptr_t)(Q8B_BOOTINFO_PHYS + 0x1000);
    uint64_t map_cap = Q8B_BOOTINFO_SIZE - 0x1000;
    uint64_t map_copy = g_memmap_size;
    if (map_copy > map_cap)
        map_copy = map_cap;

    memset(bi, 0, sizeof(*bi));
    memcpy(map_dst, g_memmap, map_copy);

    bi->magic = Q8B_BOOTINFO_MAGIC;
    bi->version = Q8B_BOOTINFO_VERSION;
    bi->ram_base = Q8B_RAM_BASE;
    bi->ram_size = ram_size;
    bi->fb_addr = fb_addr;
    /* Report the REAL scan-out geometry only: physical columns x rows, byte
     * stride, bpp and pixel format.  No rotation is applied or advertised
     * here; portrait-panel presentation rotation (displayd rotate) is a
     * user-space policy decision. */
    bi->fb_width = fb_w;
    bi->fb_height = fb_h;
    bi->fb_pitch = (uint32_t)fb_pitch;
    bi->fb_bpp = fb_bpp;
    bi->fb_format = fb_format;
    bi->_pad_fb = 0;   /* ABI v1 padding; reserved, no rotation reported */
    /* Serial hand-off: the loader probed the concrete transport, the kernel
     * decides at runtime whether to bring UART17 up and print diagnostics. */
    bi->serial_flags = (g_uart17_ok ? 1u : 0u) |
                       (g_serial_mode == 1 ? 2u : 0u);
    bi->rd_base = (uint64_t)(uintptr_t)rbuf;
    bi->rd_size = rsize;
    bi->rd_flags = rd_flags;
    bi->boot_source = g_boot_source;
    bi->kernel_phys = Q8B_KERNEL_PHYS;
    bi->kernel_size = ksize;
    bi->memmap_addr = (uint64_t)(uintptr_t)map_dst;
    bi->memmap_count = (uint32_t)(map_copy / g_memmap_dsize);
    bi->memmap_dsize = (uint32_t)g_memmap_dsize;
    bi->dtb_addr = dtb_addr;
    bi->dtb_size = dtb_size;
    memcpy(bi->model, board_model, sizeof(bi->model));
    /* Record firmware-reserved ranges that fall inside the usable window so
     * the kernel allocator can punch them out. */
    uint32_t count = (uint32_t)(g_memmap_size / g_memmap_dsize);
    uint64_t limit = Q8B_RAM_BASE + Q8B_USABLE_LIMIT;
    for (uint32_t i = 0; i < count && bi->resv_count < Q8B_MAX_RESV; i++) {
        EFI_MEMORY_DESCRIPTOR* d =
            (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)g_memmap + i * g_memmap_dsize);
        uint64_t start = d->PhysicalStart;
        uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
        if (mem_usable(d->Type))
            continue;
        if (end <= Q8B_RAM_BASE || start >= limit)
            continue;
        if (start < Q8B_RAM_BASE)
            start = Q8B_RAM_BASE;
        if (end > limit)
            end = limit;
        bi->resv[bi->resv_count].base = start;
        bi->resv[bi->resv_count].size = end - start;
        bi->resv_count++;
    }

    /* Make the copied kernel/rootfs/bootinfo visible to a boot CPU that
     * starts with MMU and caches off. */
    cache_clean((void*)(uintptr_t)Q8B_KERNEL_PHYS, Q8B_KERNEL_RESERVE);
    cache_clean((void*)(uintptr_t)Q8B_BOOTINFO_PHYS, Q8B_BOOTINFO_SIZE);
    if (rbuf != 0 && rsize != 0)
        cache_clean((void*)(uintptr_t)rbuf, rsize);
    cache_sync_icache();

    /* ---- ExitBootServices; from here on only the MMU-off jump remains ---- */
    EFI_STATUS ebs_st = gBS->ExitBootServices(ImageHandle, g_memmap_key);
    if (EFI_ERROR(ebs_st)) {
        con_puts("[uefi] ExitBootServices retry\n");
        if (get_memory_map() != 0)
            halt_forever();
        ebs_st = gBS->ExitBootServices(ImageHandle, g_memmap_key);
        if (EFI_ERROR(ebs_st)) {
            con_puts("[uefi] FATAL: ExitBootServices failed\n");
            halt_forever();
        }
    }

    /* From here on no firmware services are available. */
    g_con_ok = 0;

    /* Jump to __entry at its physical link address with the MMU off.
     * x0 is unused: the bootinfo page has a fixed physical address. */
#if !defined(Q8B_QEMU_TEST) && !defined(Q8B_NO_SERIAL)
    /* Post-EBS probe: bypass EFI Serial I/O and write UART17 TX directly.
     * If this line appears, the UART is alive at the kernel handoff; if not,
     * ExitBootServices/the UEFI serial driver stopped UART17. */
    {
        const char* probe = "\n[uefi] post-EBS UART17 raw K\n";
        while (*probe)
            uart17_putc(*probe++);
    }
#endif
    jump_no_mmu(Q8B_KERNEL_PHYS);
}
