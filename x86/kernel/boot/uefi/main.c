/* main.c — EwokOS x86 UEFI bootloader (BOOTX64.EFI)
 * 移植自 kernel-testsuite 已验证的 PE32+ bootloader。
 *
 * 与内核的交接约定 (参考 Linux 固定加载地址的启动流程):
 *   1. 经 EFI SimpleFileSystem 读取 \EFI\BOOT\KERNEL.ELF
 *   2. 按 ELF64 program header 的 p_paddr (物理 LMA) 放置段 —— 内核固定
 *      加载在物理 0x100000, 与 BIOS 路径/phy_offset/AP trampoline 完全一致
 *   3. 从 section header 找 ".uefi" 段: 首个 quad 为内核早期页表
 *      (boot_pml4), +8 处为 64 位 UEFI 入口 uefi_entry
 *   4. 收集 EFI 内存映射 + GOP framebuffer → bootinfo
 *   5. ExitBootServices 后 jmp uefi_entry, rdi = bootinfo 物理地址
 *      (CR3 切换/段装载由 uefi_entry 自己完成)
 */
#include "efi.h"
#include "bootinfo.h"

#define KERNEL_LOAD_PATH_1  "\\EFI\\BOOT\\KERNEL.ELF"
#define KERNEL_LOAD_PATH_2  "\\KERNEL.ELF"
#define ROOTFS_LOAD_PATH_1  "\\ROOTFS.IMG"
#define ROOTFS_LOAD_PATH_2  "\\EFI\\BOOT\\ROOTFS.IMG"
#define UEFI_SECTION_NAME   ".uefi"
#define KERNEL_LOAD_PHYS    0x00100000UL  /* 与 BIOS 路径/phy_offset 一致 */
/* 内存盘 rootfs 分配上限: x86 内核可用 RAM 上限 512MB, ramdisk 须在其内
 * (内核 VM 映射窗口与 MAX_USABLE_MEM_SIZE 约束) */
#define ROOTFS_MAX_PHYS     0x1FFFFFFFULL

/* ---- 端口 IO ---- */
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ---- COM1 16550 串口 ---- */
#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* 禁中断 */
    outb(COM1 + 3, 0x80);   /* DLAB=1 */
    outb(COM1 + 0, 0x01);   /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);
}
static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) {}
    outb(COM1, (uint8_t)c);
}
static EFI_SYSTEM_TABLE *gST;

static void con16(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *c, const char *ascii);

/* 真机无串口: 日志同时镜像到固件控制台 (ConOut, GOP/文本均可见)。
 * _con_ok 在 ExitBootServices 成功后清零 (此后 ConOut 不可用) */
static int _con_ok = 0;

static void sputs(const char *s) {
    while (*s) serial_putc(*s++);
    if (_con_ok) con16(gST->ConOut, s);
}
static void sputhex(uint64_t v) {
    static const char h[] = "0123456789abcdef";
    sputs("0x");
    for (int i = 60; i >= 0; i -= 4) serial_putc(h[(v >> i) & 0xF]);
}
static void sputdec(uint64_t v) {
    char buf[24];
    int i = 23;
    buf[i--] = 0;
    if (!v) buf[i--] = '0';
    while (v) { buf[i--] = '0' + (v % 10); v /= 10; }
    sputs(&buf[i + 1]);
}

static void con16(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *c, const char *ascii) {
    CHAR16 w[512];
    uint64_t i = 0, j = 0;
    for (; ascii[i] && j < 505; i++) {
        if (ascii[i] == '\n') w[j++] = (CHAR16)'\r';   /* ConOut 换行需回车 */
        w[j++] = (CHAR16)ascii[i];
    }
    w[j] = 0;
    {
        uint64_t st = (uint64_t)efi_output_string(c, w);
        if (st >> 63) {                          /* EFI_ERROR: 串口报错并停用 */
            sputs("[UEFI] ConOut err=");
            sputhex(st);
            sputs("\n");
            _con_ok = 0;
        }
    }
}

void *efi_memcpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *efi_memset(void *d, uint8_t v, uint64_t n) {
    uint8_t *dd = d;
    while (n--) *dd++ = v;
    return d;
}

static EFI_BOOT_SERVICES *BS;


/* gcc 的 L"..." 在 ELF 上是 4 字节 wchar_t, UEFI 要 UTF-16 → 运行时转换 */
static CHAR16 wpath[128];
static CHAR16 *wstr(const char *s) {
    uint64_t i = 0;
    for (; s[i]; i++) wpath[i] = (CHAR16)s[i];
    wpath[i] = 0;
    return wpath;
}

