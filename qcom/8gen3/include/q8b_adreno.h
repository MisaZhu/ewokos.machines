/*
 * q8b_adreno.h - Adreno 690 (SC8280XP) physical-address map shared by the
 * Q8B kernel and any user-space Adreno driver.
 *
 * Provenance: upstream Linux arch/arm64/boot/dts/qcom/sc8280xp.dtsi nodes
 * gpu@3d00000, gmu@3d6a000, gpucc@3d90000, adreno_smmu@3da0000 (Bjorn
 * Andersson, "arm64: dts: qcom: sc8280xp: Add GPU related nodes", merged
 * in Linux 6.5) plus arch/arm64/boot/dts/qcom/sc8280xp-crd.dts /
 * sc8280xp-lenovo-thinkpad-x13s.dts reserved-memory node gpu_mem.
 *
 * A690 identity (Linux drivers/gpu/drm/msm/adreno/a6xx_catalog.c a690
 * entry, chip id 0x06090000):
 *   .family            = ADRENO_6XX_GEN4
 *   .fw[ADRENO_FW_SQE] = "a660_sqe.fw"
 *   .fw[ADRENO_FW_GMU] = "a660_gmu.bin"
 *   .zapfw             = "a690_zap.mdt"
 *   .gmem              = SZ_4M
 *   .address_space_size= SZ_16G
 *   .hwcg              = a690_hwcg
 *   hw_apriv = true, a660_family = true
 *
 * The kernel installs a device-memory mapping of Q8B_GPU_WIN_* and
 * Q8B_GPU_PDC_WIN_* in arch_vm() (see machines/q8b/kernel/bsp/hw_arch.h
 * and hw_info_arch.c).  User space uses SYS_MEM_MAP with the physical
 * addresses below to get its own mapping; check_mem_map_arch() whitelists
 * the entire low MMIO span (phy_top <= 0x20000000) plus the zap-shader
 * reserved region.
 */
#ifndef Q8B_ADRENO_H
#define Q8B_ADRENO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adreno core registers (kgsl_3d0_reg_memory) */
#define Q8B_ADRENO_CORE_PHY    0x03d00000ULL
#define Q8B_ADRENO_CORE_SIZE   0x00040000ULL

/* GPU CX debug + CX memory (secondary reg slices from the gpu DT node) */
#define Q8B_ADRENO_CX_DBGC_PHY 0x03d61000ULL
#define Q8B_ADRENO_CX_DBGC_SIZE 0x00000800ULL
#define Q8B_ADRENO_CX_MEM_PHY  0x03d9e000ULL
#define Q8B_ADRENO_CX_MEM_SIZE 0x00001000ULL

/* Graphics Management Unit (GMU): main block, RSCC, PDC */
#define Q8B_ADRENO_GMU_PHY     0x03d6a000ULL
#define Q8B_ADRENO_GMU_SIZE    0x00034000ULL
#define Q8B_ADRENO_GMU_RSCC_PHY 0x03de0000ULL
#define Q8B_ADRENO_GMU_RSCC_SIZE 0x00010000ULL
#define Q8B_ADRENO_GMU_PDC_PHY 0x0b290000ULL
#define Q8B_ADRENO_GMU_PDC_SIZE 0x00010000ULL

/* GPU clock controller (gpucc).  Physically overlaps the GMU span but is
   ioremapped separately in Linux; same treatment here. */
#define Q8B_ADRENO_GPUCC_PHY   0x03d90000ULL
#define Q8B_ADRENO_GPUCC_SIZE  0x00009000ULL

/* Global clock controller (gcc @ 0x00100000).  Only the GPU branch clocks
   are touched: gcc_gpu_gpll0_clk_src / gcc_gpu_gpll0_div_clk_src parent the
   two GPUCC RCGs (0x052000 bits 15/16), and gcc_ddrss_gpu_axi_clk /
   gcc_gpu_memnoc_gfx_clk (0x07115c / 0x071010) gate the GMU's memory path.
   Linux turns these on through the clock parent chain, which EwokOS has no
   equivalent of.  See drivers/clk/qcom/gcc-sc8280xp.c. */
#define Q8B_GCC_PHY            0x00100000ULL
#define Q8B_GCC_SIZE           0x00100000ULL

/* Adreno SMMU-500 (translation for GPU + GMU + CB contexts) */
#define Q8B_ADRENO_SMMU_PHY    0x03da0000ULL
#define Q8B_ADRENO_SMMU_SIZE   0x00020000ULL

/* Zap-shader reserved memory.  The DT gpu_mem node marks this no-map, so
   the UEFI memory map already excludes it from the kernel heap; user space
   loads the zap image here before triggering the zap-restore SMC.  The Q8B
   DTS (sc8280xp-radxa-dragon-q8b.dts) reuses the X13s signing image
   qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn (the SoC is unfused, so the
   X13s blob authenticates on this board too).  EwokOS looks for a copy under
   /etc/firmware/adreno/, then /etc/firmware/qcom/ and /usr/lib/firmware/. */
#define Q8B_ADRENO_ZAP_PHY     0x8bf00000ULL
#define Q8B_ADRENO_ZAP_SIZE    0x00002000ULL

/* GIC SPI numbers (from the DT interrupts properties).  Convert to INTID
   with (32 + SPI) as usual for GICv3.  All Adreno lines are LEVEL_HIGH. */
#define Q8B_ADRENO_SPI_GPU     300    /* INTID 332: GPU / RBBM */
#define Q8B_ADRENO_SPI_GMU_HFI 304    /* INTID 336: GMU HFI */
#define Q8B_ADRENO_SPI_GMU     305    /* INTID 337: GMU general */
#define Q8B_ADRENO_SPI_SMMU_G0 672    /* INTID 704: SMMU global 0 */
#define Q8B_ADRENO_SPI_SMMU_G1 673    /* INTID 705: SMMU global 1 */
#define Q8B_ADRENO_SPI_SMMU_CB0 678   /* INTID 710: SMMU context bank 0 (GPU) */
#define Q8B_ADRENO_SPI_SMMU_CB1 679   /* INTID 711: SMMU context bank 1 (GMU) */

/* Firmware file names, installed under /etc/firmware/adreno/ in the Q8B
	rootfs.  The A690 catalog entry requires these exact A660 images. */
#define Q8B_ADRENO_FW_DIR       "/etc/firmware/adreno/"
#define Q8B_ADRENO_FW_SQE       "a660_sqe.fw"       /* CP/SQE microcode */
#define Q8B_ADRENO_FW_GMU       "a660_gmu.bin"      /* GMU firmware (A690) */
#define Q8B_ADRENO_FW_GMU_SIZE  55252u              /* linux-firmware v3.01.0B */
#define Q8B_ADRENO_FW_SQE_SIZE  43292u              /* linux-firmware v1.12 */
#define Q8B_ADRENO_FW_ZAP_MDT   "a690_zap.mdt"      /* zap shader MBN header */
/* Zap shader segment files follow the standard MBN naming: a690_zap.b00,
   a690_zap.b01, ...  The count is encoded in the .mdt header. */
#define Q8B_ADRENO_FW_ZAP_FMT   "a690_zap.b%02d"

/* A690 hardware constants (see adreno_device.c gpulist[690]) */
#define Q8B_ADRENO_REVN          690
#define Q8B_ADRENO_GMEM_SIZE     (4ULL * 1024 * 1024)      /* 4 MB */
#define Q8B_ADRENO_ADDR_SPACE    (16ULL * 1024 * 1024 * 1024) /* 16 GB */
#define Q8B_ADRENO_HW_APRIV      1

#ifdef __cplusplus
}
#endif

#endif
