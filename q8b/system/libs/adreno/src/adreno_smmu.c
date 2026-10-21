/*
 * adreno_smmu.c - Adreno SMMU-500 context-bank 0 + ARM LPAE stage-1 pagetable.
 *
 * The GPU (StreamID 0) is hard-wired to context bank 0 by the qcom adreno SMMU
 * impl (arm-smmu-qcom.c: qcom_adreno_smmu_alloc_context_bank forces cbndx 0 for
 * the GPU device).  We build a single-table ARM LPAE stage-1 pagetable that
 * IDENTITY-maps [0, 64 GB) with 1 GB block descriptors, so every resolved
 * contig physical base g2dd/displayd hands down (framebuffers, dma pool, zap
 * region) is directly usable as a GPU virtual address (GPUVA == PA).  This
 * matches the coherent_walk LPAE config Linux programs (arm_64_lpae_alloc_
 * pgtable_s1): inner-shareable, write-back write-allocate, 4 KB granule.
 *
 * The context-bank register window is RUNTIME-DISCOVERED, not hardcoded:
 * GR0_ID1.PAGESIZE picks pgshift (12 for 4 KB pages, 16 for 64 KB), CB0 sits
 * at page (numpage + 0) with numpage = 2 (GR0 + GR1) on MMU-500, so
 *   CB0 base = smmu_base + (2 << pgshift).
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
#include <string.h>

/* --- GR0 (global, page 0) --- */
#define SMMU_GR0_sCR0        0x0
#define SMMU_sCR0_USFCFG     (1u << 10)
#define SMMU_sCR0_CLIENTPD   (1u << 0)
#define SMMU_GR0_ID0         0x20
#define SMMU_GR0_ID1         0x24
#define SMMU_ID1_PAGESIZE    (1u << 31)
#define SMMU_ID1_NUMCB_MASK  0xffu
#define SMMU_GR0_ID2         0x28
#define SMMU_GR0_SMR(n)      (0x800 + ((n) << 2))
#define SMMU_SMR_VALID       (1u << 31)
#define SMMU_GR0_S2CR(n)     (0xc00 + ((n) << 2))
#define SMMU_S2CR_TYPE_TRANS 0u
#define SMMU_GR0_TLBIALLNSNH 0x68
#define SMMU_GR0_sTLBGSTATUS 0x74
#define SMMU_sTLBGSTATUS_GSACTIVE (1u << 0)

/* --- GR1 (context-bank attributes, page 1) --- */
#define SMMU_GR1_CBAR(n)     (0x0 + ((n) << 2))
#define SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS (1u << 16)
#define SMMU_CBAR_S1_MEMATTR_WB           (0xfu << 12)
#define SMMU_GR1_CBA2R(n)    (0x800 + ((n) << 2))
#define SMMU_CBA2R_VA64      (1u << 0)

/* --- CBn (context bank registers, page numpage + n) --- */
#define SMMU_CB_SCTLR        0x0
#define SMMU_SCTLR_M         (1u << 0)
#define SMMU_SCTLR_HUPCF     (1u << 8)   /* qcom adreno: keep going after fault */
#define SMMU_CB_ACTLR        0x4
#define SMMU_CB_TCR2         0x10
#define SMMU_CB_TTBR0        0x20        /* 64-bit */
#define SMMU_CB_TCR          0x30
#define SMMU_CB_CONTEXTIDR   0x34
#define SMMU_CB_S1_MAIR0     0x38
#define SMMU_CB_S1_MAIR1     0x3c
#define SMMU_CB_FSR          0x58
#define SMMU_CB_FAR          0x60
#define SMMU_CB_S1_TLBIASID  0x610
#define SMMU_CB_TLBSYNC      0x7f0
#define SMMU_CB_TLBSTATUS    0x7f4
#define SMMU_CB_TLBSTATUS_ACTIVE (1u << 0)

