/* Q8B apps_smmu bring-up, following Linux arm-smmu.c/io-pgtable-arm.c. */
#include <stdint.h>
#include <string.h>
#include <kernel/proc.h>
#include <kernel/kernel.h>
#include <mm/dma.h>
#include <mm/mmu.h>
#include <kprintf.h>
#ifndef Q8B_NPU_DEBUG
#define Q8B_NPU_DEBUG 0
#endif
#define npu_debug(...) do { if(Q8B_NPU_DEBUG) printf(__VA_ARGS__); } while(0)
#include <q8b_npu.h>
#include "hw_arch.h"
#include "npu_smmu.h"

/* ID0/1/2 direct Device-memory reads verified on Q8B on 2026-10-24. */
#if defined(Q8B_BOARD) && !defined(Q8B_QEMU_TEST)
#define Q8B_NPU_SMMU_DIRECT_MMIO 1
#endif

#ifndef Q8B_QEMU_TEST
extern void q8b_scm_smc(uint64_t regs[8]);
#endif

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
#define ID1_PAGESIZE (1u << 31)
#define ID1_NUMPAGES(x) (1u << ((((x) >> 28) & 7u) + 1u))
#define ID1_NUMCB(x) ((x) & 0xffu)
#define ID2_OAS(x) (((x) >> 4) & 0xfu)
#define ID2_IAS(x) ((x) & 0xfu)
#define ID2_UBS(x) (((x) >> 8) & 0xfu)
#define ID2_PTFS_4K (1u << 12)
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
#define CB_FSR 0x58u
#define CB_FAR 0x60u
#define CB_FSYNR0 0x68u
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
#define ACTLR_FASTRPC 0x00000303u /* PREFETCH_DEEP | CPRE | CMTLB */
#define TCR_EPD1 (1u << 23)
#define TCR_SH_IS (3u << 12)
#define TCR_ORGN_WBWA (1u << 10)
#define TCR_IRGN_WBWA (1u << 8)
#define TCR2_SEP (7u << 15)
#define TCR2_AS (1u << 4)
#define PTE_VALID ((uint64_t)1 << 0)
#define PTE_TABLE ((uint64_t)3 << 0)
#define PTE_AF ((uint64_t)1 << 10)
#define PTE_SH_OS ((uint64_t)2 << 8)
#define PTE_AP_UNPRIV ((uint64_t)1 << 6)
#define PTE_NG ((uint64_t)1 << 11)
#define SMMU_PTE_ADDR_MASK ((((uint64_t)1 << 48) - 1) & ~0xfffull)
#define MAIR_NC_WB_DEV 0xf404ff44u
#define SID 0x3183u
#define SID_MASK 0x0420u
#define CB (smmu.cb)
#define ASID (CB + 1u)
#define IOVA_LIMIT 0x100000000ull
#define PT_MAX 512u
#define SCM_SVC_SMMU_PROGRAM 0x15u
#define SCM_SMMU_PT_FORMAT 0x01u
#define SCM_SMMU_CONFIG_ERRATA1 0x03u
#define SMMU_DUMMY_VAL 0xffffffffu /* Linux QCOM_DUMMY_VAL */

typedef struct { ewokos_addr_t pa, va; } npu_pt_t;
static struct {
    int ready;
    /* Device-memory ID reads are verified on Q8B; SCM remains a fallback
     * for builds without the board-specific direct access path. */
    int scm_io;
    uint32_t saved_scr0;
    int saved_scr0_valid;
    int cb_owned;
    uint32_t pgshift, numpage, numcb, nsmr, smr, pa_bits, cb;
    npu_pt_t root, l2[4], l3[4][PT_MAX];
} smmu;

static int smmu_scm_read(uint32_t off, uint32_t *value) {
#ifdef Q8B_QEMU_TEST
    (void)off; (void)value;
    return -1;
#else
    uint64_t regs[8] = {
        0xc2000000u | 0x501u, 1, Q8B_APPS_SMMU_PHY + off,
        0, 0, 0, 0, 0
    };
    q8b_scm_smc(regs);
    if ((int64_t)regs[0] != 0) {
        printf("q8b npu: scm io read off=%x a0=%lld a1=%llx\n", off,
               (long long)(int64_t)regs[0],
               (unsigned long long)regs[1]);
        return -1;
    }
    *value = (uint32_t)regs[1];
    return 0;
#endif
}

