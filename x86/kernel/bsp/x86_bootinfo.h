/* x86_bootinfo.h — 内核侧 bootinfo 交接协议 (UEFI stub → 内核)
 * 与 machines/x86/kernel/boot/uefi/bootinfo.h 布局保持一致 (wire format);
 * 内核侧经 hw_info_arch.c 的 x86_parse_bootinfo() 消费。
 */
#ifndef X86_BOOTINFO_H
#define X86_BOOTINFO_H

#include <stdint.h>

#define X86_BOOTINFO_MAGIC   0x4B4F5745ULL   /* "EWOK" */
#define X86_BOOTINFO_VERSION 3

/* UEFI 内存描述符 (与固件 GetMemoryMap 布局一致) */
typedef struct __attribute__((packed)) {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} x86_efi_memdesc_t;

#define X86_EFI_MEM_USABLE(t) ((t) == 7 || (t) == 4 || (t) == 5 || (t) == 13 || (t) == 14 || (t) == 1 || (t) == 2)
/* 7=Conventional 4/5=BootServices code/data 13/14=ACPI 1/2=Loader code/data */

typedef struct {
    uint32_t magic;          /* X86_BOOTINFO_MAGIC */
    uint32_t version;        /* X86_BOOTINFO_VERSION */
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
} x86_bootinfo_t;

/* 内存盘 rootfs 物理区间 (bootinfo v3; 定义于 hw_info_arch.c,
 * 0 = 未提供。bsp/sd.c 与 x86_parse_bootinfo 均消费) */
extern uint64_t x86_rd_base;
extern uint64_t x86_rd_size;

/* 内存盘内核态访问窗口: PDPT[2] (0x80000000-0xBFFFFFFF) 的空闲区,
 * 2MB 大页直映射 (x86_map_ramdisk_vm), 避开内核直映射 (P2V 0x80000000 起,
 * 512MB 上限到 0x9FF00000) 与 VGA 文本页 0xBE000000 / AHCI 0xC0000000。
 * 上限 224MB (再往上压 VGA 的 PT 页): ISO 的 ROOTFS.IMG 为 128MB, 充裕。 */
#define X86_RAMDISK_VA_BASE  0xB0000000ULL
#define X86_RAMDISK_VA(off)  (X86_RAMDISK_VA_BASE + (ewokos_addr_t)(off))

#endif