/* ================= ELF64 ================= */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;
typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr_t;
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;  /* Elf64_Shdr, 64 字节 */

#define PT_LOAD 1
#define SHT_NOBITS 8

/* 读文件全文到新分配的 EfiLoaderData 页 */
static uint8_t *read_file(const CHAR16 *path, uint64_t *out_size) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    uint64_t n_handles = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS st = (EFI_STATUS)efi_call5((void *)BS->LocateHandleBuffer,
        2 /*ByProtocol*/, (uint64_t)&sfs_guid, 0, (uint64_t)&n_handles,
        (uint64_t)&handles);
    if (EFI_ERROR(st)) {
        sputs("[UEFI] no SimpleFileSystem handles\n");
        return 0;
    }
    sputs("[UEFI] fs handles="); sputdec(n_handles); sputs("\n");

    for (uint64_t i = 0; i < n_handles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
        if (EFI_ERROR(efi_handle_protocol(BS, handles[i], &sfs_guid, (void **)&sfs)))
            continue;
        EFI_FILE_PROTOCOL *root = 0;
        if (EFI_ERROR((EFI_STATUS)efi_call2((void *)sfs->OpenVolume,
                (uint64_t)sfs, (uint64_t)&root)))
            continue;
        EFI_FILE_PROTOCOL *f = 0;
        st = (EFI_STATUS)efi_call5((void *)root->Open, (uint64_t)root,
            (uint64_t)&f, (uint64_t)path, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(st)) continue;

        /* 文件大小: GetInfo(EFI_FILE_INFO) */
        static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
        uint8_t info_buf[256];
        uint64_t info_size = sizeof(info_buf);
        st = (EFI_STATUS)efi_call4((void *)f->GetInfo, (uint64_t)f,
            (uint64_t)&fi_guid, (uint64_t)&info_size, (uint64_t)info_buf);
        if (EFI_ERROR(st)) continue;
        uint64_t fsize = *(uint64_t *)(info_buf + 8);   /* EFI_FILE_INFO.FileSize */

        uint64_t pages = (fsize + 0xFFF) / 0x1000;
        uint64_t buf = 0;
        st = (EFI_STATUS)efi_call4((void *)BS->AllocatePages, AllocateAnyPages,
            EfiLoaderData, pages, (uint64_t)&buf);
        if (EFI_ERROR(st)) continue;

        uint64_t rsz = fsize;
        st = (EFI_STATUS)efi_call3((void *)f->Read, (uint64_t)f,
            (uint64_t)&rsz, buf);
        if (EFI_ERROR(st)) continue;
        efi_call1((void *)f->Close, (uint64_t)f);
        *out_size = fsize;
        return (uint8_t *)buf;
    }
    return 0;
}

/* 读文件到 "物理地址 ≤ max_addr" 的新分配页 (内存盘 rootfs 用:
 * 必须落在内核映射窗口 <512MB 内)。SFS 路径; 失败返回 0。 */
static uint8_t *read_file_at(const CHAR16 *path, uint64_t max_addr,
                             uint64_t *out_size) {
    static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    uint64_t n_handles = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS st = (EFI_STATUS)efi_call5((void *)BS->LocateHandleBuffer,
        2 /*ByProtocol*/, (uint64_t)&sfs_guid, 0, (uint64_t)&n_handles,
        (uint64_t)&handles);
    if (EFI_ERROR(st)) return 0;

    for (uint64_t i = 0; i < n_handles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
        if (EFI_ERROR(efi_handle_protocol(BS, handles[i], &sfs_guid, (void **)&sfs)))
            continue;
        EFI_FILE_PROTOCOL *root = 0;
        if (EFI_ERROR((EFI_STATUS)efi_call2((void *)sfs->OpenVolume,
                (uint64_t)sfs, (uint64_t)&root)))
            continue;
        EFI_FILE_PROTOCOL *f = 0;
        st = (EFI_STATUS)efi_call5((void *)root->Open, (uint64_t)root,
            (uint64_t)&f, (uint64_t)path, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(st)) continue;

        static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
        uint8_t info_buf[256];
        uint64_t info_size = sizeof(info_buf);
        st = (EFI_STATUS)efi_call4((void *)f->GetInfo, (uint64_t)f,
            (uint64_t)&fi_guid, (uint64_t)&info_size, (uint64_t)info_buf);
        if (EFI_ERROR(st)) continue;
        uint64_t fsize = *(uint64_t *)(info_buf + 8);

        /* +2MB 余量: 分配后把基址向上圆整到 2MB (内核按 2MB 大页映射
         * 内存盘访问窗口, 见 bsp/x86_bootinfo.h; 4K 对齐的裸分配可能
         * 落在 2MB 边界中间, 内核态读会整体偏移) */
        uint64_t pages = (fsize + 0x200000ULL + 0xFFF) / 0x1000;
        uint64_t buf = max_addr;
        st = (EFI_STATUS)efi_call4((void *)BS->AllocatePages, AllocateMaxAddress,
            EfiLoaderData, pages, (uint64_t)&buf);
        if (EFI_ERROR(st)) continue;
        buf = (buf + 0x1FFFFF) & ~0x1FFFFFULL;

        uint64_t rsz = fsize;
        st = (EFI_STATUS)efi_call3((void *)f->Read, (uint64_t)f,
            (uint64_t)&rsz, buf);
        if (EFI_ERROR(st)) continue;
        efi_call1((void *)f->Close, (uint64_t)f);
        *out_size = fsize;
        return (uint8_t *)buf;
    }
    return 0;
}