static int smmu_scm_write(uint32_t off, uint32_t value) {
#ifdef Q8B_QEMU_TEST
    (void)off; (void)value;
    return -1;
#else
    uint64_t regs[8] = {
        0xc2000000u | 0x502u, 2, Q8B_APPS_SMMU_PHY + off,
        value, 0, 0, 0, 0
    };
    q8b_scm_smc(regs);
    if ((int64_t)regs[0] != 0) {
        printf("q8b npu: scm io write off=%x val=%x a0=%lld a1=%llx\n",
               off, value, (long long)(int64_t)regs[0],
               (unsigned long long)regs[1]);
        return -1;
    }
    return 0;
#endif
}

static inline uint32_t smmu_read(uint32_t off) {
    uint32_t value = 0;
    if (smmu.scm_io) {
        if (smmu_scm_read(off, &value) == 0) return value;
        return UINT32_MAX;
    }
#if defined(Q8B_NPU_SMMU_DIRECT_MMIO) && !defined(Q8B_QEMU_TEST)
    return *(volatile uint32_t *)(uintptr_t)(SMMU_WIN + off);
#else
    return UINT32_MAX;
#endif
}
static inline int smmu_write(uint32_t off, uint32_t value) {
    if (smmu.scm_io) return smmu_scm_write(off, value);
#if defined(Q8B_NPU_SMMU_DIRECT_MMIO) && !defined(Q8B_QEMU_TEST)
    *(volatile uint32_t *)(uintptr_t)(SMMU_WIN + off) = value;
    return 0;
#else
    (void)off; (void)value;
    return -1;
#endif
}

static inline uint32_t gr0r(uint32_t off) { return smmu_read(off); }
static inline int gr0w(uint32_t off, uint32_t v) { return smmu_write(off, v); }
static inline uint32_t gr1r(uint32_t off) { return smmu_read((1u << smmu.pgshift) + off); }
static inline int gr1w(uint32_t off, uint32_t v) { return smmu_write((1u << smmu.pgshift) + off, v); }
static inline uint32_t cbr(uint32_t off) { return smmu_read(((smmu.numpage + CB) << smmu.pgshift) + off); }
static inline int cbw(uint32_t off, uint32_t v) { return smmu_write(((smmu.numpage + CB) << smmu.pgshift) + off, v); }
static inline int cbwq(uint32_t off, uint64_t v) {
    return cbw(off, (uint32_t)v) || cbw(off + 4, (uint32_t)(v >> 32));
}
static inline uint64_t cbrq(uint32_t off) { return (uint64_t)cbr(off) | ((uint64_t)cbr(off + 4) << 32); }
static inline uint64_t pt_addr(ewokos_addr_t pa) { return ((uint64_t)pa) & SMMU_PTE_ADDR_MASK; }

/* platform_control runs on the caller's address space on EwokOS.  The
 * architecture map installs the SMMU window in the kernel VM, but a user
 * FastRPC process may still be using its own page table when this function
 * is entered.  Mirror the PAS driver's caller mapping before the first MMIO
 * read; otherwise the faulting SMMU access can strand the syscall without a
 * diagnostic. */
