/*
 * adreno_smmu.c - Adreno SMMU-500 context-bank 0 + ARM LPAE stage-1 pagetable.
 *
 * The GPU (StreamID 0) is hard-wired to context bank 0 by the qcom adreno SMMU
 * impl (arm-smmu-qcom.c: qcom_adreno_smmu_alloc_context_bank forces cbndx 0 for
 * the GPU device).  We build a single-table ARM LPAE stage-1 pagetable that
 * IDENTITY-maps [0, 64 GB) with unprivileged 1 GB block descriptors, so every resolved
 * contig physical base g2dd/displayd hands down (framebuffers, dma pool, zap
 * region) is directly usable as a GPU virtual address (GPUVA == PA).  This
 * matches the coherent_walk LPAE config Linux programs (arm_64_lpae_alloc_
 * pgtable_s1): inner-shareable, write-back write-allocate, 4 KB granule.
 *
 * The context-bank register window is RUNTIME-DISCOVERED, not hardcoded:
 * GR0_ID1.PAGESIZE picks pgshift (12 for 4 KB pages, 16 for 64 KB), and
 * GR0_ID1.NUMPAGENDXB encodes the number of global-register pages before
 * CB0.  Thus CB0 base = smmu_base + (numpage << pgshift).
 * All register offsets below are transcribed verbatim from Linux
 * drivers/iommu/arm/arm-smmu/arm-smmu.h (v6.6); LPAE descriptor bits from
 * drivers/iommu/io-pgtable-arm.c.
 *
 * NOT hardware-validated on SC8280XP.  See adreno_a6xx.c bring-up note.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <q8b_adreno.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <string.h>

/* --- GR0 (global, page 0) --- */
#define SMMU_GR0_sCR0        0x0
#define SMMU_sCR0_USFCFG     (1u << 10)
#define SMMU_sCR0_CLIENTPD   (1u << 0)
/* Fault reporting + IRQ generation (arm-smmu.h).  EwokOS installs no SMMU
   fault handler for SPI 679, so these must stay off: a single fault would
   otherwise storm the GIC and wedge the CPU. */
#define SMMU_sCR0_GFRE       (1u << 1)
#define SMMU_sCR0_GFIE       (1u << 2)
#define SMMU_sCR0_GCFGFRE    (1u << 4)
#define SMMU_sCR0_GCFGFIE    (1u << 5)
#define SMMU_GR0_ID0         0x20
#define SMMU_GR0_ID1         0x24
#define SMMU_ID1_PAGESIZE    (1u << 31)
#define SMMU_ID1_NUMPAGENDXB_SHIFT 28
#define SMMU_ID1_NUMPAGENDXB_MASK  0x7u
#define SMMU_ID1_NUMCB_MASK  0xffu
#define SMMU_GR0_ID2         0x28
#define SMMU_GR0_sGFSR       0x48
#define SMMU_GR0_SMR(n)      (0x800 + ((n) << 2))
#define SMMU_SMR_VALID       (1u << 31)
/* DT specifiers are <&gpu_smmu SID 0xc00> on every Adreno master: the second
   cell is the SMR.MASK, i.e. SID bits 10:11 (Qualcomm SSD/VMID bits) are
   ignored when matching.  An exact-match SMR (mask 0) does NOT match the
   GMU's real StreamID, which makes its transactions fall through to the
   default S2CR (CB0's identity map) - the CM3 then executes whatever lives
   at physical 0x4000 and usually resets the board. */
#define SMMU_SMR_SID0_MASK   (0xc00u << 16)
#define SMMU_GR0_S2CR(n)     (0xc00 + ((n) << 2))
#define SMMU_S2CR_TYPE_TRANS 0u
#define SMMU_GR0_TLBIALLNSNH 0x68
#define SMMU_GR0_sTLBGSTATUS 0x74
#define SMMU_sTLBGSTATUS_GSACTIVE (1u << 0)

/* --- GR1 (context-bank attributes, page 1) --- */
#define SMMU_GR1_CBAR(n)     (0x0 + ((n) << 2))
#define SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS (1u << 16)
#define SMMU_CBAR_S1_MEMATTR_WB           (0xfu << 12)
#define SMMU_CBAR_S1_BPSHCFG_NSH          (3u << 8)
#define SMMU_CBAR_STAGE1 (SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS | \
                         SMMU_CBAR_S1_MEMATTR_WB | SMMU_CBAR_S1_BPSHCFG_NSH)
#define SMMU_GR1_CBA2R(n)    (0x800 + ((n) << 2))
#define SMMU_CBA2R_VA64      (1u << 0)

/* --- CBn (context bank registers, page numpage + n) --- */
#define SMMU_CB_SCTLR        0x0
#define SMMU_SCTLR_M         (1u << 0)
#define SMMU_SCTLR_TRE       (1u << 1)
#define SMMU_SCTLR_AFE       (1u << 2)
#define SMMU_SCTLR_CFRE      (1u << 5)
#define SMMU_SCTLR_HUPCF     (1u << 8)   /* qcom adreno: keep going after fault */
#define SMMU_SCTLR_S1_ASIDPNE (1u << 12)
#define SMMU_CB_ACTLR        0x4
#define SMMU_CB_TCR2         0x10
#define SMMU_CB_TTBR0        0x20        /* 64-bit */
#define SMMU_CB_TTBR1        0x28        /* 64-bit, disabled by TCR.EPD1 */
#define SMMU_CB_TCR          0x30
#define SMMU_CB_CONTEXTIDR   0x34
#define SMMU_CB_S1_MAIR0     0x38
#define SMMU_CB_S1_MAIR1     0x3c
#define SMMU_CB_FSR          0x58
#define SMMU_CB_FAR          0x60
#define SMMU_CB_FSYNR0       0x68
#define SMMU_CB_FSYNR1       0x6c
#define SMMU_CB_S1_TLBIASID  0x610
#define SMMU_CB_TLBSYNC      0x7f0
#define SMMU_CB_TLBSTATUS    0x7f4
#define SMMU_CB_TLBSTATUS_ACTIVE (1u << 0)

