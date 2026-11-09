/*
 * apps_smmu: S1 route bring-up for a new apps-SMMU stream (candidate for
 * gaokun3 gpi_dma0 SID 0x576).  NOT wired into any boot path: this is a
 * compile-checked blueprint mirroring npu_smmu.c conventions (SMR/S2CR/CBAR/
 * CB register programming with read-back validation and TLBI), with the
 * encoders host-tested by system/libs/bsp/tests/smmu_route_test.c.
 *
 * Call order (single owner, after interconnect/CX resources are held and a
 * pinned root page table exists):
 *
 *   a = apps_smmu_prepare();            // read ID0/ID1/ID2, find slots
 *   apps_smmu_program(&a, root_pa);     // CB+route programming + read-back
 *   apps_smmu_commit(&a);               // TLBI + SCTLR.M, verify
 *
 * Any failure poisons handle state: do not retry without a full reset of
 * the context bank.  mirro npu_smmu.c for the exact offsets.
 */
#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

#define SMMU_SIZE Q8B_APPS_SMMU_SIZE
#define SMMU_WIN  (MMIO_BASE + Q8B_APPS_SMMU_OFF)

#define GR0_ID0 0x20u
#define GR0_sCR0 0x0u
#define GR0_ID1 0x24u
#define GR0_ID2 0x28u
#define GR0_SMR(n) (0x800u + ((n) << 2))
#define GR0_S2CR(n) (0xc00u + ((n) << 2))
#define GR0_TLBIALLNSNH 0x68u
#define GR0_sTLBGSYNC 0x70u
#define GR0_sTLBGSTATUS 0x74u
#define GSTATUS_ACTIVE (1u << 0)
#define ID0_SMS (1u << 27)
#define SCR0_CLIENTPD (1u << 0)
#define SCR0_USFCFG (1u << 10)
#define ID0_NUMSMRG(x) ((x) & 0xffu)
#define ID1_NUMPAGES(x) (1u << ((((x) >> 28) & 7u) + 1u))
#define ID1_NUMCB(x) ((x) & 0xffu)
#define ID2_IAS(x) ((x) & 0xfu)

#define SMR_VALID (1u << 31)
#define S2CR_TYPE_TRANS 0u
#define GR1_CBAR(n) ((n) << 2)
#define GR1_CBA2R(n) (0x800u + ((n) << 2))
#define CBA2R_VA64 (1u << 0)
#define CBAR_S1 (1u << 16)
#define CBAR_MEMATTR_WB (0xfu << 12)
#define CBAR_BPSHCFG_NSH (3u << 8)

#define CB_SCTLR 0x0u
#define CB_ACTLR 0x4u
#define CB_TCR2 0x10u
#define CB_TTBR0 0x20u
#define CB_TTBR1 0x28u
#define CB_TCR 0x30u
#define CB_MAIR0 0x38u
#define CB_MAIR1 0x3cu
#define CB_TLBIASID 0x610u
#define CB_TLBSYNC 0x7f0u
#define CB_TLBSTATUS 0x7f4u
#define CB_TLB_ACTIVE (1u << 0)

#define SCTLR_M (1u << 0)
#define SCTLR_TRE (1u << 1)
#define SCTLR_AFE (1u << 2)
#define SCTLR_CFRE (1u << 5)
#define SCTLR_CFIE (1u << 6)
#define SCTLR_ASIDPNE (1u << 12)
#define ACTLR_FASTRPC 0x00000303u
#define TCR_EPD1 (1u << 23)
#define TCR_SH_IS (3u << 12)
#define TCR_ORGN_WBWA (1u << 10)
#define TCR_IRGN_WBWA (1u << 8)
#define TCR2_SEP (7u << 15)
#define TCR2_AS (1u << 4)
#define MAIR_NC_WB_DEV 0xf404ff44u
#define SMMU_PTE_ADDR_MASK ((((uint64_t)1 << 48) - 1) & ~0xfffull)

typedef struct {
    int ready;
    uint32_t saved_scr0;
    int saved_scr0_valid;
    uint32_t pgshift, numpage, numcb, nsmr, smr, pa_bits, cb;
    uint32_t va_bits, sid, sid_mask;
    uint64_t root_pa;
    int poisoned;
} q8b_smmu1_t;

