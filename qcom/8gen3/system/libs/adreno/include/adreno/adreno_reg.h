/*
 * adreno_reg.h - Adreno 690 (SC8280XP / Q8B) MMIO window + register access.
 *
 * The GPU core, GMU, GPUCC, Adreno-SMMU and GMU-RSCC register blocks all live
 * inside one 1 MB physical span (0x03d0_0000 .. 0x03df_ffff), so a single
 * SYS_MEM_MAP of that span gives user space every block at its natural offset.
 * The GMU PDC/RSCC sequencer window lives far away at 0x0b29_0000 and gets its
 * own 64 KB mapping.  Physical addresses and sizes come from q8b_adreno.h
 * (upstream Linux sc8280xp.dtsi).
 *
 * Register offsets in a6xx_regs.h / a6xx_gmu_regs.h are WORD indices; the
 * accessors below multiply by 4 and add the block's byte base inside the
 * mapped span, matching Linux gpu_read/gpu_write (a6xx_gpu.c) and gmu_read/
 * gmu_write (a6xx_gmu.c) semantics.
 *
 * VA layout (one process = one GPU driver instance; g2dd/displayd each map
 * their own copy).  The base sits in the high user MMIO region, clear of the
 * sdcc2 window at 0x7000_0000 and of the low binary/heap.
 */
#ifndef ADRENO_REG_H
#define ADRENO_REG_H

#include <stdint.h>
#include <ewoksys/ewokdef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User-space VA base for the Adreno MMIO windows. */
#define ADRENO_MMIO_VA_BASE   0x68000000ULL
/* 1 MB span covering GPU core + GMU + GPUCC + SMMU + RSCC (phy 0x03d0_0000). */
#define ADRENO_CORE_VA        (ADRENO_MMIO_VA_BASE + 0x00000000ULL)
#define ADRENO_CORE_MAP_SIZE  0x00100000ULL
/* 64 KB PDC/RSCC sequencer window (phy 0x0b29_0000). */
#define ADRENO_PDC_VA         (ADRENO_MMIO_VA_BASE + 0x00100000ULL)
#define ADRENO_PDC_MAP_SIZE   0x00010000ULL
/* 1 MB GCC window (phy 0x0010_0000): the GPU branch clocks that feed the
   GPUCC RCGs (gcc_gpu_gpll0*, gcc_ddrss_gpu_axi*, gcc_gpu_memnoc_gfx*) live
   here and are enabled by Linux's clock parent chain, which EwokOS has no
   equivalent of.  Only 0x052000..0x07115c is touched. */
#define ADRENO_GCC_VA         (ADRENO_MMIO_VA_BASE + 0x00200000ULL)
#define ADRENO_GCC_MAP_SIZE   0x00100000ULL

/* Byte offsets of each block inside the 1 MB core span, relative to
   Q8B_ADRENO_CORE_PHY (0x03d0_0000).  Word-index regs add (word*4). */
#define ADRENO_GPU_BYTE_BASE   0x00000000u  /* kgsl_3d0_reg_memory          */
#define ADRENO_CX_DBGC_BYTE    0x00061000u  /* gpu cx debug                 */
#define ADRENO_GMU_BYTE_BASE   0x0006a000u  /* gmu@3d6a000                  */
#define ADRENO_GPUCC_BYTE_BASE 0x00090000u  /* gpucc@3d90000                */
#define ADRENO_CX_MEM_BYTE     0x0009e000u  /* gpu cx memory                */
#define ADRENO_SMMU_BYTE_BASE  0x000a0000u  /* adreno_smmu@3da0000          */
#define ADRENO_RSCC_BYTE_BASE  0x000e0000u  /* gmu rscc@3de0000             */

/* Map (or re-map after fork) both MMIO windows.  Idempotent.  Returns 0 on
   success, negative on failure.  Must run before any register access. */
int adreno_reg_map(void);

/* Raw block accessors.  `word` is a register word index (a6xx_regs.h style);
   the helpers translate to a byte address inside the mapped span. */
uint32_t adreno_gpu_read(uint32_t word);
void     adreno_gpu_write(uint32_t word, uint32_t val);

uint32_t adreno_gmu_read(uint32_t word);
void     adreno_gmu_write(uint32_t word, uint32_t val);

uint32_t adreno_gpucc_read(uint32_t word);
void     adreno_gpucc_write(uint32_t word, uint32_t val);

uint32_t adreno_smmu_read(uint32_t byte_off);
void     adreno_smmu_write(uint32_t byte_off, uint32_t val);

uint32_t adreno_rscc_read(uint32_t word);
void     adreno_rscc_write(uint32_t word, uint32_t val);

uint32_t adreno_pdc_read(uint32_t byte_off);
void     adreno_pdc_write(uint32_t byte_off, uint32_t val);

/* GCC (global clock controller) byte-offset accessors for the GPU-side
   branch clocks.  Offsets are byte offsets from GCC base 0x0010_0000, as in
   drivers/clk/qcom/gcc-sc8280xp.c. */
uint32_t adreno_gcc_read(uint32_t byte_off);
void     adreno_gcc_write(uint32_t byte_off, uint32_t val);

/* Read-modify-write and polled wait, mirroring Linux gmu_rmw /
   gmu_poll_timeout (a6xx_gmu.c).  poll returns 0 when (*addr & mask) == val
   within the timeout, -ETIMEDOUT otherwise.  Delay/usleep granularity is the
   EwokOS proc yield; timeouts are expressed in poll iterations. */
uint32_t adreno_gmu_rmw(uint32_t word, uint32_t mask, uint32_t val);
int      adreno_gmu_poll(uint32_t word, uint32_t mask, uint32_t val, int tries);
int      adreno_gpu_poll(uint32_t word, uint32_t mask, uint32_t val, int tries);
int      adreno_rscc_poll(uint32_t word, uint32_t mask, uint32_t val, int tries);

/* Memory + IO barriers.  aarch64 user space: dmb(sy) for ordering, the
   volatile MMIO accessors already force the load/store. */
void adreno_mb(void);   /* full memory barrier (dmb sy) */
void adreno_wmb(void);  /* write barrier (dmb st)     */

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_REG_H */
