/*
 * qcom8gen3_platform.h - sys_info.platform_data layout shared by the QCOM8GEN3 kernel
 * and user-space drivers (bsp_fb / bsp_sd / uartd).
 *
 * Keep in sync with the kernel-side bootinfo consumer in
 * machines/qcom/8gen3/kernel/bsp/hw_info_arch.c.
 */
#ifndef QCOM8GEN3_PLATFORM_H
#define QCOM8GEN3_PLATFORM_H

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
} qcom8gen3_fb_info_t;

typedef struct {
    uint32_t size;       /* INITRAMFS.IMG/ROOTFS.IMG size, 0 = no ramdisk */
    uint32_t flags;      /* QCOM8GEN3_RD_FLAG_* */
    uint64_t phy_base;
    uint32_t boot_source; /* QCOM8GEN3_BOOT_SOURCE_* */
    uint32_t _pad;
} qcom8gen3_rd_info_t;

#define QCOM8GEN3_RD_FLAG_BOOT_ONLY  (1u << 0)
#define QCOM8GEN3_PLATFORM_RAMFS_RELEASE 0x51425201u

/* Storage/network source that supplied BOOTAA64.EFI/KERNEL.BIN/INITRAMFS.IMG.
 * The value is firmware metadata only; init still probes each backend and
 * falls through when the corresponding persistent rootfs is unavailable. */
#define QCOM8GEN3_BOOT_SOURCE_UNKNOWN 0u
#define QCOM8GEN3_BOOT_SOURCE_SD      1u
#define QCOM8GEN3_BOOT_SOURCE_USB     2u
#define QCOM8GEN3_BOOT_SOURCE_NVME    3u
#define QCOM8GEN3_BOOT_SOURCE_NET     4u

typedef struct {
    uint32_t state;      /* "SD" in bits 31:16, stage/error in bits 15:0 */
    uint32_t detail;     /* OCR on success, interrupt/status value on error */
} qcom8gen3_sd_info_t;

typedef struct {
    qcom8gen3_fb_info_t fb;
    qcom8gen3_rd_info_t rd;
    qcom8gen3_sd_info_t sd;
} qcom8gen3_platform_data_t;

#ifdef __cplusplus
}
#endif

#endif