static inline uint32_t r32(uintptr_t off) {
    return *(volatile uint32_t*)(SMMU_WIN + off);
}
static inline void w32(uintptr_t off, uint32_t v) {
    *(volatile uint32_t*)(SMMU_WIN + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline uint64_t r64(uintptr_t off) {
    return *(volatile uint64_t*)(SMMU_WIN + off);
}
static inline void w64(uintptr_t off, uint64_t v) {
    *(volatile uint64_t*)(SMMU_WIN + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int tlb_sync(void) {
    w32(GR0_TLBIALLNSNH, 0);
    w32(GR0_sTLBGSYNC, 0);
    for (unsigned i = 0; i < 1000; i++) {
        if (!(r32(GR0_sTLBGSTATUS) & GSTATUS_ACTIVE))
            return 0;
    }
    return -1;
}

/* Step 1: read ID registers and choose an unused SMR and S1 context bank.
 * cb_hint is the first bank to scan (skip GPU/NPU banks as the caller
 * knows).  Mirrors npu_smmu selection. */
int apps_smmu_prepare(q8b_smmu1_t* s, unsigned sid, unsigned sid_mask,
                      unsigned va_bits, unsigned cb_hint, unsigned pgshift) {
    if (!s || sid > 0x7fff || sid_mask > 0x7fff)
        return -1;
    /* pa_bits/nsmr from ID0/ID1/ID2; bank capacity from ID1. */
    uint32_t id0 = r32(GR0_ID0), id1 = r32(GR0_ID1), id2 = r32(GR0_ID2);
    if (!(id0 & ID0_SMS))
        return -1;
    s->pgshift = pgshift;
    s->numpage = ID1_NUMPAGES(id1);
    s->numcb = ID1_NUMCB(id1);
    s->nsmr = ID0_NUMSMRG(id0);
    s->pa_bits = 40;                 /* Q8B OAS=40; SLA2 not assumed */
    (void)id2;
    s->va_bits = va_bits;
    s->sid = sid;
    s->sid_mask = sid_mask;
    s->poisoned = 0;
    s->ready = 0;

    unsigned found_smr = 0xffffffffu;
    for (unsigned i = 0; i < s->nsmr; i++)
        if (!(r32(GR0_SMR(i)) & SMR_VALID)) {
            found_smr = i;
            break;
        }
    if (found_smr == 0xffffffffu)
        return -1;
    s->smr = found_smr;

    unsigned found_cb = 0xffffffffu;
    for (unsigned cb = cb_hint; cb < s->numcb; cb++) {
        uintptr_t cb_base = ((s->numpage + cb) << pgshift);
        if (!(r32(cb_base + CB_SCTLR) & SCTLR_M)) {
            found_cb = cb;
            break;
        }
    }
    if (found_cb == 0xffffffffu)
        return -1;
    s->cb = found_cb;
    return 0;
}

/* Step 2: program the CB and the SMR/S2CR route; verify every write. */
int apps_smmu_program(q8b_smmu1_t* s, uint64_t root_pa) {
    if (!s || s->poisoned)
        return -1;
    uintptr_t cb_base = ((s->numpage + s->cb) << s->pgshift);
    uint32_t scr0 = r32(GR0_sCR0);
    s->saved_scr0 = scr0;
    s->saved_scr0_valid = 1;
    w32(GR0_sCR0, scr0 & ~SCR0_CLIENTPD);
    if (r32(GR0_sCR0) & SCR0_CLIENTPD)
        goto fail;

    w32(GR1_CBA2R(s->cb), CBA2R_VA64);
    w32(GR1_CBAR(s->cb), CBAR_S1 | CBAR_MEMATTR_WB | CBAR_BPSHCFG_NSH);
    uint32_t tcr = TCR_EPD1 | TCR_SH_IS | TCR_ORGN_WBWA | TCR_IRGN_WBWA |
                   (64u - s->va_bits);
    uint32_t tcr2 = TCR2_SEP | TCR2_AS | 1u;  /* IAS=1 (40-bit IPA) */
    uint32_t asid = s->cb + 1u;
    w32(cb_base + CB_TCR2, tcr2);
    w32(cb_base + CB_TCR, tcr);
    w64(cb_base + CB_TTBR0, (root_pa & SMMU_PTE_ADDR_MASK) | ((uint64_t)asid << 48));
    w64(cb_base + CB_TTBR1, (uint64_t)asid << 48);
    w32(cb_base + CB_MAIR0, MAIR_NC_WB_DEV);
    w32(cb_base + CB_MAIR1, 0);
    w32(cb_base + CB_ACTLR, ACTLR_FASTRPC);
    w32(cb_base + CB_SCTLR, SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE |
            SCTLR_TRE | SCTLR_ASIDPNE);

    if (r32(GR1_CBA2R(s->cb)) != CBA2R_VA64 ||
            r32(GR1_CBAR(s->cb)) != (CBAR_S1 | CBAR_MEMATTR_WB | CBAR_BPSHCFG_NSH) ||
            r32(cb_base + CB_TCR) != tcr ||
            r64(cb_base + CB_TTBR0) !=
                ((root_pa & SMMU_PTE_ADDR_MASK) | ((uint64_t)asid << 48)) ||
            r32(cb_base + CB_MAIR0) != MAIR_NC_WB_DEV)
        goto fail;

    w32(GR0_S2CR(s->smr), S2CR_TYPE_TRANS | s->cb);
    w32(GR0_SMR(s->smr), SMR_VALID | s->sid | (s->sid_mask << 16));
    if ((r32(GR0_S2CR(s->smr)) & 0xffu) != s->cb ||
            r32(GR0_SMR(s->smr)) != (SMR_VALID | s->sid | (s->sid_mask << 16)))
        goto fail;
    s->root_pa = root_pa;
    s->ready = 1;
    return 0;
fail:
    s->poisoned = 1;
    return -1;
}

/* Step 3: invalidate the new ASID and enable translation, verifying SCTLR.M. */
int apps_smmu_commit(q8b_smmu1_t* s) {
    if (!s || !s->ready || s->poisoned)
        return -1;
    uintptr_t cb_base = ((s->numpage + s->cb) << s->pgshift);
    uint32_t asid = s->cb + 1u;
    w32(cb_base + CB_TLBIASID, asid);
    if (tlb_sync() != 0) {
        s->poisoned = 1;
        return -1;
    }
    w32(cb_base + CB_SCTLR, SCTLR_M | SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE |
            SCTLR_TRE | SCTLR_ASIDPNE);
    if (!(r32(cb_base + CB_SCTLR) & SCTLR_M)) {
        s->poisoned = 1;
        return -1;
    }
    return 0;
}