/* CB_TCR field packing (arm-smmu.h). */
#define SMMU_TCR_EPD1        (1u << 23)  /* disable TTBR1 walk (single TTBR0 map) */
#define SMMU_TCR_SH0_IS      (3u << 12)
#define SMMU_TCR_SH0_OS      (2u << 12)
#define SMMU_TCR_ORGN0_WBWA  (1u << 10)
#define SMMU_TCR_IRGN0_WBWA  (1u << 8)
/* gpu_smmu is dma-coherent in sc8280xp.dtsi, so io-pgtable-arm programs a
	coherent WBWA, inner-shareable page-table walk. */
#define SMMU_TCR_WALK_WBWA   (SMMU_TCR_SH0_IS | SMMU_TCR_ORGN0_WBWA | \
				      SMMU_TCR_IRGN0_WBWA)
/* CB_TCR2 (arm-smmu.h). */
#define SMMU_TCR2_SEP_UPSTREAM (7u << 15)
#define SMMU_TCR2_AS         (1u << 4)   /* AArch64 16-bit ASID */
#define SMMU_TCR2_PASIZE_40  (2u << 0)   /* 40-bit output address size */
#define SMMU_TCR_STAGE1     (SMMU_TCR_EPD1 | SMMU_TCR_WALK_WBWA | LPAE_T0SZ)
#define SMMU_TCR2_STAGE1    (SMMU_TCR2_SEP_UPSTREAM | SMMU_TCR2_AS | \
                            SMMU_TCR2_PASIZE_40)

/* LPAE stage-1 descriptor bits (io-pgtable-arm.c). */
#define LPAE_PTE_VALID       ((uint64_t)1 << 0)
#define LPAE_PTE_TYPE_BLOCK  ((uint64_t)1)
#define LPAE_PTE_TYPE_TABLE  ((uint64_t)3)
#define LPAE_PTE_AF          ((uint64_t)1 << 10)
#define LPAE_PTE_SH_IS       ((uint64_t)3 << 8)
#define LPAE_PTE_SH_OS       ((uint64_t)2 << 8)
#define LPAE_PTE_AP_UNPRIV   ((uint64_t)1 << 6)
#define LPAE_PTE_nG          ((uint64_t)1 << 11)
#define LPAE_PTE_ATTRINDX_CACHE ((uint64_t)1 << 2)  /* MAIR idx 1 = WBWA */
#define LPAE_PTE_ATTRINDX_NC   ((uint64_t)0 << 2)   /* MAIR idx 0 = NC   */
#define LPAE_PTE_ADDR_MASK   (((uint64_t)1 << 48) - ((uint64_t)1 << 12))
/* MSM_BO_WC does not set Linux's IOMMU_CACHE protection bit, so
   arm_lpae_prot_to_pte() selects MAIR index 0 (Normal-NC) and Outer
   Shareable.  Fixed GMU mappings add IOMMU_PRIV; auto-allocated log/HFI
   mappings do not and therefore also set AP_UNPRIV. */
#define LPAE_PROT_NC_PRIV    (LPAE_PTE_AF | LPAE_PTE_nG | LPAE_PTE_SH_OS | \
				      LPAE_PTE_ATTRINDX_NC)
#define LPAE_PROT_NC_UNPRIV  (LPAE_PROT_NC_PRIV | LPAE_PTE_AP_UNPRIV)

/* qcom_smmu_actlr_client_of_match: Adreno and its GMU use deep prefetch,
   CPRE and CMTLB on the SC8280XP SMMU-500. */
#define SMMU_ACTLR_ADRENO    0x00000303u
#define SMMU_SCTLR_STAGE1    (SMMU_SCTLR_M | SMMU_SCTLR_TRE | \
				      SMMU_SCTLR_AFE | SMMU_SCTLR_CFRE | \
				      SMMU_SCTLR_HUPCF | SMMU_SCTLR_S1_ASIDPNE)

/* MAIR0: idx0=NC(0x44) idx1=WBRWA(0xff) idx2=DEVICE(0x04) idx3=INC_OWBRWA(0xf4) */
#define LPAE_MAIR0           0xf404ff44u

/* Identity map extent: 64 x 1 GB blocks = [0, 64 GB).  One 4 KB L1 table. */
#define LPAE_L1_ENTRIES      512u
#define LPAE_IDENTITY_BLOCKS 64u
#define LPAE_BLOCK_SIZE      (1ULL << 30)

/* T0SZ for a 39-bit input (start level L1, 4 KB granule). */
#define LPAE_T0SZ            25u
#define LPAE_ASID            1u   /* CB0 (GPU) ASID  */
#define LPAE_GMU_ASID        1u   /* CB1 (GMU) ASID (per-bank space) */

/* 4 KB granule geometry for the on-demand CB1 mapper. */
#define LPAE_PAGE_SIZE       4096u
#define LPAE_L2_BLOCK_SIZE   (1ULL << 21)