#ifndef Q8B_QEMU_TEST
static int map_smmu_in_caller(void) {
    proc_t *p = proc_get_proc(get_current_proc());
    page_dir_entry_t *pvm = (p && p->space) ? p->space->vm : 0;

    if (map_pages_size(_kernel_info.kernel_vm, SMMU_WIN,
                Q8B_APPS_SMMU_PHY, (uint32_t)SMMU_SIZE,
                AP_RW_D, PTE_ATTR_DEV) != 0)
        return -1;
    if (pvm != 0 && pvm != _kernel_info.kernel_vm &&
            map_pages_size(pvm, SMMU_WIN, Q8B_APPS_SMMU_PHY,
                (uint32_t)SMMU_SIZE, AP_RW_D, PTE_ATTR_DEV) != 0)
        return -1;
    flush_tlb();
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
#endif

/* Explicit bring-up diagnostic, separate from enable(): never program a
 * context bank or mark the transport ready. Only architectural ID registers
 * are allowed. SCM rejecting an address does not establish MMIO ownership;
 * sc8280xp.dtsi reserves the PCIe SMMU, not apps_smmu, for QHEE. */
int64_t q8b_npu_smmu_read_id(uint32_t offset) {
    if (offset != GR0_ID0 && offset != GR0_ID1 && offset != GR0_ID2)
        return Q8B_NPU_PAS_INVALID;
#ifdef Q8B_QEMU_TEST
    return Q8B_NPU_PAS_UNSUPPORTED;
#else
    if (map_smmu_in_caller() != 0)
        return Q8B_NPU_PAS_FAILED;
    npu_debug("q8b npu: explicit Device-memory ID read pa=%llx\n",
            (unsigned long long)(Q8B_APPS_SMMU_PHY + offset));
    uint32_t value = *(volatile uint32_t *)(uintptr_t)(SMMU_WIN + offset);
    __asm__ volatile("dsb sy" ::: "memory");
    npu_debug("q8b npu: apps_smmu ID off=%x value=%x\n", offset, value);
    return (int64_t)value;
#endif
}

static int dma_pt_alloc(npu_pt_t *p) {
    p->pa = dma_alloc(0, -1, 0x1000);
    if (!p->pa) return -1;
    if (smmu.pa_bits && (uint64_t)p->pa + 0x1000 > (1ull << smmu.pa_bits)) {
        dma_free(0, -1, p->pa);
        p->pa = 0;
        return -1;
    }
    p->va = dma_v_addr(0, p->pa);
    if (!p->va) {
        dma_free(0, -1, p->pa);
        p->pa = 0;
        return -1;
    }
    /* Kernel-owned DMA is not automatically installed in the syscall
     * caller's VM. The CPU uses 16K pages even though the SMMU table is 4K. */
    proc_t *caller = proc_get_proc(get_current_proc());
    page_dir_entry_t *vm = (caller && caller->space) ? caller->space->vm : 0;
    if (map_pages_size(_kernel_info.kernel_vm, p->va, p->pa, PAGE_SIZE,
                AP_RW_D, PTE_ATTR_SYS_DMA) != 0 ||
            (vm && vm != _kernel_info.kernel_vm &&
             map_pages_size(vm, p->va, p->pa, PAGE_SIZE,
                 AP_RW_D, PTE_ATTR_SYS_DMA) != 0)) {
        dma_free(0, -1, p->pa);
        p->pa = p->va = 0;
        return -1;
    }
    flush_tlb();
    memset((void *)(uintptr_t)p->va, 0, 0x1000);
    return 0;
}
static int tlb_sync(void) {
    for (unsigned i = 0; i < 100000; i++) {
        if (cbw(CB_TLBSYNC, SMMU_DUMMY_VAL) != 0) return -1;
        if (!(cbr(CB_TLBSTATUS) & CB_TLB_ACTIVE)) return 0;
    }
    return -1;
}
static int global_sync(void) {
    if (gr0w(GR0_sTLBGSYNC, SMMU_DUMMY_VAL) != 0) return -1;
    for (unsigned i = 0; i < 100000; i++)
        if (!(gr0r(GR0_sTLBGSTATUS) & GSTATUS_ACTIVE)) return 0;
    return -1;
}
static int find_l3(uint32_t idx, int create, uint64_t **out) {
    uint32_t ridx = idx >> 9;
    uint32_t l2idx = idx & 0x1ffu;
    uint64_t *l2;
    npu_pt_t *p;
    if (ridx >= 4) return -1;
    l2 = (uint64_t *)(uintptr_t)smmu.l2[ridx].va;
    if (!l2) {
        if (!create || dma_pt_alloc(&smmu.l2[ridx]) != 0) return -1;
        l2 = (uint64_t *)(uintptr_t)smmu.l2[ridx].va;
        ((uint64_t *)(uintptr_t)smmu.root.va)[ridx] =
                pt_addr(smmu.l2[ridx].pa) | PTE_TABLE;
        __asm__ volatile("dmb sy" ::: "memory");
    }
    if (!(l2[l2idx] & PTE_VALID)) {
        if (!create) return -1;
        p = &smmu.l3[ridx][l2idx];
        if (!p->pa && dma_pt_alloc(p) != 0) return -1;
        l2[l2idx] = pt_addr(p->pa) | PTE_TABLE;
        __asm__ volatile("dmb sy" ::: "memory");
    }
    p = &smmu.l3[ridx][l2idx];
    if (!p->va) return -1;
    *out = (uint64_t *)(uintptr_t)p->va;
    return 0;
}

int q8b_npu_smmu_enable(void) {
#ifdef Q8B_QEMU_TEST
    return -1;
#else
    uint32_t id0, id1, id2, i, cb_base, scr0;
    static const uint8_t address_bits[] = {32, 36, 40, 42, 44, 48};
    uint32_t tcr, tcr2, va_bits;
    uint8_t reserved_cb[256] = {0};
    if (smmu.ready) return 0;
    if (smmu.root.pa) return -1; /* failed teardown requires board recovery */
    npu_debug("q8b npu: apps_smmu enable enter\n");
    smmu.smr = UINT32_MAX;
    smmu.pgshift = 12;
    smmu.scm_io = 0;
    /* SC8280XP uses qcom_smmu_500_impl, not the SDM845 SAFE-toggle
     * reset quirk. Direct Device-memory ID reads have been verified on Q8B.
     * Do not call unrelated SCM reset/aperture services before probing. */
#if defined(Q8B_NPU_SMMU_DIRECT_MMIO)
    if (map_smmu_in_caller() != 0) goto fail;
    id0 = gr0r(GR0_ID0); id1 = gr0r(GR0_ID1); id2 = gr0r(GR0_ID2);
#else
    if (!smmu_scm_io_available() || smmu_scm_read(GR0_ID0, &id0) != 0)
        goto fail;
    smmu.scm_io = 1;
    if (smmu_scm_read(GR0_ID1, &id1) != 0 ||
            smmu_scm_read(GR0_ID2, &id2) != 0)
        goto fail;
#endif
    smmu.pgshift = (id1 & ID1_PAGESIZE) ? 16 : 12;
    smmu.numpage = ID1_NUMPAGES(id1); smmu.numcb = ID1_NUMCB(id1);
    smmu.nsmr = ID0_NUMSMRG(id0);
    npu_debug("q8b npu: apps_smmu id0=%x id1=%x id2=%x pg=%u pages=%u cb=%u smr=%u\n",
           id0, id1, id2, smmu.pgshift, smmu.numpage, smmu.numcb, smmu.nsmr);
    /* Unlike Linux's full-device reset, preserve firmware's unmatched-stream
     * policy and existing context banks. Only take an unused CB and SMR. */
    if (!(id0 & (1u << 30)) || !(id0 & ID0_SMS) || !smmu.nsmr || !smmu.numcb ||
            ((smmu.numpage + smmu.numcb) << smmu.pgshift) > SMMU_SIZE ||
            !(id2 & ID2_PTFS_4K) || ID2_UBS(id2) > 5 ||
            ID2_IAS(id2) > 5 || ID2_OAS(id2) > 5)
        goto fail;
    va_bits = address_bits[ID2_UBS(id2)];
    if (va_bits > 39) va_bits = 39; /* three-level 4K tables */
    smmu.pa_bits = address_bits[ID2_IAS(id2) < ID2_OAS(id2) ?
            ID2_IAS(id2) : ID2_OAS(id2)];
    smmu.smr = UINT32_MAX;
    for (i = 0; i < smmu.nsmr; i++) {
        uint32_t smr = gr0r(GR0_SMR(i));
        if (smr & SMR_VALID) {
            uint32_t s2cr = gr0r(GR0_S2CR(i));
            uint32_t mask = (smr >> 16) & 0x7fffu;
            if (((s2cr >> 16) & 3u) == S2CR_TYPE_TRANS)
                reserved_cb[s2cr & 0xffu] = 1;
            if (((smr ^ SID) & ~(mask | SID_MASK) & 0x7fffu) == 0) {
                printf("q8b npu: SID overlaps SMR%u=%x S2CR=%x\n", i, smr, s2cr);
                goto fail; /* do not overlap firmware's route */
            }
        } else if (smmu.smr == UINT32_MAX) {
            smmu.smr = i;
        }
    }
    if (smmu.smr == UINT32_MAX) goto fail;
    /* compute-cb@3's reg is a DSP address tag, not hardware CB number 3.
     * As Linux does, allocate an unused S1 bank independently of that tag. */
    for (smmu.cb = (id1 >> 16) & 0xffu; smmu.cb < smmu.numcb; smmu.cb++)
        if (!reserved_cb[smmu.cb] && !(cbr(CB_SCTLR) & SCTLR_M)) break;
    if (smmu.cb == smmu.numcb) {
        printf("q8b npu: no unused S1 context bank\n");
        goto fail;
    }
    cb_base = (smmu.numpage + CB) << smmu.pgshift;
    npu_debug("q8b npu: selected CB%u offset=%x SMR%u\n", CB, cb_base, smmu.smr);
    scr0 = gr0r(GR0_sCR0);
    if (scr0 == UINT32_MAX)
        goto fail;
    smmu.saved_scr0 = scr0;
    smmu.saved_scr0_valid = 1;
    scr0 &= ~SCR0_CLIENTPD;
    if (gr0w(GR0_sCR0, scr0) != 0 || gr0r(GR0_sCR0) != scr0)
        goto fail;
    if (gr0r(GR0_sCR0) & SCR0_CLIENTPD)
        goto fail;
    if (dma_pt_alloc(&smmu.root) != 0) goto fail;
    smmu.cb_owned = 1;
    if (gr1w(GR1_CBA2R(CB), CBA2R_VA64) != 0 ||
            gr1w(GR1_CBAR(CB), CBAR_S1 | CBAR_MEMATTR_WB | CBAR_BPSHCFG_NSH) != 0)
        goto fail;
    /* Mainline uses EAE only for AArch32 LPAE. VA64 selects AArch64 here.
     * S1 output size comes from ID2.IAS (IPA), not ID2.OAS (S2 output). */
    tcr = TCR_EPD1 | TCR_SH_IS | TCR_ORGN_WBWA | TCR_IRGN_WBWA |
            (64u - va_bits);
    tcr2 = TCR2_SEP | TCR2_AS | ID2_IAS(id2);
    if (cbw(CB_TCR2, tcr2) != 0 || cbw(CB_TCR, tcr) != 0 ||
            cbwq(CB_TTBR0, pt_addr(smmu.root.pa) | ((uint64_t)ASID << 48)) != 0 ||
            cbwq(CB_TTBR1, ((uint64_t)ASID << 48)) != 0 ||
            cbw(CB_MAIR0, MAIR_NC_WB_DEV) != 0 || cbw(CB_MAIR1, 0) != 0 ||
            cbw(CB_ACTLR, ACTLR_FASTRPC) != 0 ||
            cbw(CB_SCTLR, SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE |
                SCTLR_TRE | SCTLR_ASIDPNE) != 0)
        goto fail;
    npu_debug("q8b npu: CB%u readback cba2r=%x cbar=%x tcr=%x/%x ttbr=%llx mair=%x actlr=%x\n",
            CB, gr1r(GR1_CBA2R(CB)), gr1r(GR1_CBAR(CB)), cbr(CB_TCR), cbr(CB_TCR2),
            (unsigned long long)cbrq(CB_TTBR0), cbr(CB_MAIR0), cbr(CB_ACTLR));
    if (gr1r(GR1_CBA2R(CB)) != CBA2R_VA64 ||
            gr1r(GR1_CBAR(CB)) != (CBAR_S1 | CBAR_MEMATTR_WB | CBAR_BPSHCFG_NSH) ||
            cbr(CB_TCR) != tcr ||
            (cbr(CB_TCR2) & (TCR2_SEP | TCR2_AS | 0xfu)) != tcr2 ||
            cbrq(CB_TTBR0) != (pt_addr(smmu.root.pa) | ((uint64_t)ASID << 48)) ||
            cbr(CB_MAIR0) != MAIR_NC_WB_DEV || cbr(CB_ACTLR) != ACTLR_FASTRPC)
        goto fail;
    if (gr0w(GR0_S2CR(smmu.smr), S2CR_TYPE_TRANS | CB) != 0 ||
            gr0w(GR0_SMR(smmu.smr), SMR_VALID | SID | (SID_MASK << 16)) != 0)
        goto fail;
    if ((gr0r(GR0_S2CR(smmu.smr)) & 0xffu) != CB || gr0r(GR0_SMR(smmu.smr)) != (SMR_VALID | SID | (SID_MASK << 16))) goto fail;
    if (cbw(CB_TLBIASID, ASID) != 0 || tlb_sync() != 0) goto fail;
    if (cbw(CB_SCTLR, SCTLR_M | SCTLR_CFIE | SCTLR_CFRE | SCTLR_AFE |
                SCTLR_TRE | SCTLR_ASIDPNE) != 0)
        goto fail;
    if (!(cbr(CB_SCTLR) & SCTLR_M)) goto fail;
    smmu.ready = 1;
    npu_debug("q8b npu: apps_smmu CB%u ready smr=%u root=%llx\n", CB, smmu.smr, (unsigned long long)smmu.root.pa);
    return 0;
fail:
    printf("q8b npu: apps_smmu setup/readback failed\n");
    q8b_npu_smmu_stop();
    return -1;
#endif
}

int q8b_npu_smmu_ready(void) { return smmu.ready; }

int q8b_npu_smmu_map(uint64_t pa, uint64_t iova, uint32_t size) {
    uint64_t off, end, mapped_size, *l3;
    if (!smmu.ready || !size || (pa & 0xfff) || (iova & 0xfff)) return -1;
    mapped_size = ((uint64_t)size + 0xfffull) & ~0xfffull;
    if (mapped_size > UINT64_MAX - pa ||
            pa + mapped_size > (1ull << smmu.pa_bits)) return -1;
    end = iova + mapped_size;
    if (end < iova || end > IOVA_LIMIT) return -1;
    for (off = 0; off < mapped_size; off += 0x1000) {
        uint64_t va = iova + off;
        uint32_t rooti = (uint32_t)((va >> 30) & 0x1ffu);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ffu);
        uint32_t l3i = (uint32_t)((va >> 12) & 0x1ffu);
        if (find_l3(rooti * 512u + l2i, 1, &l3) != 0 || (l3[l3i] & PTE_VALID)) {
            /* Leave no partially published mapping when a bounded table
             * allocation or duplicate check fails. */
            for (uint64_t undo = 0; undo < off; undo += 0x1000) {
                uint64_t uva = iova + undo;
                uint32_t ur = (uint32_t)((uva >> 30) & 0x1ffu);
                uint32_t ul2 = (uint32_t)((uva >> 21) & 0x1ffu);
                uint32_t ul3 = (uint32_t)((uva >> 12) & 0x1ffu);
                if (find_l3(ur * 512u + ul2, 0, &l3) == 0) l3[ul3] = 0;
            }
            return -1;
        }
        l3[l3i] = pt_addr(pa + off) | PTE_TABLE | PTE_AF | PTE_SH_OS | PTE_AP_UNPRIV | PTE_NG;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    if (cbw(CB_TLBIASID, ASID) == 0 && tlb_sync() == 0)
        return 0;
    /* A failed invalidate must not leave a half-published mapping behind.
     * Clear every PTE we wrote and make one best-effort invalidate before
     * reporting failure to the FastRPC ownership table. */
    for (off = 0; off < mapped_size; off += 0x1000) {
        uint64_t va = iova + off;
        uint32_t rooti = (uint32_t)((va >> 30) & 0x1ffu);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ffu);
        uint32_t l3i = (uint32_t)((va >> 12) & 0x1ffu);
        if (find_l3(rooti * 512u + l2i, 0, &l3) == 0)
            l3[l3i] = 0;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    if (cbw(CB_TLBIASID, ASID) == 0)
        (void)tlb_sync();
    return -1;
}
int q8b_npu_smmu_unmap(uint64_t iova, uint32_t size) {
    uint64_t off, mapped_size, *l3;
    if (!smmu.ready || !size || (iova & 0xfff)) return -1;
    mapped_size = ((uint64_t)size + 0xfffull) & ~0xfffull;
    if (mapped_size > UINT64_MAX - iova || iova + mapped_size > IOVA_LIMIT)
        return -1;
    /* Validate the full range before clearing any PTE.  This keeps a bad
     * ownership record from turning into a partially unmapped buffer. */
    for (off = 0; off < mapped_size; off += 0x1000) {
        uint64_t va = iova + off;
        uint32_t rooti = (uint32_t)((va >> 30) & 0x1ffu);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ffu);
        uint32_t l3i = (uint32_t)((va >> 12) & 0x1ffu);
        if (find_l3(rooti * 512u + l2i, 0, &l3) != 0) return -1;
        if (!(l3[l3i] & PTE_VALID)) return -1;
    }
    for (off = 0; off < mapped_size; off += 0x1000) {
        uint64_t va = iova + off;
        uint32_t rooti = (uint32_t)((va >> 30) & 0x1ffu);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ffu);
        uint32_t l3i = (uint32_t)((va >> 12) & 0x1ffu);
        if (find_l3(rooti * 512u + l2i, 0, &l3) != 0) return -1;
        l3[l3i] = 0;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    if (cbw(CB_TLBIASID, ASID) != 0) return -1;
    return tlb_sync();
}
/* Snapshot before PAS reset/translation teardown destroys the fault state.
 * These registers are read-only here: do not clear a stalled transaction. */
