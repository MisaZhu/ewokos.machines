#include <bsp/smmu_route.h>

/* Encoders mirror npu_smmu.c exactly (S2CR packs CB in the low byte with
 * TYPE_TRANS, TTBR0 carries ASID in bits [63:48]). */

uint32_t qcom8gen3_smmu_smr_reg(unsigned sid, unsigned mask, bool valid) {
    uint32_t v = (sid & 0x7fffu) | ((mask & 0x7fffu) << 16);
    if (valid)
        v |= QCOM8GEN3_SMRU_VALID;
    return v;
}

uint32_t qcom8gen3_smmu_s2cr_reg(unsigned cb) {
    return QCOM8GEN3_S2CR_TYPE_TRANS | (cb & 0xffu);
}

uint32_t qcom8gen3_smmu_cbar_reg(unsigned cb) {
    (void)cb;
    return QCOM8GEN3_SMRU_CBAR_S1 | QCOM8GEN3_SMRU_CBAR_MEMATTR_WB |
           QCOM8GEN3_SMRU_CBAR_BPSHCFG_NSH;
}

uint32_t qcom8gen3_smmu_tcr(unsigned va_bits) {
    return QCOM8GEN3_SMRU_TCR_EPD1 | QCOM8GEN3_SMRU_TCR_SH_IS |
           QCOM8GEN3_SMRU_TCR_ORGN_WBWA | QCOM8GEN3_SMRU_TCR_IRGN_WBWA |
           (64u - va_bits);
}

uint32_t qcom8gen3_smmu_tcr2_reg(uint32_t id2) {
    return QCOM8GEN3_SMRU_TCR2_SEP | QCOM8GEN3_SMRU_TCR2_AS | QCOM8GEN3_SMRU_ID2_IAS(id2);
}

uint64_t qcom8gen3_smmu_ttbr0_reg(uint64_t root_pa, unsigned asid) {
    return (root_pa & QCOM8GEN3_SMRU_PTE_ADDR_MASK) | ((uint64_t)asid << 48);
}

uint32_t qcom8gen3_smmu_sctlr_reg(bool enable_m) {
    uint32_t v = QCOM8GEN3_SMRU_SCTLR_CFIE | QCOM8GEN3_SMRU_SCTLR_CFRE |
                 QCOM8GEN3_SMRU_SCTLR_AFE | QCOM8GEN3_SMRU_SCTLR_TRE |
                 QCOM8GEN3_SMRU_SCTLR_ASIDPNE;
    if (enable_m)
        v |= QCOM8GEN3_SMRU_SCTLR_M;
    return v;
}

uint64_t qcom8gen3_smmu_pte(uint64_t pa, bool table, uint64_t extra) {
    uint64_t v = (pa & QCOM8GEN3_SMRU_PTE_ADDR_MASK) | extra;
    v |= table ? QCOM8GEN3_SMRU_PTE_TABLE : QCOM8GEN3_SMRU_PTE_VALID;
    return v;
}

int qcom8gen3_smmu_build_route(uint32_t id1, uint32_t id2, unsigned va_bits,
                         unsigned smr, unsigned cb, uint64_t root_pa,
                         unsigned sid, unsigned sid_mask, unsigned pgshift,
                         qcom8gen3_smmu_route_t* out) {
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
    out->cba2r = QCOM8GEN3_SMRU_CBA2R_VA64;
    out->cbar = qcom8gen3_smmu_cbar_reg(cb);
    out->tcr2 = qcom8gen3_smmu_tcr2_reg(id2);
    out->tcr = qcom8gen3_smmu_tcr(va_bits);
    out->ttbr0 = qcom8gen3_smmu_ttbr0_reg(root_pa, asid);
    out->mair0 = QCOM8GEN3_SMRU_MAIR0_NC_WB_DEV;
    out->actlr = QCOM8GEN3_SMRU_ACTLR_FASTRPC;
    out->sctlr_off = qcom8gen3_smmu_sctlr_reg(false);
    out->sctlr_on = qcom8gen3_smmu_sctlr_reg(true);
    out->s2cr = qcom8gen3_smmu_s2cr_reg(cb);
    out->smr_val = qcom8gen3_smmu_smr_reg(sid, sid_mask, true);
    (void)numpage;
    (void)pgshift;
    return 0;
}

int qcom8gen3_smmu_find_smr(const uint32_t* smrs, unsigned nsmr) {
    for (unsigned i = 0; i < nsmr; i++)
        if (!(smrs[i] & QCOM8GEN3_SMRU_VALID))
            return (int)i;
    return -1;
}

int qcom8gen3_smmu_find_cb(const uint32_t* cb_sctlr, unsigned numcb) {
    for (unsigned i = 0; i < numcb; i++)
        if (!(cb_sctlr[i] & QCOM8GEN3_SMRU_SCTLR_M))
            return (int)i;
    return -1;
}