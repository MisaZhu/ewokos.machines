#ifndef Q8B_SMMU_ROUTE_H
#define Q8B_SMMU_ROUTE_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Host-tested S1 route construction for a new apps-SMMU stream (candidate
 * for gaokun3 GPI0 SID 0x576).  Pure encoding: no MMIO, no SCM.  The bit
 * fields mirror the board code in q8b/kernel/bsp/npu_smmu.c (verified on
 * Q8B); SMR/S2CR/CBAR/CB and PTE layouts follow ARM SMMU v2 as used there.
 *
 * A real kernel bring-up still must pick an unused SMR and S1 context bank
 * from live registers, allocate a pinned root page table, and go through
 * TLBI + SCTLR enable with read-back checks, exactly like npu_smmu.
 */
#define Q8B_SMRU_VALID          (1u << 31)
#define Q8B_SMRU_SMASK          (0x7fffu << 16)
#define Q8B_S2CR_TYPE_TRANS     0u
#define Q8B_SMRU_CBAR_S1        (1u << 16)
#define Q8B_SMRU_CBAR_MEMATTR_WB (0xfu << 12)
#define Q8B_SMRU_CBAR_BPSHCFG_NSH (3u << 8)
#define Q8B_SMRU_CBA2R_VA64     (1u << 0)
#define Q8B_SMRU_TCR_EPD1       (1u << 23)
#define Q8B_SMRU_TCR_SH_IS      (3u << 12)
#define Q8B_SMRU_TCR_ORGN_WBWA  (1u << 10)
#define Q8B_SMRU_TCR_IRGN_WBWA  (1u << 8)
#define Q8B_SMRU_TCR2_SEP       (7u << 15)
#define Q8B_SMRU_TCR2_AS        (1u << 4)
#define Q8B_SMRU_ID2_IAS(x)     ((x) & 0xfu)
#define Q8B_SMRU_SCTLR_M        (1u << 0)
#define Q8B_SMRU_SCTLR_TRE      (1u << 1)
#define Q8B_SMRU_SCTLR_AFE      (1u << 2)
#define Q8B_SMRU_SCTLR_CFRE     (1u << 5)
#define Q8B_SMRU_SCTLR_CFIE     (1u << 6)
#define Q8B_SMRU_SCTLR_ASIDPNE  (1u << 12)
#define Q8B_SMRU_ACTLR_FASTRPC  0x00000303u
#define Q8B_SMRU_MAIR0_NC_WB_DEV 0xf404ff44u
#define Q8B_SMRU_PTE_VALID      ((uint64_t)1 << 0)
#define Q8B_SMRU_PTE_TABLE      ((uint64_t)3 << 0)
#define Q8B_SMRU_PTE_AF         ((uint64_t)1 << 10)
#define Q8B_SMRU_PTE_SH_OS      ((uint64_t)2 << 8)
#define Q8B_SMRU_PTE_AP_UNPRIV  ((uint64_t)1 << 6)
#define Q8B_SMRU_PTE_NG         ((uint64_t)1 << 11)
#define Q8B_SMRU_PTE_ADDR_MASK  ((((uint64_t)1 << 48) - 1) & ~0xfffull)

typedef struct {
    unsigned smr;               /* chosen SMR index */
    unsigned cb;                /* chosen context bank */
    uint32_t cba2r, cbar;
    uint32_t tcr2, tcr;
    uint64_t ttbr0;             /* root PA | (ASID << 48); ASID = cb+1 */
    uint32_t mair0, actlr;
    uint32_t sctlr_off, sctlr_on;   /* before/after SCTLR_M */
    uint32_t s2cr, smr_val;
} q8b_smmu_route_t;

/* Build a full S1 route for a stream.  id0/id1/id2/va_bits/root_pa/asid are
 * the platform facts; smr and cb are the chosen free slots. */
int q8b_smmu_build_route(uint32_t id1, uint32_t id2, unsigned va_bits,
                         unsigned smr, unsigned cb, uint64_t root_pa,
                         unsigned sid, unsigned sid_mask, unsigned pgshift,
                         q8b_smmu_route_t* out);

/* Find the first VALID-free SMR slot in a simulated SMR table. */
int q8b_smmu_find_smr(const uint32_t* smrs, unsigned nsmr);

/* Find the first context bank with SCTLR.M clear (mask of in-use banks). */
int q8b_smmu_find_cb(const uint32_t* cb_sctlr, unsigned numcb);

/* PTE and register encoders. */
uint64_t q8b_smmu_pte(uint64_t pa, bool table, uint64_t extra);
uint32_t q8b_smmu_smr_reg(unsigned sid, unsigned mask, bool valid);
uint32_t q8b_smmu_s2cr_reg(unsigned cb);
uint32_t q8b_smmu_cbar_reg(unsigned cb);
uint32_t q8b_smmu_tcr(unsigned va_bits);
uint32_t q8b_smmu_tcr2_reg(uint32_t id2);
uint64_t q8b_smmu_ttbr0_reg(uint64_t root_pa, unsigned asid);
uint32_t q8b_smmu_sctlr_reg(bool enable_m);

#endif