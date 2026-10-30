/*
 * q8b_platform.h - sys_info.platform_data layout shared by the Q8B kernel
 * and user-space drivers (bsp_fb / bsp_sd / uartd).
 *
 * Keep in sync with the kernel-side bootinfo consumer in
 * machines/q8b/kernel/bsp/hw_info_arch.c.
 */
#ifndef Q8B_PLATFORM_H
#define Q8B_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t phy_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per line */
    uint32_t bpp;
    uint32_t format;     /* UEFI GOP PixelFormat: 0=RGB, 1=BGR, 2=BitMask */
    uint32_t _pad;
} q8b_fb_info_t;

typedef struct {
    uint32_t size;       /* RAMFS.IMG/ROOTFS.IMG size, 0 = no ramdisk */
    uint32_t flags;      /* Q8B_RD_FLAG_* */
    uint64_t phy_base;
    uint32_t boot_source; /* Q8B_BOOT_SOURCE_* */
    uint32_t _pad;
} q8b_rd_info_t;

#define Q8B_RD_FLAG_BOOT_ONLY  (1u << 0)
#define Q8B_PLATFORM_RAMFS_RELEASE 0x51425201u

/* Storage/network source that supplied BOOTAA64.EFI/KERNEL.BIN/RAMFS.IMG.
 * The value is firmware metadata only; init still probes each backend and
 * falls through when the corresponding persistent rootfs is unavailable. */
#define Q8B_BOOT_SOURCE_UNKNOWN 0u
#define Q8B_BOOT_SOURCE_SD      1u
#define Q8B_BOOT_SOURCE_USB     2u
#define Q8B_BOOT_SOURCE_NVME    3u
#define Q8B_BOOT_SOURCE_NET     4u

typedef struct {
    uint32_t state;      /* "SD" in bits 31:16, stage/error in bits 15:0 */
    uint32_t detail;     /* OCR on success, interrupt/status value on error */
} q8b_sd_info_t;

typedef struct {
    q8b_fb_info_t fb;
    q8b_rd_info_t rd;
    q8b_sd_info_t sd;
} q8b_platform_data_t;

#ifdef __cplusplus
}
#endif

#endif