/* ================= ISO9660 直读 (BlockIo 兜底) =================
 * Ventoy UEFI 把 ISO 映射为块设备但未必提供 ISO9660 文件系统驱动;
 * 这里按裸块读 PVD/根目录, 与生成端 (mkboot.py iso) 的平面布局配合。 */
#define ISO_SECTOR 2048
#define ISO_PVD_LBA 16

static int iso_read_blocks(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba,
                           uint32_t n, uint8_t *buf) {
    uint64_t sz = (uint64_t)n * ISO_SECTOR;
    return (EFI_STATUS)efi_call5((void *)bio->ReadBlocks, (uint64_t)bio,
        (uint64_t)bio->Media->MediaId, lba, sz, (uint64_t)buf) != 0;
}

/* 找第一个 2048 字节块且非分区的 BlockIo handle (即整张 CD/ISO) */
static EFI_BLOCK_IO_PROTOCOL *iso_find_bio(void) {
    static EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    uint64_t n_handles = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS st = (EFI_STATUS)efi_call5((void *)BS->LocateHandleBuffer,
        2 /*ByProtocol*/, (uint64_t)&bio_guid, 0, (uint64_t)&n_handles,
        (uint64_t)&handles);
    if (EFI_ERROR(st)) return 0;
    for (uint64_t i = 0; i < n_handles; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (EFI_ERROR(efi_handle_protocol(BS, handles[i], &bio_guid, (void **)&bio)))
            continue;
        if (bio->Media == 0 || !bio->Media->MediaPresent ||
                bio->Media->LogicalPartition || bio->Media->BlockSize != ISO_SECTOR)
            continue;
        return bio;
    }
    return 0;
}

/* 目录记录内文件名与目标比较 (忽略大小写与 ";版本号") */
static int iso_name_eq(const uint8_t *name, uint32_t len, const char *want) {
    uint32_t nlen = len;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == ';') {
            nlen = i;              /* 去掉 ";1" 版本号后缀 */
            break;
        }
    }
    uint32_t wlen = 0;
    while (want[wlen]) wlen++;
    if (nlen != wlen) return 0;
    for (uint32_t i = 0; i < nlen; i++) {
        char c = (char)name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        char w = want[i];
        if (w >= 'a' && w <= 'z') w -= 32;
        if (c != w) return 0;
    }
    return 1;
}

typedef struct {
    uint64_t extent;   /* 起始 LBA */
    uint64_t size;     /* 字节 */
} iso_file_t;

/* 在目录 extent 中找文件 (一级扁平查找) */
static int iso_find_in_dir(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t dir_extent,
                           uint64_t dir_size, const char *name, iso_file_t *out) {
    static uint8_t sec[ISO_SECTOR];
    uint64_t nsec = (dir_size + ISO_SECTOR - 1) / ISO_SECTOR;
    for (uint64_t s = 0; s < nsec; s++) {
        if (iso_read_blocks(bio, dir_extent + s, 1, sec)) return -1;
        uint32_t off = 0;
        while (off < ISO_SECTOR) {
            uint8_t len = sec[off];
            if (len == 0) break;   /* 记录不跨扇区 (生成端保证) */
            /* len,ext_lba@+2,size@+10,flags@+25,name_len@+32,name@+33 */
            if (len >= 34 && !(sec[off + 25] & 0x02) &&
                    iso_name_eq(sec + off + 33, sec[off + 32], name)) {
                out->extent = *(const uint32_t *)(sec + off + 2);
                out->size = *(const uint32_t *)(sec + off + 10);
                return 0;
            }
            off += len;
        }
    }
    return -1;
}

