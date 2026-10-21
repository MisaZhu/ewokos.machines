/*
 * BOOTAA64.EFI — EwokOS Q8B UEFI bootloader.
 *
 * The Radxa Dragon Q8B has no U-Boot: its SPI NOR EDK2 firmware loads this
 * PE32+ application (Boot From File / UEFI Shell / PXE).  We then:
 *
 *   1. locate the GOP framebuffer and record it for user space,
 *   2. load KERNEL.BIN and, for RAM-disk boots, ROOTFS.IMG either from a
 *      SimpleFileSystem volume (ESP/USB), EFI_LOAD_FILE (PXE TFTP), or a GRUB
 *      initrd exposed as EFI_LOAD_FILE2; SD_ROOTFS boots leave the rootfs
 *      empty so the kernel can read the ext3 partition through SDCC2,
 *   3. reserve and fill the kernel at Q8B_KERNEL_PHYS,
 *   4. collect the final UEFI memory map, ExitBootServices,
 *   5. publish the bootinfo page at Q8B_BOOTINFO_PHYS and jump to the kernel.
 *
 * Calling convention: AArch64 UEFI is AAPCS64, identical to the compiler's
 * normal convention, so no assembly shim is required.
 */
#include "efi.h"

/* Must match machines/q8b/kernel/config.mk, mkos.lds.S and bsp/hw_arch.h.
 *
 * Q8B_QEMU_TEST builds the same loader for a QEMU virt smoke test: only
 * the DRAM base and the fixed bootinfo/kernel physical addresses change,
 * so the SFS -> ExitBootServices -> jump path can be exercised without a
 * board.  The mock payload is boot/uefi/test/mock_kernel.S. */
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
#define Q8B_ROOTFS_MTFTP_MAX (160ULL * 1024 * 1024)
#define Q8B_BOOTINFO_SIZE   0x10000ULL
#define Q8B_USABLE_LIMIT    (8ULL * 1024 * 1024 * 1024)

#define Q8B_BOOTINFO_MAGIC   0x42385145u
#define Q8B_BOOTINFO_VERSION 1

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

    uint64_t kernel_phys;
    uint64_t kernel_size;

    uint64_t memmap_addr;
    uint32_t memmap_count;
    uint32_t memmap_dsize;

    uint32_t resv_count;
    uint32_t _pad0;
    q8b_resv_t resv[Q8B_MAX_RESV];

    uint64_t dtb_addr;
    uint64_t dtb_size;
} q8b_bootinfo_t;

static EFI_SYSTEM_TABLE* gST;
static EFI_BOOT_SERVICES* gBS;
static EFI_HANDLE gImage;
static EFI_LOADED_IMAGE_PROTOCOL* gImageInfo;
static int g_con_ok;
static EFI_SERIAL_IO_PROTOCOL* g_serial;
static int g_serial_mode;      /* 0=none, 1=EFI Serial I/O, 2=UART17 direct */

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

/* UEFI already brings up the QUP clock, TLMM pinmux and 115200 baud.
 * Only normalise the packet format and TX path, exactly like the kernel's
 * bsp/uart.c + bsp/dbg.c do after ExitBootServices. */
static void uart17_putc(char c) {
    uart_wr(0x80c, 2);
    uart_wr(0x618, (1u << 0) | (1u << 5) | (1u << 30));
    uart_wr(0x270, 1);
    uart_wr(0x600, 1u << 27);  /* UART_START_TX */
    if (uart_poll(0x610, 1u << 30, 1) != 0) {
        uart_wr(0x604, 1u << 1);
        uart_wr(0x618, 1u << 5);
        return;
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
    }
    uart_wr(0x618, 1u << 0);
}
#endif /* !Q8B_QEMU_TEST */

static void serial_init(void) {
    g_serial = 0;
    g_serial_mode = 0;
#ifndef Q8B_QEMU_TEST
    /* UEFI BIOS已经初始化好 UART17。loader 不再碰任何 UART 配置，
     * 只用 EFI Serial I/O 的 Write（或 UART17 的 TX 命令）输出字符串。 */
#ifdef Q8B_SERIAL_FORCE_UART17
    g_serial_mode = 2;       /* raw TX only, no init */
    return;
#endif
    static const EFI_GUID serial_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    if (!EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&serial_guid, 0,
                    (void**)&g_serial)) && g_serial) {
        g_serial_mode = 1;
        return;              /* no SetAttributes: BIOS already did it */
    }
    g_serial_mode = 2;       /* raw TX only, no init */
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

