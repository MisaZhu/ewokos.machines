#include <bsp/smmu_route.h>

/* Encoders mirror npu_smmu.c exactly (S2CR packs CB in the low byte with
 * TYPE_TRANS, TTBR0 carries ASID in bits [63:48]). */

uint32_t q8b_smmu_smr_reg(unsigned sid, unsigned mask, bool valid) {
    uint32_t v = (sid & 0x7fffu) | ((mask & 0x7fffu) << 16);
    if (valid)
        v |= Q8B_SMRU_VALID;
    return v;
}

uint32_t q8b_smmu_s2cr_reg(unsigned cb) {
    return Q8B_S2CR_TYPE_TRANS | (cb & 0xffu);
}

uint32_t q8b_smmu_cbar_reg(unsigned cb) {
    (void)cb;
    return Q8B_SMRU_CBAR_S1 | Q8B_SMRU_CBAR_MEMATTR_WB |
           Q8B_SMRU_CBAR_BPSHCFG_NSH;
}

uint32_t q8b_smmu_tcr(unsigned va_bits) {
    return Q8B_SMRU_TCR_EPD1 | Q8B_SMRU_TCR_SH_IS |
           Q8B_SMRU_TCR_ORGN_WBWA | Q8B_SMRU_TCR_IRGN_WBWA |
           (64u - va_bits);
}

uint32_t q8b_smmu_tcr2_reg(uint32_t id2) {
    return Q8B_SMRU_TCR2_SEP | Q8B_SMRU_TCR2_AS | Q8B_SMRU_ID2_IAS(id2);
}

uint64_t q8b_smmu_ttbr0_reg(uint64_t root_pa, unsigned asid) {
    return (root_pa & Q8B_SMRU_PTE_ADDR_MASK) | ((uint64_t)asid << 48);
}

uint32_t q8b_smmu_sctlr_reg(bool enable_m) {
    uint32_t v = Q8B_SMRU_SCTLR_CFIE | Q8B_SMRU_SCTLR_CFRE |
                 Q8B_SMRU_SCTLR_AFE | Q8B_SMRU_SCTLR_TRE |
                 Q8B_SMRU_SCTLR_ASIDPNE;
    if (enable_m)
        v |= Q8B_SMRU_SCTLR_M;
    return v;
}

uint64_t q8b_smmu_pte(uint64_t pa, bool table, uint64_t extra) {
    uint64_t v = (pa & Q8B_SMRU_PTE_ADDR_MASK) | extra;
    v |= table ? Q8B_SMRU_PTE_TABLE : Q8B_SMRU_PTE_VALID;
    return v;
}

int q8b_smmu_build_route(uint32_t id1, uint32_t id2, unsigned va_bits,
                         unsigned smr, unsigned cb, uint64_t root_pa,
                         unsigned sid, unsigned sid_mask, unsigned pgshift,
                         q8b_smmu_route_t* out) {
    if (!out || sid > 0x7fffu || sid_mask > 0x7fffu)
        return -1;
    unsigned numcb = id1 & 0xffu;
    if (cb >= numcb)
        return -1;
    /* CB base for register reads: (numpage + cb) << pgshift (see npu.c). */
    unsigned numpage = 1u << (((id1 >> 28) & 7u) + 1u);
    unsigned asid = cb + 1u;
    out->smr = smr;
    out->cb = cb;
    out->cba2r = Q8B_SMRU_CBA2R_VA64;
    out->cbar = q8b_smmu_cbar_reg(cb);
    out->tcr2 = q8b_smmu_tcr2_reg(id2);
    out->tcr = q8b_smmu_tcr(va_bits);
    out->ttbr0 = q8b_smmu_ttbr0_reg(root_pa, asid);
    out->mair0 = Q8B_SMRU_MAIR0_NC_WB_DEV;
    out->actlr = Q8B_SMRU_ACTLR_FASTRPC;
    out->sctlr_off = q8b_smmu_sctlr_reg(false);
    out->sctlr_on = q8b_smmu_sctlr_reg(true);
    out->s2cr = q8b_smmu_s2cr_reg(cb);
    out->smr_val = q8b_smmu_smr_reg(sid, sid_mask, true);
    (void)numpage;
    (void)pgshift;
    return 0;
}

int q8b_smmu_find_smr(const uint32_t* smrs, unsigned nsmr) {
    for (unsigned i = 0; i < nsmr; i++)
        if (!(smrs[i] & Q8B_SMRU_VALID))
            return (int)i;
    return -1;
}

int q8b_smmu_find_cb(const uint32_t* cb_sctlr, unsigned numcb) {
    for (unsigned i = 0; i < numcb; i++)
        if (!(cb_sctlr[i] & Q8B_SMRU_SCTLR_M))
            return (int)i;
    return -1;
}