/* ISO9660 根目录读文件全文到 ≤ max_addr 的页; 失败返回 0 */
static uint8_t *iso9660_read_file(const char *name, uint64_t max_addr,
                                  uint64_t *out_size) {
    EFI_BLOCK_IO_PROTOCOL *bio = iso_find_bio();
        sputs("[UEFI] iso9660: BlockIo search "); sputs(name); sputs("\n");
    if (bio == 0) {
        sputs("[UEFI] iso9660: no 2048B BlockIo\n");
        return 0;
    }
    static uint8_t pvd[ISO_SECTOR];
    if (iso_read_blocks(bio, ISO_PVD_LBA, 1, pvd)) return 0;
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' ||
            pvd[4] != '0' || pvd[5] != '1') {
        sputs("[UEFI] iso9660: no PVD\n");
        return 0;
    }
    uint64_t root_extent = *(const uint32_t *)(pvd + 156 + 2);
    uint64_t root_size = *(const uint32_t *)(pvd + 156 + 10);

    iso_file_t f;
    if (iso_find_in_dir(bio, root_extent, root_size, name, &f) != 0) {
        sputs("[UEFI] iso9660: "); sputs(name); sputs(" not found\n");
        return 0;
    }

    /* +2MB 余量: 分配后把基址向上圆整到 2MB (同 read_file_at) */
    uint64_t pages = (f.size + 0x200000ULL + 0xFFF) / 0x1000;
    uint64_t buf = max_addr;
    if (EFI_ERROR((EFI_STATUS)efi_call4((void *)BS->AllocatePages,
            AllocateMaxAddress, EfiLoaderData, pages, (uint64_t)&buf))) {
        sputs("[UEFI] iso9660: alloc failed\n");
        return 0;
    }
    buf = (buf + 0x1FFFFF) & ~0x1FFFFFULL;
    uint64_t nsec = (f.size + ISO_SECTOR - 1) / ISO_SECTOR;
    uint64_t done = 0;
    while (nsec > 0) {
        uint32_t batch = (nsec > 64) ? 64 : (uint32_t)nsec;
        if (iso_read_blocks(bio, f.extent + done / ISO_SECTOR, batch,
                (uint8_t *)buf + done)) {
            sputs("[UEFI] iso9660: read error\n");
            return 0;
        }
        done += (uint64_t)batch * ISO_SECTOR;
        nsec -= batch;
    }
    *out_size = f.size;
    return (uint8_t *)buf;
}

/* 按 p_paddr (物理 LMA) 放置 PT_LOAD 段。
 * 内核必须位于物理 0x100000 (与 BIOS 路径/phy_offset 约定一致)。
 * OVMF 常占用低地址 1MB 附近, AllocateAddress 会失败 —— 此时退化为
 * 任意地址暂存, ExitBootServices 之后(EBS 后内存归 OS 所有)再拷到
 * 0x100000, 与 Linux EFI stub 的处理方式相同。
 * 返回: 暂存基址; *need_post_copy=1 表示 EBS 后需拷贝到 *target。 */
static uint64_t elf_load(uint8_t *img, uint64_t *out_span,
                         int *need_post_copy, uint64_t *target) {
    elf64_ehdr_t *eh = (elf64_ehdr_t *)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_machine != 0x3E) {
        sputs("[UEFI] bad ELF magic/machine\n");
        return 0;
    }
    sputs("[UEFI] kernel elf: phnum="); sputdec(eh->e_phnum);
    sputs(" entry="); sputhex(eh->e_entry); sputs("\n");

    uint64_t min_pa = ~0ULL, max_end = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_paddr == 0) {
            sputs("[UEFI] segment paddr=0 unsupported\n");
            return 0;
        }
        if (ph->p_paddr < min_pa) min_pa = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > max_end) max_end = ph->p_paddr + ph->p_memsz;
    }
    if (min_pa > max_end) return 0;
    if (min_pa != KERNEL_LOAD_PHYS) {
        sputs("[UEFI] warning: kernel paddr "); sputhex(min_pa);
        sputs(" != expected\n");
    }

    uint64_t span = max_end - min_pa;
    uint64_t pages = (span + 0xFFF) / 0x1000;
    uint64_t base = min_pa;
    *need_post_copy = 0;
    *target = min_pa;
    if (EFI_ERROR((EFI_STATUS)efi_call4((void *)BS->AllocatePages, AllocateAddress,
            EfiLoaderData, pages, (uint64_t)(void *)&base))) {
        base = 0;
        if (EFI_ERROR((EFI_STATUS)efi_call4((void *)BS->AllocatePages, AllocateAnyPages,
                EfiLoaderData, pages, (uint64_t)(void *)&base))) {
            sputs("[UEFI] AllocatePages(kernel staging) failed\n");
            return 0;
        }
        *need_post_copy = 1;
        sputs("[UEFI] fixed load blocked by firmware, staging at ");
        sputhex(base); sputs(" (post-EBS copy to "); sputhex(min_pa); sputs(")\n");
    }
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint64_t dst = base + (ph->p_paddr - min_pa);
        efi_memcpy((void *)dst, img + ph->p_offset, ph->p_filesz);
        efi_memset((void *)(dst + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        sputs("[UEFI] seg pa="); sputhex(ph->p_paddr);
        sputs(" va="); sputhex(ph->p_vaddr);
        sputs(" memsz="); sputdec(ph->p_memsz); sputs("\n");
    }
    *out_span = span;
    return base;
}