/* CB_TCR field packing (arm-smmu.h). */
#define SMMU_TCR_EAE         (1u << 31)
#define SMMU_TCR_EPD1        (1u << 23)  /* disable TTBR1 walk (single TTBR0 map) */
#define SMMU_TCR_SH0_IS      (3u << 12)
#define SMMU_TCR_ORGN0_WBWA  (1u << 10)
#define SMMU_TCR_IRGN0_WBWA  (1u << 8)
/* CB_TCR2 (arm-smmu.h). */
#define SMMU_TCR2_SEP_UPSTREAM (7u << 15)
#define SMMU_TCR2_PASIZE_40  (2u << 0)   /* 40-bit output address size */

/* LPAE stage-1 descriptor bits (io-pgtable-arm.c). */
#define LPAE_PTE_VALID       ((uint64_t)1 << 0)
#define LPAE_PTE_TYPE_BLOCK  ((uint64_t)1)
#define LPAE_PTE_TYPE_TABLE  ((uint64_t)3)
#define LPAE_PTE_AF          ((uint64_t)1 << 10)
#define LPAE_PTE_SH_IS       ((uint64_t)3 << 8)
#define LPAE_PTE_ATTRINDX_CACHE ((uint64_t)1 << 2)  /* MAIR idx 1 = WBWA */
#define LPAE_PTE_ADDR_MASK   (((uint64_t)1 << 48) - ((uint64_t)1 << 12))
/* prot for normal WB-cacheable inner-shareable RW block/page. */
#define LPAE_PROT_NORMAL     (LPAE_PTE_AF | LPAE_PTE_SH_IS | LPAE_PTE_ATTRINDX_CACHE)

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
static uint32_t gr1_read(uint32_t off)  { return adreno_smmu_read(_gr1_byte + off); }
static void     gr1_write(uint32_t off, uint32_t v) { adreno_smmu_write(_gr1_byte + off, v); }