/* GMU (CB1) SMMU StreamID, from sc8280xp.dtsi gmu@3d6a000:
   `iommus = <&gpu_smmu 5 0xc00>` -> StreamID 5.  The GPU is StreamID 0. */
#define SMMU_GMU_STREAMID    5u

/* CBn register access: byte offset inside the CBn page window.  The SMMU base
   is ADRENO_SMMU_BYTE_BASE inside the 1 MB core span; CB0 adds (cb0_page <<
   pgshift).  adreno_smmu_read/write take a byte offset from the SMMU base, so
   we fold the page offset in here. */
static uint32_t _cb0_byte;   /* byte offset of CB0 from the SMMU base */
static uint32_t _cb1_byte;   /* byte offset of CB1 from the SMMU base */
static uint32_t _gr1_byte;   /* byte offset of GR1 from the SMMU base */

static uint32_t cb0_read(uint32_t off)  { return adreno_smmu_read(_cb0_byte + off); }
static void     cb0_write(uint32_t off, uint32_t v) { adreno_smmu_write(_cb0_byte + off, v); }
static void     cb0_writeq(uint32_t off, uint64_t v) {
	adreno_smmu_write(_cb0_byte + off, (uint32_t)(v & 0xffffffffu));
	adreno_smmu_write(_cb0_byte + off + 4, (uint32_t)(v >> 32));
}
static uint64_t cb0_readq(uint32_t off) {
	return (uint64_t)cb0_read(off) | ((uint64_t)cb0_read(off + 4) << 32);
}

static uint32_t gr1_read(uint32_t off)  { return adreno_smmu_read(_gr1_byte + off); }
static void     gr1_write(uint32_t off, uint32_t v) { adreno_smmu_write(_gr1_byte + off, v); }

static uint32_t cb1_read(uint32_t off)  { return adreno_smmu_read(_cb1_byte + off); }
static void     cb1_write(uint32_t off, uint32_t v) { adreno_smmu_write(_cb1_byte + off, v); }
static void     cb1_writeq(uint32_t off, uint64_t v) {
	adreno_smmu_write(_cb1_byte + off, (uint32_t)(v & 0xffffffffu));
	adreno_smmu_write(_cb1_byte + off + 4, (uint32_t)(v >> 32));
}

static uint64_t cb1_readq(uint32_t off) {
	return (uint64_t)cb1_read(off) | ((uint64_t)cb1_read(off + 4) << 32);
}

/* Poll the CB1 TLB-sync until the invalidation completes. */
static int cb1_tlb_sync(void) {
	int i;
	cb1_write(SMMU_CB_TLBSYNC, 0);
	for (i = 0; i < 100000; i++) {
		if (!(cb1_read(SMMU_CB_TLBSTATUS) & SMMU_CB_TLBSTATUS_ACTIVE))
			return 0;
	}
	return -1;
}

/* Poll the CB TLB-sync until the invalidation completes. */
static int cb_tlb_sync(void) {
	int i;
	cb0_write(SMMU_CB_TLBSYNC, 0);
	for (i = 0; i < 100000; i++) {
		if (!(cb0_read(SMMU_CB_TLBSTATUS) & SMMU_CB_TLBSTATUS_ACTIVE))
			return 0;
	}
	return -1;
}

static int cb0_build_privileged_mappings(adreno_dev_t *d);

int adreno_bo_alloc(adreno_bo_t *bo, uint32_t size) {
	ewokos_addr_t va;
	ewokos_addr_t pa;
	if (size == 0)
		return -1;
	va = dma_alloc(0, size);
	if (va == 0)
		return -2;
	pa = dma_phy_addr(0, va);
	if (pa == 0) {
		dma_free(0, va);
		return -3;
	}
	bo->va = (void*)va;
	bo->pa = (uint64_t)pa;
	bo->size = size;
	memset(bo->va, 0, size);
	return 0;
}

void adreno_bo_free(adreno_bo_t *bo) {
	if (bo && bo->va) {
		dma_free(0, (ewokos_addr_t)bo->va);
		bo->va = 0;
		bo->pa = 0;
		bo->size = 0;
	}
}