/* 在内核 ELF 里定位 64 位 UEFI 入口 (uefi_entry):
 * 优先扫 .symtab 符号表(链接后 .uefi 输入段会并入 .init 输出段),
 * 兜底找名为 ".uefi" 的 section header。
 * 布局约定: uefi_header 首个 quad = boot_pml4, uefi_entry = +8。 */
static int efi_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

#define SHT_SYMTAB 2
#define SHT_STRTAB 3

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;   /* Elf64_Sym, 24 字节 */

static uint64_t find_uefi_entry(uint8_t *img) {
    elf64_ehdr_t *eh = (elf64_ehdr_t *)img;
    elf64_shdr_t *shs = (elf64_shdr_t *)(img + eh->e_shoff);
    uint64_t hdr_addr = 0;

    /* ---- 路径1: symtab 符号查找 ---- */
    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        elf64_shdr_t *s = &shs[i];
        if (s->sh_type != SHT_SYMTAB || s->sh_link >= eh->e_shnum) continue;
        const char *strs =
            (const char *)(img + shs[s->sh_link].sh_offset);
        elf64_sym_t *syms = (elf64_sym_t *)(img + s->sh_offset);
        uint32_t n = (uint32_t)(s->sh_size / sizeof(elf64_sym_t));
        for (uint32_t j = 0; j < n; j++) {
            if (syms[j].st_name >= shs[s->sh_link].sh_size) continue;
            if (efi_strcmp(strs + syms[j].st_name, "uefi_header") == 0) {
                hdr_addr = syms[j].st_value;
            }
        }
        break;
    }
    if (hdr_addr != 0) {
        sputs("[UEFI] uefi_header at "); sputhex(hdr_addr); sputs("\n");
        return hdr_addr + 8;
    }

    /* ---- 路径2: 按段名查找(内核 stripped 时) ---- */
    if (eh->e_shstrndx < eh->e_shnum) {
        elf64_shdr_t *strsec = &shs[eh->e_shstrndx];
        const char *strs2 = (const char *)(img + strsec->sh_offset);
        for (uint32_t i = 0; i < eh->e_shnum; i++) {
            elf64_shdr_t *s = &shs[i];
            if (s->sh_name >= strsec->sh_size) continue;
            if (s->sh_type == SHT_NOBITS || s->sh_size < 16) continue;
            if (efi_strcmp(strs2 + s->sh_name, UEFI_SECTION_NAME) != 0) continue;
            sputs("[UEFI] .uefi section at "); sputhex(s->sh_addr); sputs("\n");
            return s->sh_addr + 8;
        }
    }
    sputs("[UEFI] uefi entry not found in kernel\n");
    return 0;
}

