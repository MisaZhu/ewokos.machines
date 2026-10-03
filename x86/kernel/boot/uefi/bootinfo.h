/* bootinfo.h — UEFI stub 与内核之间的交接协议
 * 与 machines/x86/kernel/bsp/x86_bootinfo.h 布局保持一致。
 * 跳转约定: rdi = bootinfo 物理地址 (SysV 第1参)。
 */
#ifndef BOOTINFO_H
#define BOOTINFO_H

typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;
typedef unsigned long long uint64_t;

#define BOOTINFO_MAGIC 0x4B4F5745ULL   /* "EWOK" */
#define BOOTINFO_VERSION 3

/* UEFI 内存描述符 (与固件 GetMemoryMap 布局一致) */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} efi_memdesc_t;

#define EFI_MEM_USABLE(t) ((t) == 7 || (t) == 4 || (t) == 5 || (t) == 13 || (t) == 14 || (t) == 1 || (t) == 2)
/* 7=Conventional 4/5=BootServices code/data 13/14=ACPI 1/2=Loader code/data */

typedef struct {
    uint32_t magic;          /* BOOTINFO_MAGIC */
    uint32_t version;        /* 1 */
    /* EFI 内存映射 (ExitBootServices 前最后一份) */
    uint64_t memmap_addr;
    uint64_t memmap_count;
    uint64_t memmap_dsize;
    /* GOP 线性帧缓冲 */
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;       /* 字节/行 */
    uint32_t fb_bpp;
    /* 杂项 */
    uint64_t kernel_phys_base;
    uint64_t firmware_rev;   /* UEFI 2.70 -> 0x00020046 */
    /* v2: ACPI MADT 表拷贝 (内核恒等映射 <1GB 可读) */
    uint64_t madt_addr;
    uint32_t madt_len;
    uint32_t _pad_v2;
    /* v3: 内存盘 rootfs (UEFI stub 在 EBS 前拷入 RAM 的 ROOTFS.IMG,
     * 0=未提供, 如 kernel.hdd 磁盘引导) */
    uint64_t rd_base;
    uint64_t rd_size;
} bootinfo_t;

#endif