int adreno_smmu_init(adreno_dev_t *d) {
	uint32_t id1, scr0;
	uint64_t smmu_size, page_size, cb0_byte, cb1_byte;
	uint64_t *l1;
	uint32_t i;

	if (d->smmu_ready)
		return 0;

	/* Match arm_smmu_device_cfg_probe(): NUMPAGENDXB is log2(pages) - 1,
	   while NUMCB is the actual number of context banks (not count - 1). */
	id1 = adreno_smmu_read(SMMU_GR0_ID1);
	d->pgshift = (id1 & SMMU_ID1_PAGESIZE) ? 16 : 12;
	d->numpage = 1u << (((id1 >> SMMU_ID1_NUMPAGENDXB_SHIFT) &
	                     SMMU_ID1_NUMPAGENDXB_MASK) + 1u);
	d->num_cb  = id1 & SMMU_ID1_NUMCB_MASK;
	d->cb0_page = d->numpage + 0;   /* GPU is always CB0 */

	page_size = 1ULL << d->pgshift;
	smmu_size = 2ULL * d->numpage * page_size;
	cb0_byte = (uint64_t)d->cb0_page * page_size;
	cb1_byte = (uint64_t)(d->numpage + 1u) * page_size;
	klog("adreno_smmu: ID0=%x ID1=%x ID2=%x pgshift=%u numpage=%u numcb=%u "
	     "cb0_byte=%llx cb1_byte=%llx\n",
	     adreno_smmu_read(SMMU_GR0_ID0), id1,
	     adreno_smmu_read(SMMU_GR0_ID2), d->pgshift, d->numpage, d->num_cb,
	     (unsigned long long)cb0_byte, (unsigned long long)cb1_byte);

	/* The DT resource must cover both global halves and CB0/CB1 in full.
	   Refuse to touch a guessed or truncated context-bank window. */
	if (d->num_cb < 2 || smmu_size != Q8B_ADRENO_SMMU_SIZE ||
	    cb1_byte + page_size > Q8B_ADRENO_SMMU_SIZE) {
		klog("adreno_smmu: invalid geometry hw_size=%llx map_size=%llx\n",
		     (unsigned long long)smmu_size,
		     (unsigned long long)Q8B_ADRENO_SMMU_SIZE);
		return -1;
	}

	_gr1_byte = (uint32_t)page_size;   /* GR1 = page 1 */
	_cb0_byte = (uint32_t)cb0_byte;
	_cb1_byte = (uint32_t)cb1_byte;

	/* Allocate one 4 KB L1 table; identity-map [0, 64 GB) in 1 GB blocks. */
	if (adreno_bo_alloc(&d->pgtable, LPAE_L1_ENTRIES * 8) != 0) {
		klog("adreno_smmu: pgtable alloc failed\n");
		return -2;
	}
	/* Expanded APRIV makes the ring reads and RPTR-shadow writes privileged.
	   Keep a small pool for the L2/L3 tables needed to split those mappings
	   out of the otherwise unprivileged identity map. */
	if (adreno_bo_alloc(&d->cb0_pt_pool, 16u * LPAE_PAGE_SIZE) != 0) {
		klog("adreno_smmu: CB0 page-table pool alloc failed\n");
		adreno_bo_free(&d->pgtable);
		return -2;
	}
	l1 = (uint64_t*)d->pgtable.va;
	for (i = 0; i < LPAE_IDENTITY_BLOCKS; i++) {
		uint64_t pa = (uint64_t)i * LPAE_BLOCK_SIZE;
		/* Normal GPU BO mappings do not carry IOMMU_PRIV in Linux.  They must
		   therefore set AP_UNPRIV.  Expanded APRIV makes ringbuffer traffic
		   privileged, but SQE instruction fetches remain unprivileged; using a
		   privileged-only block lets the ring drain and then faults at the SQE
		   firmware address with CB0 FSR.FORMAT|PF (0x408). */
		l1[i] = (pa & LPAE_PTE_ADDR_MASK) | LPAE_PROT_NC_UNPRIV |
		        LPAE_PTE_TYPE_BLOCK;
	}
	for (; i < LPAE_L1_ENTRIES; i++)
		l1[i] = 0;
	if (!d->rptr_shadow.va || !d->ring.bo.va ||
	    cb0_build_privileged_mappings(d) != 0) {
		klog("adreno_smmu: CB0 privileged mappings failed\n");
		adreno_bo_free(&d->cb0_pt_pool);
		adreno_bo_free(&d->pgtable);
		return -2;
	}
	adreno_wmb();   /* publish descriptors before the walker sees TTBR0 */

	d->ttbr0 = (d->pgtable.pa & LPAE_PTE_ADDR_MASK) |
	           ((uint64_t)LPAE_ASID << 48);

	/* GPU StreamIDs 0 and 1 use CB0 with the DT-provided 0xc00 mask. */
	adreno_smmu_write(SMMU_GR0_SMR(0),
	                  SMMU_SMR_VALID | SMMU_SMR_SID0_MASK | 0u);
	adreno_smmu_write(SMMU_GR0_S2CR(0), SMMU_S2CR_TYPE_TRANS | 0u /* CBNDX 0 */);
	adreno_smmu_write(SMMU_GR0_SMR(1),
	                  SMMU_SMR_VALID | SMMU_SMR_SID0_MASK | 1u);
	adreno_smmu_write(SMMU_GR0_S2CR(1), SMMU_S2CR_TYPE_TRANS | 0u /* CBNDX 0 */);
	klog("adreno_smmu: SMR0=%x S2CR0=%x SMR1=%x S2CR1=%x\n",
	     adreno_smmu_read(SMMU_GR0_SMR(0)),
	     adreno_smmu_read(SMMU_GR0_S2CR(0)),
	     adreno_smmu_read(SMMU_GR0_SMR(1)),
	     adreno_smmu_read(SMMU_GR0_S2CR(1)));

	/* Disable translation while reprogramming the CB. */
	cb0_write(SMMU_CB_SCTLR, 0);
	cb0_write(SMMU_CB_ACTLR, SMMU_ACTLR_ADRENO);

	/* arm_smmu_write_context_bank(): CBA2R, then the weakest bypass
	   attributes so the TCR/PTE shareability and memory type take precedence. */
	gr1_write(SMMU_GR1_CBA2R(0), SMMU_CBA2R_VA64);
	gr1_write(SMMU_GR1_CBAR(0), SMMU_CBAR_STAGE1);
	/* Linux sets AS in TCR2 for AArch64; EAE in TCR is AArch32-only.
	   Program TCR2 before TCR and both TTBRs, then the MAIRs. */
	cb0_write(SMMU_CB_TCR2, SMMU_TCR2_STAGE1);
	cb0_write(SMMU_CB_TCR, SMMU_TCR_STAGE1);
	cb0_writeq(SMMU_CB_TTBR0, d->ttbr0);
	cb0_writeq(SMMU_CB_TTBR1, (uint64_t)LPAE_ASID << 48);
	cb0_write(SMMU_CB_S1_MAIR0, LPAE_MAIR0);
	cb0_write(SMMU_CB_S1_MAIR1, 0);
	adreno_mb();

	/* Invalidate any stale CB0 TLB entries, then enable stage-1 translation
	   with the qcom adreno HUPCF quirk (keep processing after a fault). */
	cb0_write(SMMU_CB_S1_TLBIASID, LPAE_ASID);
	if (cb_tlb_sync() != 0) {
		klog("adreno_smmu: CB0 TLB sync timeout\n");
		return -3;
	}

	/* Match arm_smmu_write_context_bank() plus the qcom HUPCF quirk.  Keep
	   CFIE clear because EwokOS has no handler for the context-fault SPI. */
	cb0_write(SMMU_CB_FSR, cb0_read(SMMU_CB_FSR));
	cb0_write(SMMU_CB_SCTLR, SMMU_SCTLR_STAGE1);
	adreno_mb();

	/* Global: make sure the SMMU is out of CLIENT power-down and unmatched
	   transactions fault (so a missing stream map is visible, not silent). */
	scr0 = adreno_smmu_read(SMMU_GR0_sCR0);
	scr0 &= ~(SMMU_sCR0_CLIENTPD | SMMU_sCR0_GFRE | SMMU_sCR0_GFIE |
	          SMMU_sCR0_GCFGFRE | SMMU_sCR0_GCFGFIE);
	scr0 |= SMMU_sCR0_USFCFG;
	adreno_smmu_write(SMMU_GR0_sCR0, scr0);

	d->smmu_ready = 1;
	klog("adreno_smmu: CB0 ready page=%u byte=%x ttbr0=%llx actlr=%x "
	     "sctlr=%x want=%x\n",
	     d->cb0_page, _cb0_byte, (unsigned long long)d->ttbr0,
	     cb0_read(SMMU_CB_ACTLR), cb0_read(SMMU_CB_SCTLR),
	     SMMU_SCTLR_STAGE1);
	klog("adreno_smmu: CB0 AArch64 cbar=%x cba2r=%x tcr=%x tcr2=%x ttbr1=%llx\n",
	     gr1_read(SMMU_GR1_CBAR(0)), gr1_read(SMMU_GR1_CBA2R(0)),
	     cb0_read(SMMU_CB_TCR), cb0_read(SMMU_CB_TCR2),
	     (unsigned long long)cb0_readq(SMMU_CB_TTBR1));
	return 0;
}

