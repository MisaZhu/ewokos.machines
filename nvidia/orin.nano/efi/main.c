/*
 * BOOTAA64.EFI — EwokOS Jetson Orin Nano UEFI bootloader.
 *
 * The Orin Nano devkit boots MB1/MB2 -> TF-A BL31 -> NVIDIA EDK2 UEFI (EL2).
 * UEFI loads this PE32+ application from the ESP (\EFI\BOOT\BOOTAA64.EFI,
 * UEFI Shell, or a boot option).  We then:
 *
 *   1. log to EFI Serial I/O / the UARTC debug UART (J14 header),
 *   2. load KERNEL.BIN and INITRAMFS.IMG from a SimpleFileSystem volume
 *      (SD/USB/NVMe ESP), with a GRUB-style LoadFile2 initrd fallback,
 *   3. reserve and fill the kernel at ORIN_KERNEL_PHYS,
 *   4. collect the final UEFI memory map, ExitBootServices,
 *   5. publish the bootinfo page at ORIN_BOOTINFO_PHYS and jump to the
 *      kernel with the active MMU regime off (EL2 on Jetson firmware).
 *
 * Calling convention: AArch64 UEFI is AAPCS64, identical to the compiler's
 * normal convention, so no assembly shim is required.
 */
#include "efi.h"

/* Must match machines/nvidia/orin.nano/kernel/config.mk, mkos.lds.S and
 * bsp/hw_arch.h. */
#define ORIN_RAM_BASE        0x80000000ULL
#define ORIN_KERNEL_PHYS     0x90080000ULL
#define ORIN_BOOTINFO_PHYS   0x90070000ULL
#define ORIN_KERNEL_RESERVE  (4ULL * 1024 * 1024)
#define ORIN_BOOTINFO_SIZE   0x10000ULL
#define ORIN_USABLE_LIMIT    (8ULL * 1024 * 1024 * 1024)

#define ORIN_BOOTINFO_MAGIC   0x4e524f45u   /* "EORN" little-endian */
#define ORIN_BOOTINFO_VERSION 1
#define ORIN_RD_FLAG_BOOT_ONLY (1u << 0)

#define ORIN_BOOT_SOURCE_UNKNOWN 0u
#define ORIN_BOOT_SOURCE_SD      1u
#define ORIN_BOOT_SOURCE_USB     2u
#define ORIN_BOOT_SOURCE_NVME    3u
#define ORIN_BOOT_SOURCE_NET     4u

#define ORIN_MAX_RESV 32

/* Debug UART: Tegra234 UARTC (AON), 16550-compatible, 32-bit spaced
 * registers (reg-shift=2).  Firmware leaves it clocked and programmed for
 * 115200 8N1; the loader only drives TX. */
#define ORIN_UARTC_PHYS 0x0c280000ULL
#define UART_THR 0x00
#define UART_LSR 0x14
#define UART_LSR_THRE (1u << 5)

typedef struct {
    uint64_t base;
    uint64_t size;
} orin_resv_t;

/* Keep in sync with kernel/bsp/hw_arch.h orin_bootinfo_t. */
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
    uint32_t serial_flags;   /* bit0: kernel may use UARTC */
    orin_resv_t resv[ORIN_MAX_RESV];

    uint64_t dtb_addr;
    uint64_t dtb_size;

    char model[32];
} orin_bootinfo_t;

static EFI_SYSTEM_TABLE* gST;
static EFI_BOOT_SERVICES* gBS;
static EFI_HANDLE gImage;
static EFI_LOADED_IMAGE_PROTOCOL* gImageInfo;
static int g_con_ok;
static EFI_SERIAL_IO_PROTOCOL* g_serial;
static int g_serial_mode;      /* 0=none, 1=EFI Serial I/O, 2=UARTC direct */
static int g_uart_ok;          /* UARTC usable by the kernel after EBS */
static uint32_t g_boot_source = ORIN_BOOT_SOURCE_UNKNOWN;

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
/* serial log (EFI Serial I/O, fallback to raw UARTC TX)              */
/* ------------------------------------------------------------------ */
static inline void uart_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(ORIN_UARTC_PHYS + off) = v;
}

static inline uint32_t uart_rd(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(ORIN_UARTC_PHYS + off);
}

/* One bounded TX byte.  Returns 0 when the THR accepted the byte, -1 when
 * THRE never asserted (UART not clocked / not wired). */