static void con_puts(const char* ascii) {
    /* On the real board the UEFI ConOut is also wired to the same debug
     * UART, so writing both duplicates every line.  With a serial backend
     * (EFI Serial I/O or UART17 direct) use it exclusively; QEMU has no
     * serial backend (g_serial_mode==0) and keeps the ConOut path. */
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

/* SFS: read a file from every SimpleFileSystem volume we can find. */
static void* read_file_sfs(const char* path, uint64_t* out_size) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE* handles = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(2, &sfs_guid, 0, &n, &handles)))
        return 0;

    CHAR16 wpath[256];
    ascii_to_w(path, wpath, sizeof(wpath) / sizeof(wpath[0]));

    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &sfs_guid,
                        (void**)&sfs)))
            continue;
        EFI_FILE_PROTOCOL* root = 0;
        if (EFI_ERROR(sfs->OpenVolume(sfs, (void**)&root)))
            continue;
        EFI_FILE_PROTOCOL* f = 0;
        if (EFI_ERROR(root->Open(root, &f, wpath, EFI_FILE_MODE_READ, 0)))
            continue;

        static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
        uint8_t info[256];
        UINTN info_size = sizeof(info);
        if (EFI_ERROR(f->GetInfo(f, &fi_guid, &info_size, info))) {
            f->Close(f);
            continue;
        }
        uint64_t fsize = ((EFI_FILE_INFO_HEAD*)info)->FileSize;

        /* The Q8B kernel uses 16KB pages; every physical range it maps
         * through the page tables must be 16KB aligned, but UEFI only
         * guarantees 4KB page granularity.  Allocate one extra page and
         * round the buffer up to 16KB. */
        UINTN pages = (fsize + 0x3FFF + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
        EFI_PHYSICAL_ADDRESS buf = alloc_pages(pages);
        if (buf == 0) {
            f->Close(f);
            continue;
        }
        uint64_t abuf = (buf + 0x3FFF) & ~0x3FFFULL;
        UINTN rsz = fsize;
        EFI_STATUS st = f->Read(f, &rsz, (void*)(uintptr_t)abuf);
        f->Close(f);
        if (EFI_ERROR(st))
            continue;
        *out_size = fsize;
        return (void*)(uintptr_t)abuf;
    }
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
    } else {                    /* rootfs */
        switch (i) {
        case 0: return "\\q8b\\ROOTFS.IMG";
        case 1: return "\\ROOTFS.IMG";
        case 2: return "\\EFI\\BOOT\\ROOTFS.IMG";
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
    } else {
        switch (i) {
        case 0: return "q8b/ROOTFS.IMG";
        case 1: return "ROOTFS.IMG";
        case 2: return "EFI/BOOT/ROOTFS.IMG";
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

static void* read_first(int which, uint64_t* size) {
#ifdef Q8B_MTFTP_FIRST
    /* PXE-first build: fetch the kernel from TFTP before the local ESP so a
     * kernel under test can be replaced without rewriting the SD card. */
    /* Fast path: raw TFTP over PXE UDP (blksize + windowsize).  Falls back
     * to the firmware's MTFTP4 if the server or firmware rejects it. */
    {
        const char* fp = pxe_path_name(which, 0);
        if (fp != 0) {
            void* p = read_file_pxe_udp(fp,
                    which == 0 ? Q8B_KERNEL_MTFTP_MAX : Q8B_ROOTFS_MTFTP_MAX,
                    size);
            if (p) {
                con_puts("[uefi]   UDP TFTP first ok, ");
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
        void* p = read_file_mtftp4(ppath,
                which == 0 ? Q8B_KERNEL_MTFTP_MAX : Q8B_ROOTFS_MTFTP_MAX,
                size);
        if (p) {
            con_puts("[uefi]   MTFTP4 first ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
    }

    /* Some Q8B firmware exposes PXE only through EFI_LOAD_FILE or the
     * legacy PXE base-code protocol, not MTFTP4.  Keep these network paths
     * ahead of SFS as well; otherwise a stale SD kernel silently wins. */
    for (int i = 0; i < 3; i++) {
        const char* path = path_name(which, i);
        if (path == 0)
            break;
        void* p = read_file_loadfile(path, size);
        if (p) {
            con_puts("[uefi]   PXE LoadFile first ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
        const char* ppath = pxe_path_name(which, i);
        if (ppath) {
            p = read_file_pxe(ppath, size);
            if (p) {
                con_puts("[uefi]   PXE Mtftp first ok, ");
                con_dec(*size);
                con_puts(" bytes\n");
                return p;
            }
        }
    }
#endif
    /* Phase 1: the local EFI system partition (SD ESP, USB ESP, QEMU test
     * image).  This is the normal boot media: the board should load the
     * kernel it carries without waiting for a TFTP timeout.  If the file is
     * absent here, fall through to the network so a PXE update loop still
     * works.  When updating over PXE, refresh KERNEL.BIN on the ESP too or
     * temporarily remove it, otherwise the stale local copy wins. */
    for (int i = 0; i < 3; i++) {
        const char* path = path_name(which, i);
        if (path == 0)
            break;
        con_puts("[uefi] try ");
        con_puts(path);
        con_puts("\n");
        void* p = read_file_sfs(path, size);
        if (p) {
            con_puts("[uefi]   SFS ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
    }

    /* Phase 2: network paths (MTFTP4).  Reached only when the EFI partition
     * does not carry the file. */
    for (int i = 0; i < 3; i++) {
        const char* ppath = pxe_path_name(which, i);
        if (ppath == 0)
            break;
        void* p = read_file_mtftp4(ppath,
                which == 0 ? Q8B_KERNEL_MTFTP_MAX : Q8B_ROOTFS_MTFTP_MAX,
                size);
        if (p) {
            con_puts("[uefi]   MTFTP4 ok, ");
            con_dec(*size);
            con_puts(" bytes\n");
            return p;
        }
    }

    /* Phase 3: legacy LoadFile / PXE-BC Mtftp fallbacks. */
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
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    static const EFI_GUID dtb_guid = EFI_DTB_TABLE_GUID;

    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImage = ImageHandle;
    g_con_ok = 1;
    serial_init();
    con_puts("[uefi] serial log: ");
    con_puts(g_serial_mode == 1 ? "EFI Serial I/O" :
             g_serial_mode == 2 ? "UART17 direct" : "disabled");
    con_puts("\n");

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

    /* ---- framebuffer ---- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = 0;
    uint64_t fb_addr = 0, fb_pitch = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_bpp = 0, fb_format = 0;

    if (!EFI_ERROR(gBS->LocateProtocol((EFI_GUID*)&gop_guid, 0,
                    (void**)&gop)) && gop && gop->Mode && gop->Mode->Info) {
        EFI_GOP_MODE_INFO* mi = gop->Mode->Info;
        fb_addr = gop->Mode->FrameBufferBase;
        fb_w = mi->HorizontalResolution;
        fb_h = mi->VerticalResolution;
        fb_pitch = (uint64_t)mi->PixelsPerScanLine * 4;
        fb_bpp = 32;
        fb_format = mi->PixelFormat;
        if (mi->PixelFormat == 2) {
            /* PixelBitMask: derive the depth from the mask widths. */
            uint32_t mask = mi->PixelInformation[0] | mi->PixelInformation[1] |
                            mi->PixelInformation[2];
            uint32_t bits = 0;
            while (mask) {
                bits += mask & 1;
                mask >>= 1;
            }
            if (bits > 0 && bits <= 32)
                fb_bpp = bits;
        }
        con_puts("[uefi] gop ");
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
    } else {
        con_puts("[uefi] no GOP (headless?)\n");
    }

    /* ---- device tree (informational for now) ---- */
    uint64_t dtb_addr = 0, dtb_size = 0;
    if (SystemTable->ConfigurationTable) {
        for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE* ct = &SystemTable->ConfigurationTable[i];
            if (guid_eq(&ct->VendorGuid, (EFI_GUID*)&dtb_guid)) {
                dtb_addr = (uint64_t)(uintptr_t)ct->VendorTable;
                if (dtb_addr) {
                    uint32_t totalsize = *(volatile uint32_t*)(uintptr_t)
                        (dtb_addr + 4);
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
#if defined(Q8B_SD_ROOTFS) || defined(Q8B_KERNEL_ONLY)
    /* Physical-SD rootfs boot: do not allocate or fetch a RAM disk.  The
     * kernel will initialize SDCC2 after ExitBootServices and select the
     * supported ext2/3 GPT partition itself. */
#ifdef Q8B_SD_ROOTFS
    con_puts("[uefi] SD_ROOTFS: kernel will load rootfs from SDCC2\n");
#else
    con_puts("[uefi] KERNEL_ONLY: skipping ROOTFS.IMG\n");
#endif
#else
    rbuf = read_first(1, &rsize);
    if (rbuf == 0) {
        con_puts("[uefi] rootfs not found, trying EFI LoadFile2 initrd\n");
        rbuf = read_initrd_loadfile2(&rsize);
    }
    if (rbuf == 0) {
        con_puts("[uefi] FATAL: ROOTFS.IMG not found\n");
        halt_forever();
    }
    con_puts("[uefi] rootfs ");
    con_dec(rsize / (1024 * 1024));
    con_puts(" MB @");
    con_hex((uint64_t)(uintptr_t)rbuf);
    con_puts("\n");
#endif

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
    bi->fb_width = fb_w;
    bi->fb_height = fb_h;
    bi->fb_pitch = (uint32_t)fb_pitch;
    bi->fb_bpp = fb_bpp;
    bi->fb_format = fb_format;
    bi->rd_base = (uint64_t)(uintptr_t)rbuf;
    bi->rd_size = rsize;
    bi->kernel_phys = Q8B_KERNEL_PHYS;
    bi->kernel_size = ksize;
    bi->memmap_addr = (uint64_t)(uintptr_t)map_dst;
    bi->memmap_count = (uint32_t)(map_copy / g_memmap_dsize);
    bi->memmap_dsize = (uint32_t)g_memmap_dsize;
    bi->dtb_addr = dtb_addr;
    bi->dtb_size = dtb_size;
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
#ifndef Q8B_QEMU_TEST
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