typedef struct {
	uint8_t  *base;
	uint64_t  base_pa;
	uint32_t  off;
	uint32_t  cap;
} lpae_pool_t;

static uint64_t *lpae_pool_alloc(lpae_pool_t *p, uint64_t *pa_out) {
	uint64_t *t;
	if (p->off + LPAE_PAGE_SIZE > p->cap)
		return 0;
	t = (uint64_t*)(p->base + p->off);
	memset(t, 0, LPAE_PAGE_SIZE);
	if (pa_out)
		*pa_out = p->base_pa + p->off;
	p->off += LPAE_PAGE_SIZE;
	return t;
}

static uint64_t *lpae_pool_va(lpae_pool_t *p, uint64_t pa) {
	if (pa < p->base_pa || pa >= p->base_pa + p->cap)
		return 0;
	return (uint64_t*)(p->base + (pa - p->base_pa));
}

/* Replace identity blocks with tables while constructing CB0, then mark the
   requested pages privileged.  No descriptor is changed after CB0 is live. */
static int cb0_map_privileged_range(uint64_t *l1, lpae_pool_t *pool,
				     uint64_t iova, uint32_t size) {
	uint32_t off;

	for (off = 0; off < size; off += LPAE_PAGE_SIZE) {
		uint64_t va = iova + off;
		uint64_t e1, e2, child_pa;
		uint64_t *l2, *l3;
		uint32_t i1 = (uint32_t)((va >> 30) & 0x1ffu);
		uint32_t i2 = (uint32_t)((va >> 21) & 0x1ffu);
		uint32_t i3 = (uint32_t)((va >> 12) & 0x1ffu);
		uint32_t i;

		if (i1 >= LPAE_IDENTITY_BLOCKS)
			return -1;

		e1 = l1[i1];
		if ((e1 & 3u) == LPAE_PTE_TYPE_BLOCK) {
			uint64_t gb_base = (uint64_t)i1 * LPAE_BLOCK_SIZE;
			l2 = lpae_pool_alloc(pool, &child_pa);
			if (!l2)
				return -2;
			for (i = 0; i < LPAE_L1_ENTRIES; i++) {
				uint64_t pa = gb_base + (uint64_t)i * LPAE_L2_BLOCK_SIZE;
				l2[i] = (pa & LPAE_PTE_ADDR_MASK) |
					 LPAE_PROT_NC_UNPRIV | LPAE_PTE_TYPE_BLOCK;
			}
			l1[i1] = (child_pa & LPAE_PTE_ADDR_MASK) |
				 LPAE_PTE_TYPE_TABLE;
		} else if ((e1 & 3u) == LPAE_PTE_TYPE_TABLE) {
			l2 = lpae_pool_va(pool, e1 & LPAE_PTE_ADDR_MASK);
			if (!l2)
				return -3;
		} else {
			return -4;
		}

		e2 = l2[i2];
		if ((e2 & 3u) == LPAE_PTE_TYPE_BLOCK) {
			uint64_t mb_base = ((uint64_t)i1 * LPAE_BLOCK_SIZE) +
					   ((uint64_t)i2 * LPAE_L2_BLOCK_SIZE);
			l3 = lpae_pool_alloc(pool, &child_pa);
			if (!l3)
				return -5;
			for (i = 0; i < LPAE_L1_ENTRIES; i++) {
				uint64_t pa = mb_base + (uint64_t)i * LPAE_PAGE_SIZE;
				l3[i] = (pa & LPAE_PTE_ADDR_MASK) |
					 LPAE_PROT_NC_UNPRIV | LPAE_PTE_TYPE_TABLE;
			}
			l2[i2] = (child_pa & LPAE_PTE_ADDR_MASK) |
				 LPAE_PTE_TYPE_TABLE;
		} else if ((e2 & 3u) == LPAE_PTE_TYPE_TABLE) {
			l3 = lpae_pool_va(pool, e2 & LPAE_PTE_ADDR_MASK);
			if (!l3)
				return -6;
		} else {
			return -7;
		}

		l3[i3] = (va & LPAE_PTE_ADDR_MASK) | LPAE_PROT_NC_PRIV |
			 LPAE_PTE_TYPE_TABLE;
	}
	return 0;
}