void q8b_npu_smmu_dump_fault(void) {
#ifndef Q8B_QEMU_TEST
    if (!smmu.cb_owned) return;
    uint32_t fsr = cbr(CB_FSR);
    if (!(fsr & 0xc00001feu)) return;
    printf("q8b npu: SMMU fault snapshot CB%u fault=%u fsr=%x far=%llx fsynr0=%x cbfrsynra=%x\n",
            CB, !!(fsr & 0xc00001feu), fsr, (unsigned long long)cbrq(CB_FAR), cbr(CB_FSYNR0),
            gr1r(0x400u + (CB << 2)));
#endif
}

int q8b_npu_smmu_stop(void) {
#ifndef Q8B_QEMU_TEST
    smmu.ready = 0;
    if (smmu.cb_owned) {
        /* Match arm_smmu_device_reset(): stop translation first, invalidate
         * walk/TLB state, and only then release the DMA page tables. */
        if (cbw(CB_SCTLR, 0) != 0 || cbr(CB_SCTLR) != 0 ||
                gr0w(GR0_SMR(smmu.smr), 0) != 0 ||
                cbw(CB_TLBIASID, ASID) != 0 || tlb_sync() != 0 ||
                global_sync() != 0) {
            printf("q8b npu: retaining page tables after failed SMMU teardown\n");
            return -1;
        }
    }
    if (smmu.saved_scr0_valid)
        (void)gr0w(GR0_sCR0, smmu.saved_scr0);
#endif
    if (smmu.root.pa) dma_free(0, -1, smmu.root.pa);
    for (uint32_t r = 0; r < 4; r++) {
        for (uint32_t i = 0; i < PT_MAX; i++)
            if (smmu.l3[r][i].pa) dma_free(0, -1, smmu.l3[r][i].pa);
        if (smmu.l2[r].pa) dma_free(0, -1, smmu.l2[r].pa);
    }
    memset(&smmu, 0, sizeof(smmu)); smmu.smr = UINT32_MAX;
    return 0;
}