/* ================= ACPI MADT (SMP 拓扑) ================= */
static EFI_GUID acpi20_guid =
    { 0x8868e871, 0xe4f1, 0x11d3,
      { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

typedef struct {
    uint8_t signature[8];
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

/* EFI 配置表里找 ACPI 2.0 RSDP */
static uint64_t acpi_find_rsdp(EFI_SYSTEM_TABLE *st) {
    for (uint64_t i = 0; i < st->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
        if (ct->VendorGuid.Data1 == 0x8868e871 && ct->VendorGuid.Data2 == 0xe4f1 &&
                ct->VendorGuid.Data3 == 0x11d3) {
            return (uint64_t)(unsigned long)ct->VendorTable;  /* RSDP 本身, 勿解引用 */
        }
    }
    return 0;
}

/* 从 RSDP 找 MADT 表指针 (u64 地址, 返回 0 失败) */
static uint64_t acpi_find_madt(uint64_t rsdp) {
    const uint8_t *p = (const uint8_t *)rsdp;
    uint32_t rsdt = *(const uint32_t *)(p + 16);
    uint64_t xsdt = *(const uint64_t *)(p + 24);
    uint8_t rev = p[15];

    if (rev >= 2 && xsdt != 0) {
        const uint32_t *hdr = (const uint32_t *)xsdt;
        uint32_t len = hdr[1];
        uint64_t n = (len - 36) / 8;
        const uint64_t *ent = (const uint64_t *)(xsdt + 36);
        for (uint64_t i = 0; i < n; ++i) {
            const uint32_t *t = (const uint32_t *)ent[i];
            if (t[0] == 0x43495041) {          /* 'APIC' */
                return ent[i];
            }
        }
    }
    if (rsdt != 0) {
        const uint32_t *hdr = (const uint32_t *)rsdt;
        uint32_t len = hdr[1];
        uint32_t n = (len - 36) / 4;
        const uint32_t *ent = (const uint32_t *)(rsdt + 36);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t *t = (const uint32_t *)ent[i];
            if (t[0] == 0x43495041) {
                return ent[i];
            }
        }
    }
    return 0;
}

/* 拷贝 MADT 表到 bootinfo 缓冲 (≤4KB), 返回拷贝长度 */
static uint32_t acpi_copy_madt(uint64_t madt, uint8_t *dst, uint32_t cap) {
    const uint32_t *hdr = (const uint32_t *)madt;
    uint32_t len = hdr[1];
    if (len == 0 || len > cap) {
        return 0;
    }
    const uint8_t *src = (const uint8_t *)madt;
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
    return len;
}

/* ================= bootinfo ================= */
static bootinfo_t *build_bootinfo(uint64_t avoid_base, uint64_t avoid_span) {
    uint64_t pages = 6;
    uint64_t addr = 0x3FFFFFFF;   /* AllocateMaxAddress: <1GB (内核恒等映射可读) */
    for (int try = 0; try < 8; try++) {
        uint32_t type = (try < 4) ? AllocateMaxAddress : AllocateAddress;
        if (type == AllocateAddress) {
            addr = 0x03000000;    /* 固定 48MB 兜底, 躲开内核目标区 */
        }
        if (!EFI_ERROR((EFI_STATUS)efi_call4((void *)BS->AllocatePages, type,
                EfiLoaderData, pages, (uint64_t)(void *)&addr))) {
            if (!(addr < avoid_base + avoid_span && addr + pages * 4096 > avoid_base)) {
                break;
            }
            /* 落在内核目标区内(EBS 后会被内核镜像覆盖), 释放重试 */
            efi_call2((void *)BS->FreePages, addr, pages);
            addr = 0;
        }
    }
    if (addr == 0) {
        return 0;
    }
    bootinfo_t *bi = (bootinfo_t *)addr;
    efi_memset(bi, 0, pages * 4096);
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->firmware_rev = gST->Revision;

    /* ACPI MADT → 拷贝到 bootinfo 缓冲 +0x2000 (容量 8KB),
     * 内核恒等映射 <1GB 可读(bootinfo 本身分配在 <1GB) */
    {
        uint64_t rsdp = acpi_find_rsdp(gST);
        if (rsdp != 0) {
            uint64_t madt = acpi_find_madt(rsdp);
            if (madt != 0) {
                uint32_t mlen = acpi_copy_madt(madt,
                        (uint8_t *)addr + 0x2000, 0x2000);
                if (mlen > 0) {
                    bi->madt_addr = addr + 0x2000;
                    bi->madt_len = mlen;
                    sputs("[UEFI] madt copied: ");
                    sputhex(madt); sputs(" len="); sputdec(mlen); sputs("\n");
                }
            }
        }
    }

    /* 内存映射 (拷进 bootinfo 页, EBS 后内核读取) */
    uint8_t *map = (uint8_t *)bi + 0x100;
    uint64_t msz = pages * 4096 - 0x100, mkey = 0, dsize = 0, dver = 0;
    EFI_STATUS st = (EFI_STATUS)efi_call5((void *)BS->GetMemoryMap, (uint64_t)&msz,
        (uint64_t)map, (uint64_t)&mkey, (uint64_t)&dsize, (uint64_t)&dver);
    if (EFI_ERROR(st)) { sputs("[UEFI] GetMemoryMap failed\n"); return 0; }
    bi->memmap_addr = (uint64_t)map;
    bi->memmap_count = msz / dsize;
    bi->memmap_dsize = dsize;

    /* GOP 线性帧缓冲 */
    static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (!EFI_ERROR(efi_locate_protocol(BS, &gop_guid, (void **)&gop)) && gop) {
        bi->fb_addr = gop->Mode->FrameBufferBase;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi =
            (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)gop->Mode->Info;
        bi->fb_width = mi->HorizontalResolution;
        bi->fb_height = mi->VerticalResolution;
        bi->fb_pitch = mi->PixelsPerScanLine * 4;
        bi->fb_bpp = 32;
        {   /* 临时诊断: 原始 Info 头 40 字节 */
            volatile uint32_t *raw = (volatile uint32_t *)mi;
            sputs("[UEFI] gop info raw:");
            for (int i = 0; i < 10; i++) { serial_putc(' '); sputhex(raw[i]); }
            sputs("\n");
            sputs("[UEFI] gop sizeofinfo="); sputhex(gop->Mode->SizeOfInfo); sputs("\n");
        }
        sputs("[UEFI] gop fb="); sputhex(bi->fb_addr);
        sputs(" "); sputdec(bi->fb_width); serial_putc('x');
        sputdec(bi->fb_height); sputs("\n");
    }
    return bi;
}

/* ================= 主流程 ================= */

/* EBS 后拷贝前的安全检查: 目标区不能是 RuntimeServices/保留区/MMIO。
 * (EBS 后 Conventional/BootServices/Loader 内存均归 OS 所有) */
static int target_range_relocatable(bootinfo_t *bi, uint64_t base, uint64_t span) {
    uint64_t end = base + span;
    const uint8_t *map = (const uint8_t *)(uint64_t)bi->memmap_addr;
    uint64_t checked = 0;
    for (uint64_t i = 0; i < bi->memmap_count; i++) {
        const efi_memdesc_t *d = (const efi_memdesc_t *)(map + i * bi->memmap_dsize);
        uint64_t ds = d->PhysicalStart;
        uint64_t de = ds + d->NumberOfPages * 4096;
        if (de <= base || ds >= end) continue;
        if (d->Type == EfiRuntimeServicesCode || d->Type == EfiRuntimeServicesData ||
            d->Type == EfiReservedMemoryType || d->Type == EfiMemoryMappedIO ||
            d->Type == EfiMemoryMappedIOPortSpace || d->Type == EfiUnusableMemory ||
            d->Type == EfiACPIMemoryNVS) {
            return 0;
        }
        if (ds > base && checked == 0) return 0;  /* 目标头部无覆盖描述符 */
        if (de > checked) checked = de;
        if (checked >= end) return 1;
    }
    return checked >= end;
}

uint64_t efi_main(uint64_t ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    _con_ok = 1;
    /* ConOut 渲染实验: Reset + SetAttribute(规范偏移 64) + 标记串 */
    {
        void **con = (void **)SystemTable->ConOut;
        uint64_t st1 = (uint64_t)efi_call2(con[0], (uint64_t)con, 0);          /* Reset */
        uint64_t st2 = (uint64_t)efi_call2(con[8], (uint64_t)con, 0x0F);       /* SetAttribute */
        sputs("[UEFI] ConOut reset=");
        sputhex(st1);
        sputs(" setattr=");
        sputhex(st2);
        sputs("\n");
    }
    BS = (EFI_BOOT_SERVICES *)SystemTable->BootServices;

    serial_init();
    sputs("\n[UEFI] EwokOS x86 bootloader\n");
    sputs("[UEFI] firmware_rev="); sputdec(SystemTable->Revision >> 16);
    serial_putc('.'); sputdec(SystemTable->Revision & 0xFFFF); sputs("\n");

    efi_call4((void *)BS->SetWatchdogTimer, 0, 0, 0, 0);

    uint64_t ksize = 0;
    sputs("[UEFI] searching KERNEL.ELF (SFS -> iso9660)...\n");
    uint8_t *kimg = read_file(wstr(KERNEL_LOAD_PATH_1), &ksize);
    if (!kimg) kimg = read_file(wstr(KERNEL_LOAD_PATH_2), &ksize);
    if (!kimg) kimg = iso9660_read_file("KERNEL.ELF", ~0ULL, &ksize);
    if (!kimg) {
        sputs("[UEFI] FATAL: KERNEL.ELF not found\n");
        con16(SystemTable->ConOut, "[UEFI] FATAL: KERNEL.ELF not found\r\n");
        for (;;) __asm__ volatile("hlt");
    }
    sputs("[UEFI] kernel.elf size="); sputdec(ksize); sputs("\n");

    /* 内存盘 rootfs: SFS 找不到时按 ISO9660 裸块兜底 (Ventoy UEFI)。
     * 无 ROOTFS.IMG (kernel.hdd 磁盘引导) 则 rd=0, 内核走磁盘 rootfs。 */
    uint64_t rd_size = 0;
    sputs("[UEFI] searching ROOTFS.IMG (SFS -> iso9660)...\n");
    uint8_t *rd = read_file_at(wstr(ROOTFS_LOAD_PATH_1), ROOTFS_MAX_PHYS, &rd_size);
    if (!rd) rd = read_file_at(wstr(ROOTFS_LOAD_PATH_2), ROOTFS_MAX_PHYS, &rd_size);
    if (!rd) rd = iso9660_read_file("ROOTFS.IMG", ROOTFS_MAX_PHYS, &rd_size);
    if (rd) {
        sputs("[UEFI] rootfs img loaded: "); sputdec(rd_size >> 20);
        sputs("MB @ "); sputhex((uint64_t)rd); sputs("\n");
    } else {
        sputs("[UEFI] no ROOTFS.IMG (disk rootfs expected)\n");
    }

    uint64_t span = 0;
    int need_post_copy = 0;
    uint64_t load_target = KERNEL_LOAD_PHYS;
    uint64_t kbase = elf_load(kimg, &span, &need_post_copy, &load_target);
    if (!kbase) {
        sputs("[UEFI] FATAL: elf load failed\n");
        for (;;) __asm__ volatile("hlt");
    }

    uint64_t entry = find_uefi_entry(kimg);
    if (!entry) {
        sputs("[UEFI] FATAL: uefi entry not found\n");
        for (;;) __asm__ volatile("hlt");
    }

    bootinfo_t *bi = build_bootinfo(load_target, span);
    if (!bi) {
        sputs("[UEFI] FATAL: bootinfo failed\n");
        for (;;) __asm__ volatile("hlt");
    }
    bi->kernel_phys_base = load_target;
    bi->rd_base = rd ? (uint64_t)rd : 0;
    bi->rd_size = rd ? rd_size : 0;

    sputs("[UEFI] bootinfo at "); sputhex((uint64_t)bi);
    sputs(" memmap entries="); sputdec(bi->memmap_count);
    sputs(" kernel="); sputhex(kbase); sputs("+"); sputdec(span); sputs("\n");

    /* ExitBootServices 前最后一次 GetMemoryMap, 用其 map_key */
    {
        uint8_t map2[16384];
        uint64_t msz = sizeof(map2), mkey = 0, dsize = 0, dver = 0;
        efi_call5((void *)BS->GetMemoryMap, (uint64_t)&msz, (uint64_t)map2,
            (uint64_t)&mkey, (uint64_t)&dsize, (uint64_t)&dver);
        sputs("[UEFI] exiting boot services...\n");
        EFI_STATUS st = (EFI_STATUS)efi_call2((void *)BS->ExitBootServices,
            (uint64_t)ImageHandle, mkey);
        if (EFI_ERROR(st)) {
            sputs("[UEFI] EBS failed, retry once\n");
            efi_call5((void *)BS->GetMemoryMap, (uint64_t)&msz, (uint64_t)map2,
                (uint64_t)&mkey, (uint64_t)&dsize, (uint64_t)&dver);
            efi_call2((void *)BS->ExitBootServices, (uint64_t)ImageHandle, mkey);
        }
        _con_ok = 0;                     /* EBS 后 ConOut 不可用 */
    }

    /* EBS 后: 把暂存的内核搬到固定物理地址 0x100000 (固件不再管内存) */
    if (need_post_copy) {
        if (!target_range_relocatable(bi, load_target, span)) {
            sputs("[UEFI] FATAL: target range not relocatable, halt\n");
            for (;;) __asm__ volatile("hlt");
        }
        efi_memcpy((void *)load_target, (const void *)kbase, span);
        sputs("[UEFI] kernel relocated to "); sputhex(load_target); sputs("\n");
    }

    /* 跳内核 uefi_entry: rdi = bootinfo (内核自己切 CR3/段) */
    sputs("[UEFI] jumping to kernel at "); sputhex(entry); sputs("\n");
    {   /* 真机定位: 文本屏右上行角 "K" = stub 已交棒, 内核尚未产出日志 */
        volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
        vga[79] = (uint16_t)(0x0F00 | 'K');
    }
    void (*kentry)(uint64_t) = (void (*)(uint64_t))entry;
    kentry((uint64_t)bi);
    sputs("[UEFI] kernel returned?! halt\n");
    for (;;) __asm__ volatile("hlt");
    return 0;
}