static int cb0_check_privileged(uint64_t *l1, lpae_pool_t *pool,
				 uint64_t iova, const char *name) {
	uint32_t i1 = (uint32_t)((iova >> 30) & 0x1ffu);
	uint32_t i2 = (uint32_t)((iova >> 21) & 0x1ffu);
	uint32_t i3 = (uint32_t)((iova >> 12) & 0x1ffu);
	uint64_t e1 = l1[i1], e2, e3;
	uint64_t *l2, *l3;

	if ((e1 & 3u) != LPAE_PTE_TYPE_TABLE)
		return -1;
	l2 = lpae_pool_va(pool, e1 & LPAE_PTE_ADDR_MASK);
	if (!l2 || ((e2 = l2[i2]) & 3u) != LPAE_PTE_TYPE_TABLE)
		return -1;
	l3 = lpae_pool_va(pool, e2 & LPAE_PTE_ADDR_MASK);
	if (!l3)
		return -1;
	e3 = l3[i3];
	if ((e3 & LPAE_PTE_ADDR_MASK) != (iova & LPAE_PTE_ADDR_MASK) ||
	    (e3 & 3u) != LPAE_PTE_TYPE_TABLE || (e3 & LPAE_PTE_AP_UNPRIV))
		return -1;
	klog("adreno_smmu: CB0 privileged %s iova=%llx pte=%llx/%llx/%llx\n",
	     name, (unsigned long long)iova, (unsigned long long)e1,
	     (unsigned long long)e2, (unsigned long long)e3);
	return 0;
}

static int cb0_build_privileged_mappings(adreno_dev_t *d) {
	lpae_pool_t pool = {
		.base = (uint8_t *)d->cb0_pt_pool.va,
		.base_pa = d->cb0_pt_pool.pa,
		.off = 0,
		.cap = d->cb0_pt_pool.size,
	};
	uint64_t *l1 = (uint64_t *)d->pgtable.va;

	if (d->smmu_ready || !l1 || !pool.base)
		return -1;
	if (cb0_map_privileged_range(l1, &pool, d->rptr_shadow.iova,
				       LPAE_PAGE_SIZE) != 0 ||
	    cb0_map_privileged_range(l1, &pool, d->ring.bo.iova,
				       d->ring.bo.size) != 0)
		return -2;
	if (cb0_check_privileged(l1, &pool, d->rptr_shadow.iova, "rptr") != 0 ||
	    cb0_check_privileged(l1, &pool, d->ring.bo.iova, "ring-first") != 0 ||
	    cb0_check_privileged(l1, &pool,
				 d->ring.bo.iova + d->ring.bo.size - LPAE_PAGE_SIZE,
				 "ring-last") != 0)
		return -3;
	return 0;
}

/* Read back both context banks: CB0 serves the GPU and CB1 serves the GMU. */
void adreno_smmu_dump_fault(adreno_dev_t *d) {
	(void)d;
	klog("adreno_smmu: CB0 FSR=%x FAR=%llx FSYNR0=%x FSYNR1=%x "
	     "CB1 FSR=%x FAR=%llx FSYNR0=%x FSYNR1=%x GFSR=%x\n",
	     cb0_read(SMMU_CB_FSR), (unsigned long long)cb0_readq(SMMU_CB_FAR),
	     cb0_read(SMMU_CB_FSYNR0), cb0_read(SMMU_CB_FSYNR1),
	     cb1_read(SMMU_CB_FSR), (unsigned long long)cb1_readq(SMMU_CB_FAR),
	     cb1_read(SMMU_CB_FSYNR0), cb1_read(SMMU_CB_FSYNR1),
	     adreno_smmu_read(SMMU_GR0_sGFSR));
}

/* ------------------------------------------------------------------ */
/* Generic 4 KB-granule LPAE stage-1 mapper (used for CB1 / GMU).       */
/*                                                                      */
/* CB0 uses a single L1 table of 1 GB block descriptors because the GPU */
/* only ever touches contig physical bases (identity map).  The GMU, on */
/* the other hand, addresses a handful of firmware-fixed iovas scattered */
/* across [0x4000 .. 0x60400000], so CB1 needs real 4 KB page mappings.  */
/* We walk the tree on demand, allocating L2/L3 tables from a bump pool. */
/* ------------------------------------------------------------------ */