static uint32_t cb1_read(uint32_t off)  { return adreno_smmu_read(_cb1_byte + off); }
static void     cb1_write(uint32_t off, uint32_t v) { adreno_smmu_write(_cb1_byte + off, v); }
static void     cb1_writeq(uint32_t off, uint64_t v) {
	adreno_smmu_write(_cb1_byte + off, (uint32_t)(v & 0xffffffffu));
	adreno_smmu_write(_cb1_byte + off + 4, (uint32_t)(v >> 32));
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
	uint64_t *l1;
	uint32_t i;

	if (d->smmu_ready)
		return 0;

	/* Discover the page geometry: PAGESIZE -> pgshift, NUMCB -> banks. */
	id1 = adreno_smmu_read(SMMU_GR0_ID1);
	d->pgshift = (id1 & SMMU_ID1_PAGESIZE) ? 16 : 12;
	d->num_cb  = (id1 & SMMU_ID1_NUMCB_MASK) + 1;
	d->numpage = 2;                 /* GR0 + GR1 precede the CB pages */
	d->cb0_page = d->numpage + 0;   /* GPU is always CB0 */

	_gr1_byte = (uint32_t)(1u << d->pgshift);            /* GR1 = page 1 */
	_cb0_byte = (uint32_t)(d->cb0_page << d->pgshift);   /* CB0 = page 2 */

	/* Allocate one 4 KB L1 table; identity-map [0, 64 GB) in 1 GB blocks. */
	if (adreno_bo_alloc(&d->pgtable, LPAE_L1_ENTRIES * 8) != 0) {
		klog("adreno_smmu: pgtable alloc failed\n");
		return -1;
	}
	l1 = (uint64_t*)d->pgtable.va;
	for (i = 0; i < LPAE_IDENTITY_BLOCKS; i++) {
		uint64_t pa = (uint64_t)i * LPAE_BLOCK_SIZE;
		l1[i] = (pa & LPAE_PTE_ADDR_MASK) | LPAE_PROT_NORMAL | LPAE_PTE_TYPE_BLOCK;
	}
	for (; i < LPAE_L1_ENTRIES; i++)
		l1[i] = 0;
	adreno_wmb();   /* publish descriptors before the walker sees TTBR0 */

	d->ttbr0 = (d->pgtable.pa & LPAE_PTE_ADDR_MASK) |
	           ((uint64_t)LPAE_ASID << 48);

	/* Stream match: GPU StreamID 0 -> SMR group 0 -> CB0, stage-1 translate.
	   The adreno SMMU is dedicated to the GPU/GMU, so programming group 0
	   here cannot disturb an unrelated device. */
	adreno_smmu_write(SMMU_GR0_SMR(0), SMMU_SMR_VALID | 0u /* ID 0, mask 0 */);
	adreno_smmu_write(SMMU_GR0_S2CR(0), SMMU_S2CR_TYPE_TRANS | 0u /* CBNDX 0 */);

	/* CB0 attributes: stage-1 translate / stage-2 bypass, WB memattr, 64-bit VA. */
	gr1_write(SMMU_GR1_CBAR(0),
	          SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS | SMMU_CBAR_S1_MEMATTR_WB);
	gr1_write(SMMU_GR1_CBA2R(0), SMMU_CBA2R_VA64);

	/* Disable translation while reprogramming the CB. */
	cb0_write(SMMU_CB_SCTLR, 0);

	cb0_write(SMMU_CB_S1_MAIR0, LPAE_MAIR0);
	cb0_write(SMMU_CB_S1_MAIR1, 0);
	cb0_write(SMMU_CB_TCR,
	          SMMU_TCR_EAE | SMMU_TCR_EPD1 | SMMU_TCR_SH0_IS |
	          SMMU_TCR_ORGN0_WBWA | SMMU_TCR_IRGN0_WBWA | LPAE_T0SZ);
	cb0_write(SMMU_CB_TCR2, SMMU_TCR2_SEP_UPSTREAM | SMMU_TCR2_PASIZE_40);
	cb0_write(SMMU_CB_CONTEXTIDR, LPAE_ASID);
	cb0_writeq(SMMU_CB_TTBR0, d->ttbr0);
	adreno_mb();

	/* Invalidate any stale CB0 TLB entries, then enable stage-1 translation
	   with the qcom adreno HUPCF quirk (keep processing after a fault). */
	cb0_write(SMMU_CB_S1_TLBIASID, LPAE_ASID);
	cb_tlb_sync();

	cb0_write(SMMU_CB_SCTLR, SMMU_SCTLR_M | SMMU_SCTLR_HUPCF);
	adreno_mb();

	/* Global: make sure the SMMU is out of CLIENT power-down and unmatched
	   transactions fault (so a missing stream map is visible, not silent). */
	scr0 = adreno_smmu_read(SMMU_GR0_sCR0);
	scr0 &= ~SMMU_sCR0_CLIENTPD;
	scr0 |= SMMU_sCR0_USFCFG;
	adreno_smmu_write(SMMU_GR0_sCR0, scr0);

	d->smmu_ready = 1;
	klog("adreno_smmu: CB0 ready pgshift=%u numcb=%u ttbr0=%llx\n",
	     d->pgshift, d->num_cb, (unsigned long long)d->ttbr0);
	return 0;
}

