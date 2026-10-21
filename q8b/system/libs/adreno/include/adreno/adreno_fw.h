/*
 * adreno_fw.h - Adreno 690 (SC8280XP / Q8B) firmware loaders.
 *
 * Three firmware images are consumed by the A690 bring-up path.  Their
 * on-disk formats and load semantics are taken verbatim from Linux v6.6
 * drivers/gpu/drm/msm/adreno:
 *
 *   1. GMU firmware  ("a690_gmu.bin")
 *      A flat list of `struct block_header` records (a6xx_gmu.c:673-744).
 *      Each record carries a target address and a word payload; the GMU
 *      boot code routes it to CM3 ITCM (0x0000_0000..0x0000_3fff),
 *      CM3 DTCM (0x0004_0000..0x0004_3fff, or 0x1000_4000 on the a650
 *      family), or one of the GMU SRAM windows (icache/dcache/dummy).
 *      adreno_fw_load_gmu() parses the image into an adreno_gmu_fw_t whose
 *      .blocks[] array preserves addr/size/type/value plus a pointer into
 *      the owned raw buffer; the caller does the actual MMIO writes.
 *
 *   2. SQE firmware  ("a660_sqe.fw")
 *      A raw microcode blob (a6xx_gpu.c:1119-1146, adreno_fw_create_bo).
 *      On A6XX there is no separate PFP/ME/CE ucode port: a single SQE
 *      (Sequencer) image is DMA'd into a GPU buffer object and the CP
 *      fetches it through REG_A6XX_CP_SQE_INSTR_BASE.  adreno_fw_load_sqe()
 *      simply returns the owned raw buffer + size.
 *
 *   3. Zap shader  ("a690_zap.mdt" + "a690_zap.bNN")
 *      Qualcomm split-ELF ("MBN/MDT") format: the .mdt file holds an ELF32
 *      header plus program headers but NO segment data; each PT_LOAD
 *      segment's bytes live in a sibling .bNN file (NN = phdr index).
 *      Linux loads them with qcom_mdt_get_size()/qcom_mdt_load() and then
 *      authenticates via qcom_scm_pas_auth_and_reset() (adreno_gpu.c:30-192).
 *      adreno_fw_load_zap() parses the .mdt, computes the total footprint
 *      and exposes each segment's paddr/memsz/filesz plus the owned .bNN
 *      buffer, so the caller can stage them into the reserved region at
 *      Q8B_ADRENO_ZAP_PHY.
 *
 *      NOTE: the zap path additionally requires a TrustZone SCM call
 *      (PAS auth-and-reset).  EwokOS q8b SCM/PAS support must be confirmed
 *      before the staged image can be authenticated; until then the loader
 *      only stages bytes and reports that the SCM step is outstanding.
 *
 * All loaders allocate through the EwokOS heap and must be released with
 * the matching adreno_fw_free_*().  File I/O goes through vfs_readfile().
 */
#ifndef ADRENO_FW_H
#define ADRENO_FW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GMU firmware (a690_gmu.bin)                                         */
/* ------------------------------------------------------------------ */

/* One `struct block_header` record from the GMU image.  Layout matches
   a6xx_gmu.c:673-679 exactly: addr/size/type/value then size bytes of
   payload (size is in bytes; data is addressed in words). */
typedef struct {
	uint32_t        addr;   /* target address (ITCM/DTCM/GMEM window) */
	uint32_t        size;   /* payload size in bytes                  */
	uint32_t        type;
	uint32_t        value;
	const uint32_t *data;   /* points into adreno_gmu_fw_t.raw        */
} adreno_fw_block_t;

typedef struct {
	uint8_t          *raw;        /* owned whole-file buffer          */
	int               raw_size;
	adreno_fw_block_t *blocks;    /* owned parsed block list          */
	int               num_blocks;
} adreno_gmu_fw_t;

/* Load and parse a690_gmu.bin.  Returns 0 on success, negative errno on
   failure (file missing, truncated, or malformed block chain). */
int  adreno_fw_load_gmu(adreno_gmu_fw_t *out);
void adreno_fw_free_gmu(adreno_gmu_fw_t *fw);

/* GMU CM3 memory windows used to route blocks (a6xx_gmu.c:698-702).
   A690 is ADRENO_6XX_GEN4; adreno_is_a650_family() tests family >= GEN3
   (adreno_gpu.h:390-395), so A690 IS in the a650 family and takes the
   dtcm_base = 0x1000_4000 branch (a6xx_gmu.c:701-702), NOT 0x0004_0000. */
#define ADRENO_GMU_ITCM_BASE   0x00000000u
#define ADRENO_GMU_DTCM_BASE   0x10004000u
#define ADRENO_GMU_TCM_SIZE    0x00004000u   /* SZ_16K per a6xx_gmu.c   */

/* ------------------------------------------------------------------ */
/* SQE firmware (a660_sqe.fw)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t *raw;     /* owned microcode blob */
	int      size;
} adreno_sqe_fw_t;

int  adreno_fw_load_sqe(adreno_sqe_fw_t *out);
void adreno_fw_free_sqe(adreno_sqe_fw_t *fw);

/* ------------------------------------------------------------------ */
/* Zap shader (a690_zap.mdt + a690_zap.bNN)                            */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t       paddr;    /* physical addr (relative to region base) */
	uint32_t       memsz;    /* in-memory size                          */
	uint32_t       filesz;   /* size present in the .bNN file           */
	uint32_t       flags;    /* PF_X / PF_W / PF_R                      */
	const uint8_t *data;     /* points into seg_bufs[i] (may be NULL)   */
	uint8_t       *seg_buf;  /* owned .bNN buffer (may be NULL)         */
	int            seg_size;
} adreno_zap_seg_t;

typedef struct {
	uint8_t         *mdt;        /* owned .mdt buffer                   */
	int              mdt_size;
	uint32_t         total_size; /* qcom_mdt_get_size() equivalent      */
	uint32_t         base_paddr; /* lowest PT_LOAD p_paddr              */
	adreno_zap_seg_t *segs;      /* owned PT_LOAD segment list          */
	int              num_segs;
} adreno_zap_fw_t;

int  adreno_fw_load_zap(adreno_zap_fw_t *out);
void adreno_fw_free_zap(adreno_zap_fw_t *fw);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_FW_H */