/* Recover the CPU va of a child table from the physical address stored in a
   table descriptor.  Every child table comes from the contiguous bump pool,
   so va = pool_base + (child_pa - pool_base_pa). */
/* Map [iova, iova+size) -> [pa, pa+size) at 4 KB granularity into the LPAE
   tree rooted at `l1`, allocating L2/L3 tables from `pool`.  iova/size must
   be 4 KB aligned (the caller rounds the firmware windows). */
static int lpae_map(uint64_t *l1, lpae_pool_t *pool,
                    uint64_t iova, uint64_t pa, uint32_t size,
                    uint64_t prot) {
	uint32_t off;
	for (off = 0; off < size; off += LPAE_PAGE_SIZE) {
		uint64_t va = iova + off;
		uint64_t p  = pa + off;
		uint32_t i1 = (uint32_t)((va >> 30) & 0x1ff);
		uint32_t i2 = (uint32_t)((va >> 21) & 0x1ff);
		uint32_t i3 = (uint32_t)((va >> 12) & 0x1ff);
		uint64_t *l2, *l3;
		uint64_t child_pa;

		if (!(l1[i1] & LPAE_PTE_VALID)) {
			l2 = lpae_pool_alloc(pool, &child_pa);
			if (!l2) return -1;
			l1[i1] = (child_pa & LPAE_PTE_ADDR_MASK) | LPAE_PTE_TYPE_TABLE;
		} else {
			l2 = lpae_pool_va(pool, l1[i1] & LPAE_PTE_ADDR_MASK);
		}

		if (!(l2[i2] & LPAE_PTE_VALID)) {
			l3 = lpae_pool_alloc(pool, &child_pa);
			if (!l3) return -1;
			l2[i2] = (child_pa & LPAE_PTE_ADDR_MASK) | LPAE_PTE_TYPE_TABLE;
		} else {
			l3 = lpae_pool_va(pool, l2[i2] & LPAE_PTE_ADDR_MASK);
		}

		/* Last level: 0b11 is a page descriptor (block bits only valid
		   above the last level). */
		l3[i3] = (p & LPAE_PTE_ADDR_MASK) | prot | LPAE_PTE_TYPE_TABLE;
	}
	return 0;
}