/* Read back the CB0 fault registers (for diagnostics on an IB fault). */
void adreno_smmu_dump_fault(adreno_dev_t *d) {
	(void)d;
	klog("adreno_smmu: FSR=%x FAR=%x\n",
	     cb0_read(SMMU_CB_FSR), cb0_read(SMMU_CB_FAR));
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

typedef struct {
	uint8_t  *base;      /* bump pool CPU base                        */
	uint64_t  base_pa;   /* bump pool physical base                   */
	uint32_t  off;       /* next free byte in the pool                */
	uint32_t  cap;       /* pool capacity in bytes                    */
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

/* Recover the CPU va of a child table from the physical address stored in a
   table descriptor.  Every child table comes from the contiguous bump pool,
   so va = pool_base + (child_pa - pool_base_pa). */
static uint64_t *lpae_pool_va(lpae_pool_t *p, uint64_t pa) {
	return (uint64_t*)(p->base + (pa - p->base_pa));
}

/* Map [iova, iova+size) -> [pa, pa+size) at 4 KB granularity into the LPAE
   tree rooted at `l1`, allocating L2/L3 tables from `pool`.  iova/size must
   be 4 KB aligned (the caller rounds the firmware windows). */
static int lpae_map(uint64_t *l1, lpae_pool_t *pool,
                    uint64_t iova, uint64_t pa, uint32_t size) {
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
		l3[i3] = (p & LPAE_PTE_ADDR_MASK) | LPAE_PROT_NORMAL | LPAE_PTE_TYPE_TABLE;
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

	cb1_page  = d->numpage + 1;   /* GMU is always CB1 */
	_cb1_byte = (uint32_t)(cb1_page << d->pgshift);

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
	if (lpae_map(l1, &pool, d->gmu_icache.iova, d->gmu_icache.pa, d->gmu_icache.size) != 0 ||
	    lpae_map(l1, &pool, d->gmu_dummy.iova,  d->gmu_dummy.pa,  d->gmu_dummy.size)  != 0 ||
	    lpae_map(l1, &pool, d->gmu_debug.iova,  d->gmu_debug.pa,  d->gmu_debug.size)  != 0 ||
	    lpae_map(l1, &pool, d->gmu_log.iova,    d->gmu_log.pa,    d->gmu_log.size)    != 0 ||
	    lpae_map(l1, &pool, d->gmu_hfi.iova,    d->gmu_hfi.pa,    d->gmu_hfi.size)    != 0) {
		klog("adreno_smmu: GMU CB1 page-map failed\n");
		return -4;
	}
	adreno_wmb();   /* publish descriptors before TTBR0 */

	d->gmu_ttbr0 = (d->gmu_pgtable.pa & LPAE_PTE_ADDR_MASK) |
	               ((uint64_t)LPAE_GMU_ASID << 48);

	/* Stream match: GMU StreamID 5 -> SMR group 1 -> CB1 (stage-1 translate). */
	adreno_smmu_write(SMMU_GR0_SMR(1), SMMU_SMR_VALID | SMMU_GMU_STREAMID);
	adreno_smmu_write(SMMU_GR0_S2CR(1), SMMU_S2CR_TYPE_TRANS | 1u /* CBNDX 1 */);

	gr1_write(SMMU_GR1_CBAR(1),
	          SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS | SMMU_CBAR_S1_MEMATTR_WB);
	gr1_write(SMMU_GR1_CBA2R(1), SMMU_CBA2R_VA64);

	cb1_write(SMMU_CB_SCTLR, 0);
	cb1_write(SMMU_CB_S1_MAIR0, LPAE_MAIR0);
	cb1_write(SMMU_CB_S1_MAIR1, 0);
	cb1_write(SMMU_CB_TCR,
	          SMMU_TCR_EAE | SMMU_TCR_EPD1 | SMMU_TCR_SH0_IS |
	          SMMU_TCR_ORGN0_WBWA | SMMU_TCR_IRGN0_WBWA | LPAE_T0SZ);
	cb1_write(SMMU_CB_TCR2, SMMU_TCR2_SEP_UPSTREAM | SMMU_TCR2_PASIZE_40);
	cb1_write(SMMU_CB_CONTEXTIDR, LPAE_GMU_ASID);
	cb1_writeq(SMMU_CB_TTBR0, d->gmu_ttbr0);
	adreno_mb();

	cb1_write(SMMU_CB_S1_TLBIASID, LPAE_GMU_ASID);
	cb1_tlb_sync();

	cb1_write(SMMU_CB_SCTLR, SMMU_SCTLR_M | SMMU_SCTLR_HUPCF);
	adreno_mb();

	d->smmu_gmu_ready = 1;
	klog("adreno_smmu: CB1 (GMU) ready ttbr0=%llx\n",
	     (unsigned long long)d->gmu_ttbr0);
	return 0;
}