static int uartc_putc(char c) {
    for (volatile uint32_t i = 0; i < 200000; i++) {
        if (uart_rd(UART_LSR) & UART_LSR_THRE) {
            uart_wr(UART_THR, (uint8_t)c);
            return 0;
        }
    }
    return -1;
}

/* Bounded UARTC probe.  An unclocked AON UART reads back 0x00 or 0xFF on
 * every register; a live 16550 idles with THRE|TSR set (LSR 0x60). */
static int __attribute__((unused)) uartc_probe(void) {
    uint32_t lsr = uart_rd(UART_LSR);
    if (lsr == 0 || lsr == 0xFFFFFFFFu)
        return -1;
    if (uartc_putc('\r') != 0 || uartc_putc('\n') != 0)
        return -1;
    return 0;
}

static void serial_init(void) {
    g_serial = 0;
    g_serial_mode = 0;          /* loader console = UEFI ConOut (safe, visible) */
    /* Do NOT raw-probe UARTC here: UEFI may not have mapped 0xc280000 in its
     * page tables, and a bare MMIO read would take a data abort (this was the
     * silent hang right after the entry banner).  The kernel maps UARTC in its
     * own MMU and drives it safely; assume the J14 debug UART is present. */
    g_uart_ok = 1;
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
    } else if (g_serial_mode == 2) {
        for (uint64_t i = 0; i < n; i++)
            uartc_putc(b[i]);
    }
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
static void con_puts(const char* ascii) {
    /* The UEFI ConOut is wired to the same debug UART, so writing both
     * duplicates every line.  With a serial backend use it exclusively. */
    if (g_serial_mode != 0) {
        serial_write_ascii(ascii);
        return;
    }
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
/* firmware DTB / SMBIOS model                                        */
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

/* Product name from the firmware's SMBIOS Type-1 record, used when the
 * firmware DTB has no /model. */
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

static EFI_PHYSICAL_ADDRESS alloc_pages_at(EFI_PHYSICAL_ADDRESS at, UINTN pages,
        uint32_t type) {
    EFI_PHYSICAL_ADDRESS p = at;
    EFI_STATUS st = gBS->AllocatePages(AllocateAddress, type, pages, &p);
    if (EFI_ERROR(st))
        return 0;
    return p;
}

static EFI_PHYSICAL_ADDRESS alloc_pages(UINTN pages) {
    EFI_PHYSICAL_ADDRESS p = 0;
    EFI_STATUS st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
            pages, &p);
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
        size = (UINTN)(64ULL * 1024 * 1024);
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

/* Classify the volume behind a handle from its device path so the loader
 * can report (and prefer) the medium that launched BOOTAA64.EFI. */
static uint32_t device_source(EFI_HANDLE handle) {
    static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL* dp = 0;
    uint32_t source = ORIN_BOOT_SOURCE_UNKNOWN;
    if (handle == 0 || EFI_ERROR(gBS->HandleProtocol(handle, &dp_guid,
                    (void**)&dp)) || dp == 0)
        return source;

    for (int guard = 0; guard < 128 && dp->Length >= 4; guard++) {
        if (dp->Type == 0x7f)
            break;
        if (dp->Type == 3) { /* messaging device path */
            switch (dp->SubType) {
            case 0x05: /* USB */
                source = ORIN_BOOT_SOURCE_USB;
                break;
            case 0x17: /* NVMe */
                source = ORIN_BOOT_SOURCE_NVME;
                break;
            case 0x1a: /* SD */
                source = ORIN_BOOT_SOURCE_SD;
                break;
            case 0x0c: /* IPv4 */
            case 0x0d: /* IPv6 */
                source = ORIN_BOOT_SOURCE_NET;
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
    case ORIN_BOOT_SOURCE_SD: return "sd";
    case ORIN_BOOT_SOURCE_USB: return "usb";
    case ORIN_BOOT_SOURCE_NVME: return "nvme";
    case ORIN_BOOT_SOURCE_NET: return "net";
    default: return "unknown";
    }
}

static int source_rank(uint32_t source) {
    switch (source) {
    case ORIN_BOOT_SOURCE_SD: return 0;
    case ORIN_BOOT_SOURCE_USB: return 1;
    case ORIN_BOOT_SOURCE_NVME: return 2;
    case ORIN_BOOT_SOURCE_UNKNOWN: return 3;
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

    /* The kernel uses 16KB pages; every physical range it maps through the
     * page tables must be 16KB aligned, while UEFI only guarantees 4KB. */
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
        case 0: return "\\orin\\KERNEL.BIN";
        case 1: return "\\KERNEL.BIN";
        case 2: return "\\EFI\\BOOT\\KERNEL.BIN";
        default: return 0;
        }
    }
    switch (i) {                /* boot ramfs */
    case 0: return "\\orin\\INITRAMFS.IMG";
    case 1: return "\\INITRAMFS.IMG";
    case 2: return "\\EFI\\BOOT\\INITRAMFS.IMG";
    default: return 0;
    }
}

/* TFTP paths (forward slashes) for the network fallback.  When the board
 * PXE-boots there is no local ESP, so the kernel/ramfs come from the same
 * TFTP server that delivered this NBP (192.168.8.1). */
static const char* pxe_path_name(int which, int i) {
    if (which == 0) {
        switch (i) {
        case 0: return "orin/KERNEL.BIN";
        case 1: return "KERNEL.BIN";
        case 2: return "kernel.bin";
        case 3: return "orin/kernel.bin";
        case 4: return "EFI/BOOT/KERNEL.BIN";
        default: return 0;
        }
    }
    switch (i) {
    case 0: return "orin/INITRAMFS.IMG";
    case 1: return "INITRAMFS.IMG";
    case 2: return "initramfs.img";
    case 3: return "orin/initramfs.img";
    case 4: return "EFI/BOOT/INITRAMFS.IMG";
    default: return 0;
    }
}

/* Independent TFTP client via EFI_MTFTP4_PROTOCOL.  Configures its own
 * network (station/server IP) so it does not depend on the PXE session state
 * that the PXE base-code Mtftp uses (which returned EFI_TFTP_ERROR here). */
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
    cfg.StationIp.Addr[0] = 192; cfg.StationIp.Addr[1] = 168;
    cfg.StationIp.Addr[2] = 8;   cfg.StationIp.Addr[3] = 42;
    cfg.SubnetMask.Addr[0] = 255; cfg.SubnetMask.Addr[1] = 255;
    cfg.SubnetMask.Addr[2] = 255; cfg.SubnetMask.Addr[3] = 0;
    cfg.GatewayIp.Addr[0] = 192; cfg.GatewayIp.Addr[1] = 168;
    cfg.GatewayIp.Addr[2] = 8;   cfg.GatewayIp.Addr[3] = 1;
    cfg.ServerIp.Addr[0] = 192;  cfg.ServerIp.Addr[1] = 168;
    cfg.ServerIp.Addr[2] = 8;    cfg.ServerIp.Addr[3] = 1;
    cfg.LocalPort = 0;
    cfg.InitialServerPort = 69;
    cfg.TryCount = 3;
    cfg.TimeoutValue = 3;
    EFI_STATUS st = mt->Configure(mt, &cfg);
    if (EFI_ERROR(st)) {
        con_puts("[uefi] mtftp4 configure st=");
        con_hex(st);
        con_puts("\n");
        return 0;
    }
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

/* Fetch a file via the firmware PXE base code (uses the DHCP-configured
 * station IP).  READ_FILE directly into a max_size buffer with the default
 * 512-byte block: GET_FILE_SIZE / negotiated large blocks returned
 * EFI_TFTP_ERROR on this firmware, so avoid them.  READ_FILE updates bsize
 * with the actual transferred length. */
static void* read_file_pxe(const char* path, uint64_t max_size,
        uint64_t* out_size) {
    static EFI_GUID pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    EFI_PXE_BASE_CODE_PROTOCOL* pxe = 0;
    EFI_IP_ADDRESS server;
    CHAR16 wpath[256];

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

    UINTN pages = (max_size + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
    if (buf == 0)
        return 0;
    uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
    uint64_t bsize = max_size;
    EFI_STATUS st = pxe->Mtftp(pxe, EFI_PXE_BASE_CODE_TFTP_READ_FILE,
            (void*)(uintptr_t)abuf, 0, &bsize, 0, &server, wpath, 0);
    con_puts("[uefi] pxe read ");
    con_puts(path);
    con_puts(" st=");
    con_hex(st);
    con_puts(" n=");
    con_dec(bsize);
    con_puts("\n");
    if (EFI_ERROR(st) || bsize == 0)
        return 0;
    *out_size = bsize;
    return (void*)(uintptr_t)abuf;
}

static void* read_first(int which, uint64_t* size) {
    for (int i = 0; path_name(which, i); i++) {
        const char* path = path_name(which, i);
        con_puts("[uefi] try ");
        con_puts(path);
        con_puts("\n");
        void* p = read_file_sfs(path, size);
        if (p)
            return p;
    }
    /* No local filesystem (PXE boot): fetch over TFTP from the PXE server. */
    uint64_t max_size = (which == 0) ? (4ULL * 1024 * 1024)
                                     : (32ULL * 1024 * 1024);
    for (int i = 0; pxe_path_name(which, i); i++) {
        const char* path = pxe_path_name(which, i);
        con_puts("[uefi] try net ");
        con_puts(path);
        con_puts("\n");
        void* p = read_file_mtftp4(path, max_size, size);
        if (p)
            return p;
        p = read_file_pxe(path, max_size, size);
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
        if (start < ORIN_RAM_BASE || start >= (1ULL << 40))
            continue;
        if (mem_usable(d->Type) && end > max_end)
            max_end = end;
    }
    if (max_end <= ORIN_RAM_BASE)
        return 4ULL * 1024 * 1024 * 1024;
    return max_end - ORIN_RAM_BASE;
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
 * The kernel must start at its fixed physical link address with the MMU off.
 * Disabling the MMU while executing from the loader PE image is unsafe: the
 * firmware remaps PE sections to non-identity virtual addresses, so the very
 * next instruction fetch after SCTLR.M=0 (PA==VA) lands on unrelated memory
 * and aborts -- and with the MMU off the exception vector cannot be fetched
 * either, so the board hangs with no register dump.  Whether the loader image
 * happens to be identity-mapped varies per boot, which made the old in-place
 * MMU-off jump fail intermittently.
 *
 * Instead we run the MMU-off sequence from a tiny position-independent
 * trampoline copied into the tail of the kernel window.  That window is a raw
 * EfiLoaderCode allocation (not a PE image), which the firmware identity-maps
 * and keeps executable at EL2, so we can branch to it with the MMU still on
 * and its self-fetch stays valid after the MMU is disabled.  It then branches
 * to the kernel physical entry with the MMU off, exactly as boot.S expects.
 *
 * NVIDIA Jetson UEFI runs at EL2, so the active translation regime is
 * SCTLR_EL2; the trampoline clears EL2 (or EL1) M/C/I and leaves the EL2->EL1
 * drop to the kernel's boot.S.
 */
/* Position-independent MMU-off trampoline as raw machine words.  Kept as a
 * .rodata byte array (not an asm symbol) because the PE converter mishandles
 * relocations against asm-section symbols, which made install_trampoline read
 * from a bad source address and abort.  All branches are PC-relative, so the
 * block executes correctly wherever it is copied.  x0 = kernel physical entry,
 * preserved in x2 across the SCTLR write. */
static const uint32_t _orin_mmu_tramp[] = {
    0xaa0003e2, /* mov  x2, x0            */
    0xd5034fdf, /* msr  daifset, #0xf     */
    0xd5384240, /* mrs  x0, CurrentEL     */
    0xd342fc00, /* lsr  x0, x0, #2        */
    0xf100081f, /* cmp  x0, #2            */
    0x540000e1, /* b.ne 1f  (+0x30)       */
    0xd53c1000, /* mrs  x0, sctlr_el2     */
    0x927ff800, /* bic  x0, x0, #1   (M)  */
    0x927df800, /* bic  x0, x0, #(1<<2) C */
    0x9273f800, /* bic  x0, x0, #(1<<12) I*/
    0xd51c1000, /* msr  sctlr_el2, x0     */
    0x14000006, /* b    2f  (+0x44)       */
    0xd5381000, /* 1: mrs x0, sctlr_el1   */
    0x927ff800, /* bic  x0, x0, #1        */
    0x927df800, /* bic  x0, x0, #(1<<2)   */
    0x9273f800, /* bic  x0, x0, #(1<<12)  */
    0xd5181000, /* msr  sctlr_el1, x0     */
    0xd5033fdf, /* 2: isb                 */
    0xd5033f9f, /* dsb  sy                */
    0xd61f0040, /* br   x2                */
};

/* Tail slot of the kernel window, beyond the copied kernel image. */
#define ORIN_TRAMP_OFF (ORIN_KERNEL_RESERVE - 0x100)

static void install_trampoline(void) {
    void* dst = (void*)(uintptr_t)(ORIN_KERNEL_PHYS + ORIN_TRAMP_OFF);
    memcpy(dst, _orin_mmu_tramp, sizeof(_orin_mmu_tramp));
}

/* The handoff branches into the kernel-window trampoline with the firmware
 * MMU still on, so the window must be executable as well as writable.  Raw
 * EfiLoaderData allocations are often already RWX, but when the firmware
 * enforces W^X they carry XP/RO; clear those attributes here (best effort --
 * absent protocol means the default mapping already allows execution).  Must
 * run before the final GetMemoryMap so the EBS key reflects the change. */
static void make_kernel_window_executable(void) {
    static EFI_GUID ma_guid = EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID;
    EFI_MEMORY_ATTRIBUTE_PROTOCOL* ma = 0;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&ma_guid, 0,
                    (void**)&ma)) || ma == 0)
        return;
    ma->ClearMemoryAttributes(ORIN_KERNEL_PHYS, ORIN_KERNEL_RESERVE,
            EFI_MEMORY_XP | EFI_MEMORY_RO);
}

static void __attribute__((noreturn)) jump_via_trampoline(uint64_t entry) {
    void (*tramp)(uint64_t) =
        (void (*)(uint64_t))(uintptr_t)(ORIN_KERNEL_PHYS + ORIN_TRAMP_OFF);
    tramp(entry);
    for (;;)
        __asm__ volatile("wfi");
}

/* ------------------------------------------------------------------ */
/* entry                                                              */
/* ------------------------------------------------------------------ */
uint64_t efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static const EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImage = ImageHandle;
    g_con_ok = 1;
    /* Guaranteed-visible early banner via the firmware ConOut, before any
     * serial backend selection: proves the loader entry was reached even if
     * the raw UARTC routing differs from the firmware console UART. */
    if (gST->ConOut) {
        CHAR16 w[64];
        ascii_to_w("\r\n[orin-loader] entry reached (ConOut)\r\n", w, 64);
        gST->ConOut->OutputString(gST->ConOut, w);
    }
    serial_init();
    con_puts("[uefi] serial log: ");
    con_puts(g_serial_mode == 1 ? "EFI Serial I/O" :
             g_serial_mode == 2 ? "UARTC direct" : "none (ConOut only)");
    con_puts(g_uart_ok ? "; kernel uartc: on\n" : "; kernel uartc: off\n");

    gBS->SetWatchdogTimer(0, 0, 0, 0);

    con_puts("\n");
    con_puts("=== EwokOS Orin Nano UEFI loader ===\n");
    con_puts("[uefi] firmware rev ");
    con_hex(SystemTable->Revision);
    con_puts("\n");

    if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, (EFI_GUID*)&li_guid,
                    (void**)&gImageInfo)))
        gImageInfo = 0;
    g_boot_source = gImageInfo ? device_source(gImageInfo->DeviceHandle) :
            ORIN_BOOT_SOURCE_UNKNOWN;
    con_puts("[uefi] boot source=");
    con_puts(source_name(g_boot_source));
    con_puts("\n");
    if (gImageInfo) {
        con_puts("[uefi] dbg: ImageBase=");
        con_hex((uint64_t)(uintptr_t)gImageInfo->ImageBase);
        con_puts(" ImageSize=");
        con_hex((uint64_t)gImageInfo->ImageSize);
        con_puts("\n");
    }

    /* ---- framebuffer: adopt the firmware GOP when it is a real linear
     * FB (display attached).  Headless boots have no GOP or a base-0 proxy;
     * both are tolerated and the kernel simply gets fb_addr=0. ---- */
    uint64_t fb_addr = 0, fb_pitch = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_bpp = 32, fb_format = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = 0;
    if (!EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&gop_guid, 0,
                    (void**)&gop)) && gop && gop->Mode && gop->Mode->Info &&
            gop->Mode->FrameBufferBase != 0 &&
            gop->Mode->FrameBufferSize != 0 &&
            gop->Mode->Info->PixelFormat <= 2) {
        EFI_GOP_MODE_INFO* info = gop->Mode->Info;
        fb_addr = gop->Mode->FrameBufferBase;
        fb_w = info->HorizontalResolution;
        fb_h = info->VerticalResolution;
        fb_pitch = (uint64_t)info->PixelsPerScanLine * 4;
        fb_format = info->PixelFormat;
        if (fb_format == 2) {
            uint32_t mask = info->PixelInformation[0] |
                            info->PixelInformation[1] |
                            info->PixelInformation[2];
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
        con_puts(" @");
        con_hex(fb_addr);
        con_puts("\n");
    } else {
        con_puts("[uefi] no usable linear framebuffer (serial-only)\n");
    }

    /* ---- firmware device tree: record it and extract /model ---- */
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

    /* The bootinfo page is written after EBS too.  It sits just below the
     * kernel window and is not covered by the kernel reservation, so claim
     * it explicitly; otherwise the firmware may leave it read-only. */
    EFI_PHYSICAL_ADDRESS bdst = alloc_pages_at(ORIN_BOOTINFO_PHYS,
            ORIN_BOOTINFO_SIZE / EFI_PAGE_SIZE, EfiLoaderData);
    if (bdst == ORIN_BOOTINFO_PHYS) {
        con_puts("[uefi] bootinfo window reserved at ");
        con_hex(ORIN_BOOTINFO_PHYS);
        con_puts("\n");
    } else {
        con_puts("[uefi] bootinfo window not reservable\n");
    }

    /* Reserve the kernel window before any large file allocation so the
     * initramfs buffer cannot be placed on top of it.  EfiLoaderData keeps the
     * raw allocation writable (for the kernel/trampoline memcpy) and identity-
     * mapped (VA==PA) at EL2 -- unlike a PE image, a raw AllocatePages region
     * is not section-remapped, so the MMU-off handoff trampoline at its tail
     * can be entered with the firmware MMU still on. */
    EFI_PHYSICAL_ADDRESS kdst = alloc_pages_at(ORIN_KERNEL_PHYS,
            ORIN_KERNEL_RESERVE / EFI_PAGE_SIZE, EfiLoaderData);
    if (kdst == ORIN_KERNEL_PHYS) {
        con_puts("[uefi] kernel window reserved at ");
        con_hex(ORIN_KERNEL_PHYS);
        con_puts("\n");
    } else {
        con_puts("[uefi] kernel window not reservable; will check overlap\n");
    }

    /* ---- kernel + boot ramfs ---- */
    uint64_t ksize = 0, rsize = 0;
    void* kbuf = read_first(0, &ksize);
    if (kbuf == 0) {
        con_puts("[uefi] FATAL: KERNEL.BIN not found\n");
        halt_forever();
    }
    if (ksize > ORIN_KERNEL_RESERVE) {
        con_puts("[uefi] FATAL: kernel too large\n");
        halt_forever();
    }

    void* rbuf = read_first(1, &rsize);
    if (rbuf == 0) {
        con_puts("[uefi] no INITRAMFS.IMG, trying EFI LoadFile2 initrd\n");
        rbuf = read_initrd_loadfile2(&rsize);
    }
    if (rbuf == 0) {
        con_puts("[uefi] FATAL: INITRAMFS.IMG not found\n");
        halt_forever();
    }
    con_puts("[uefi] boot ramfs ");
    con_dec(rsize / (1024 * 1024));
    con_puts(" MB @");
    con_hex((uint64_t)(uintptr_t)rbuf);
    con_puts("\n");

    /* Ensure the fixed kernel window does not overlap the stub or the
     * already-loaded file buffers. */
    {
        uint64_t dst = ORIN_KERNEL_PHYS;
        uint64_t dst_end = dst + ORIN_KERNEL_RESERVE;
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
    memcpy((void*)(uintptr_t)ORIN_KERNEL_PHYS, kbuf, ksize);
    con_puts("[uefi] dbg: kernel copied\n");
    install_trampoline();
    con_puts("[uefi] dbg: tramp installed\n");
    make_kernel_window_executable();
    con_puts("[uefi] dbg: exec attr set\n");

    /* ---- final memory map (must be the last Boot Service call before EBS) ---- */
    if (get_memory_map() != 0) {
        con_puts("[uefi] FATAL: GetMemoryMap failed\n");
        halt_forever();
    }
    con_puts("[uefi] dbg: memmap ok\n");

    uint64_t ram_size = memory_ram_size();

    /* Copy the final memory map into the bootinfo page (physical memory). */
    orin_bootinfo_t* bi = (orin_bootinfo_t*)(uintptr_t)ORIN_BOOTINFO_PHYS;
    uint8_t* map_dst = (uint8_t*)(uintptr_t)(ORIN_BOOTINFO_PHYS + 0x1000);
    uint64_t map_cap = ORIN_BOOTINFO_SIZE - 0x1000;
    uint64_t map_copy = g_memmap_size;
    if (map_copy > map_cap)
        map_copy = map_cap;

    memset(bi, 0, sizeof(*bi));
    memcpy(map_dst, g_memmap, map_copy);

    bi->magic = ORIN_BOOTINFO_MAGIC;
    bi->version = ORIN_BOOTINFO_VERSION;
    bi->ram_base = ORIN_RAM_BASE;
    bi->ram_size = ram_size;
    bi->fb_addr = fb_addr;
    bi->fb_width = fb_w;
    bi->fb_height = fb_h;
    bi->fb_pitch = (uint32_t)fb_pitch;
    bi->fb_bpp = fb_bpp;
    bi->fb_format = fb_format;
    bi->_pad_fb = 0;
    bi->serial_flags = g_uart_ok ? 1u : 0u;
    bi->rd_base = (uint64_t)(uintptr_t)rbuf;
    bi->rd_size = rsize;
    bi->rd_flags = ORIN_RD_FLAG_BOOT_ONLY;
    bi->boot_source = g_boot_source;
    bi->kernel_phys = ORIN_KERNEL_PHYS;
    bi->kernel_size = ksize;
    bi->memmap_addr = (uint64_t)(uintptr_t)map_dst;
    bi->memmap_count = (uint32_t)(map_copy / g_memmap_dsize);
    bi->memmap_dsize = (uint32_t)g_memmap_dsize;
    bi->dtb_addr = dtb_addr;
    bi->dtb_size = dtb_size;
    memcpy(bi->model, board_model, sizeof(bi->model));

    /* Record firmware-reserved ranges inside the usable window so the
     * kernel allocator can punch them out.  Tegra firmware carves out many
     * regions (BPMP/SPE/TCU carveouts, EFI runtime, ACPI); the descriptors
     * are address-sorted, so merge adjacent ones to stay under the cap. */
    uint32_t count = (uint32_t)(g_memmap_size / g_memmap_dsize);
    uint64_t limit = ORIN_RAM_BASE + ORIN_USABLE_LIMIT;
    for (uint32_t i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR* d =
            (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)g_memmap + i * g_memmap_dsize);
        uint64_t start = d->PhysicalStart;
        uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
        if (mem_usable(d->Type))
            continue;
        if (end <= ORIN_RAM_BASE || start >= limit)
            continue;
        if (start < ORIN_RAM_BASE)
            start = ORIN_RAM_BASE;
        if (end > limit)
            end = limit;
        if (bi->resv_count > 0) {
            orin_resv_t* last = &bi->resv[bi->resv_count - 1];
            if (last->base + last->size == start) {
                last->size = end - last->base;
                continue;
            }
        }
        if (bi->resv_count >= ORIN_MAX_RESV) {
            con_puts("[uefi] WARN: resv table full, further firmware "
                     "reservations dropped\n");
            break;
        }
        bi->resv[bi->resv_count].base = start;
        bi->resv[bi->resv_count].size = end - start;
        bi->resv_count++;
    }
    con_puts("[uefi] ram ");
    con_dec(ram_size / (1024 * 1024));
    con_puts(" MB, resv ");
    con_dec(bi->resv_count);
    con_puts(" regions\n");

    con_puts("[uefi] handoff trampoline @");
    con_hex(ORIN_KERNEL_PHYS + ORIN_TRAMP_OFF);
    con_puts("\n");

    /* Make the copied kernel/rootfs/bootinfo visible to a boot CPU that
     * starts with MMU and caches off. */
    cache_clean((void*)(uintptr_t)ORIN_KERNEL_PHYS, ORIN_KERNEL_RESERVE);
    cache_clean((void*)(uintptr_t)ORIN_BOOTINFO_PHYS, ORIN_BOOTINFO_SIZE);
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

    /* No raw UARTC/WDT poke here: those MMIO ranges may be unmapped in the
     * firmware MMU and a bare access would abort.  The kernel maps them in its
     * own MMU and arms the watchdog safely (arch_kernel_entry_early). */
    jump_via_trampoline(ORIN_KERNEL_PHYS);
}