int adreno_smmu_gmu_cb_init(adreno_dev_t *d) {
	uint32_t cb1_page;
	uint64_t *l1;
	lpae_pool_t pool;

	if (d->smmu_gmu_ready)
		return 0;
	if (!d->smmu_ready)
		return -1;

	cb1_page = d->numpage + 1;   /* GMU is always CB1 */

	/* L1 root plus a bump pool for the L2/L3 tables.  32 tables (128 KB)
	   comfortably covers the icache (16 MB -> 8 L3 tables) plus dummy /
	   debug / log / hfi. */
	if (adreno_bo_alloc(&d->gmu_pgtable, LPAE_PAGE_SIZE) != 0)
		return -2;
	if (adreno_bo_alloc(&d->gmu_pt_pool, 32u * LPAE_PAGE_SIZE) != 0)
		return -3;

	pool.base    = (uint8_t*)d->gmu_pt_pool.va;
	pool.base_pa = d->gmu_pt_pool.pa;
	pool.off     = 0;
	pool.cap     = d->gmu_pt_pool.size;

	l1 = (uint64_t*)d->gmu_pgtable.va;

	/* Map every GMU buffer at the iova the firmware expects (fixed for
	   icache/dummy/debug, driver-chosen for log/hfi).  a6xx_gmu.c:
	   1595-1656 fixes icache@0x4000 (16M-16K), dummy@0x60000000 (8K),
	   debug@0x60400000 (28K). */
	if (lpae_map(l1, &pool, d->gmu_icache.iova, d->gmu_icache.pa,
	             d->gmu_icache.size, LPAE_PROT_NC_PRIV) != 0 ||
	    lpae_map(l1, &pool, d->gmu_dummy.iova, d->gmu_dummy.pa,
	             d->gmu_dummy.size, LPAE_PROT_NC_PRIV) != 0 ||
	    lpae_map(l1, &pool, d->gmu_debug.iova, d->gmu_debug.pa,
	             d->gmu_debug.size, LPAE_PROT_NC_PRIV) != 0 ||
	    lpae_map(l1, &pool, d->gmu_log.iova, d->gmu_log.pa,
	             d->gmu_log.size, LPAE_PROT_NC_UNPRIV) != 0 ||
	    lpae_map(l1, &pool, d->gmu_hfi.iova, d->gmu_hfi.pa,
	             d->gmu_hfi.size, LPAE_PROT_NC_UNPRIV) != 0) {
		klog("adreno_smmu: GMU CB1 page-map failed\n");
		return -4;
	}
	adreno_wmb();   /* publish descriptors before TTBR0 */

	/* Walk every GMU iova in software and compare with its BO PA.  The CM3
	   executes the icache fetch first, but the firmware init then reads the
	   HFI QTBL/log/dummy/debug; a wrong L3 entry for any of them makes the
	   CM3's data read fault and the firmware never reports FW_INIT. */
	{
		uint64_t *l1v = (uint64_t*)d->gmu_pgtable.va;
		uint64_t iovas[5] = { 0x4000u, d->gmu_dummy.iova, d->gmu_debug.iova,
		                      d->gmu_log.iova, d->gmu_hfi.iova };
		uint64_t pas[5] = { d->gmu_icache.pa, d->gmu_dummy.pa,
		                    d->gmu_debug.pa, d->gmu_log.pa, d->gmu_hfi.pa };
		const char *names[5] = { "icache", "dummy", "debug", "log", "hfi" };
		int wi;
		for (wi = 0; wi < 5; wi++) {
			uint64_t va = iovas[wi];
			uint64_t e1 = l1v[(va >> 30) & 0x1ff];
			uint64_t e2 = 0, e3 = 0, pa = 0;
			if (e1 & 1) {
				e2 = lpae_pool_va(&pool, e1 & LPAE_PTE_ADDR_MASK)[(va >> 21) & 0x1ff];
				if (e2 & 1)
					e3 = lpae_pool_va(&pool, e2 & LPAE_PTE_ADDR_MASK)[(va >> 12) & 0x1ff];
			}
			if (e3 & 1)
				pa = e3 & LPAE_PTE_ADDR_MASK;
			klog("adreno_smmu: CB1 walk %s iova=%llx pa=%llx bo=%llx pte=%llx\n",
			     names[wi], (unsigned long long)va, (unsigned long long)pa,
			     (unsigned long long)pas[wi], (unsigned long long)e3);
		}
	}

	d->gmu_ttbr0 = (d->gmu_pgtable.pa & LPAE_PTE_ADDR_MASK) |
	               ((uint64_t)LPAE_GMU_ASID << 48);

	/* GMU StreamID 5 -> SMR group 2 -> CB1.  The 0xc00 mask is the second
	   cell of the DT IOMMU specifier and must be preserved. */
	{
		uint32_t s2cr = SMMU_S2CR_TYPE_TRANS | 1u;   /* CBNDX 1 */
		adreno_smmu_write(SMMU_GR0_SMR(2),
		                  SMMU_SMR_VALID | SMMU_SMR_SID0_MASK |
		                  SMMU_GMU_STREAMID);
		adreno_smmu_write(SMMU_GR0_S2CR(2), s2cr);
		klog("adreno_smmu: SMR2=%x S2CR2=%x (gmu sid %u mask c00)\n",
		     adreno_smmu_read(SMMU_GR0_SMR(2)),
		     adreno_smmu_read(SMMU_GR0_S2CR(2)), SMMU_GMU_STREAMID);
	}

	cb1_write(SMMU_CB_SCTLR, 0);
	cb1_write(SMMU_CB_ACTLR, SMMU_ACTLR_ADRENO);
	gr1_write(SMMU_GR1_CBA2R(1), SMMU_CBA2R_VA64);
	gr1_write(SMMU_GR1_CBAR(1), SMMU_CBAR_STAGE1);
	cb1_write(SMMU_CB_TCR2, SMMU_TCR2_STAGE1);
	cb1_write(SMMU_CB_TCR, SMMU_TCR_STAGE1);
	cb1_writeq(SMMU_CB_TTBR0, d->gmu_ttbr0);
	cb1_writeq(SMMU_CB_TTBR1, (uint64_t)LPAE_GMU_ASID << 48);
	cb1_write(SMMU_CB_S1_MAIR0, LPAE_MAIR0);
	cb1_write(SMMU_CB_S1_MAIR1, 0);
	adreno_mb();

	cb1_write(SMMU_CB_S1_TLBIASID, LPAE_GMU_ASID);
	if (cb1_tlb_sync() != 0) {
		klog("adreno_smmu: CB1 TLB sync timeout\n");
		return -5;
	}

	cb1_write(SMMU_CB_FSR, cb1_read(SMMU_CB_FSR));
	cb1_write(SMMU_CB_SCTLR, SMMU_SCTLR_STAGE1);
	adreno_mb();

	d->smmu_gmu_ready = 1;
	klog("adreno_smmu: CB1 (GMU) ready page=%u byte=%x ttbr0=%llx\n",
	     cb1_page, _cb1_byte, (unsigned long long)d->gmu_ttbr0);
	klog("adreno_smmu: CB1 AArch64 tcr2=%x ttbr1=%llx\n",
	     cb1_read(SMMU_CB_TCR2),
	     (unsigned long long)cb1_readq(SMMU_CB_TTBR1));
	/* Read the CB1 state back: the CM3's first fetch dies unless the SMMU
	   really has this bank enabled and pointing at our tables. */
	{
		uint64_t ttbr = (uint64_t)cb1_read(SMMU_CB_TTBR0) |
		                ((uint64_t)cb1_read(SMMU_CB_TTBR0 + 4) << 32);
		klog("adreno_smmu: CB1 readback sctlr=%x want=%x tcr=%x ttbr0=%llx "
		     "s2cr2=%x cbar=%x cba2r=%x scr0=%x gfsr=%x\n",
		     cb1_read(SMMU_CB_SCTLR), SMMU_SCTLR_STAGE1,
		     cb1_read(SMMU_CB_TCR),
		     (unsigned long long)ttbr,
		     adreno_smmu_read(SMMU_GR0_S2CR(2)),
		     gr1_read(SMMU_GR1_CBAR(1)), gr1_read(SMMU_GR1_CBA2R(1)),
		     adreno_smmu_read(SMMU_GR0_sCR0),
		     adreno_smmu_read(0x4));
	}
	return 0;
}
