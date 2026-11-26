/* Host test for the SMMU S1 route builder (smmu_route.c): encoders must
 * match the board's npu_smmu.c conventions, slot selection must skip
 * occupied SMRs and enabled CBs.  No hardware writes possible here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <bsp/smmu_route.h>

int main(void) {
    /* SC8280XP-ish ID1/ID2 (as read by spitest --dma-route). */
    uint32_t id1 = 0x2000000au;   /* pages=2^5? ID1[30:28]=1 -> 4 pages */
    uint32_t id2 = 0x00005111u;   /* IAS=1 (OAS in [7:4]) */
    (void)id1; (void)id2;

    qcom8gen3_smmu_route_t r;
    assert(qcom8gen3_smmu_build_route(0x0au, 0x111u, 40, 3, 0, 0x10000000u,
                                0x576, 0, 14, &r) == 0);
    assert(r.smr == 3 && r.cb == 0);
    assert(r.cba2r == QCOM8GEN3_SMRU_CBA2R_VA64);
    assert(r.cbar == (QCOM8GEN3_SMRU_CBAR_S1 | QCOM8GEN3_SMRU_CBAR_MEMATTR_WB |
                      QCOM8GEN3_SMRU_CBAR_BPSHCFG_NSH));
    assert((r.tcr & 0x3f) == (64u - 40u));        /* T0SZ */
    assert(r.tcr & QCOM8GEN3_SMRU_TCR_EPD1);
    assert(r.tcr2 == (QCOM8GEN3_SMRU_TCR2_SEP | QCOM8GEN3_SMRU_TCR2_AS | 0x1u));
    assert(r.ttbr0 == ((uint64_t)(0x10000000u & (uint32_t)QCOM8GEN3_SMRU_PTE_ADDR_MASK) |
                      (0x1ull << 48)));           /* ASID = cb+1 */
    assert(r.mair0 == QCOM8GEN3_SMRU_MAIR0_NC_WB_DEV);
    assert((r.sctlr_off & QCOM8GEN3_SMRU_SCTLR_M) == 0);
    assert(r.sctlr_on & QCOM8GEN3_SMRU_SCTLR_M);
    assert(r.s2cr == (QCOM8GEN3_S2CR_TYPE_TRANS | 0u));
    assert(r.smr_val == (QCOM8GEN3_SMRU_VALID | 0x576u));
    /* SMR with an explicit mask */
    assert(qcom8gen3_smmu_smr_reg(0x576, 0x0200, true) ==
           (QCOM8GEN3_SMRU_VALID | 0x576u | (0x0200u << 16)));

    /* slot selection */
    uint32_t smrs[8] = { QCOM8GEN3_SMRU_VALID | 1, 0, QCOM8GEN3_SMRU_VALID | 3, 0, 0, 0, 0, 0 };
    assert(qcom8gen3_smmu_find_smr(smrs, 8) == 1);      /* first free */
    assert(qcom8gen3_smmu_find_smr(smrs, 1) == -1);     /* occupied */
    uint32_t cbs[8] = { QCOM8GEN3_SMRU_SCTLR_M, QCOM8GEN3_SMRU_SCTLR_M, 0, 0, 0, 0, 0, 0 };
    assert(qcom8gen3_smmu_find_cb(cbs, 8) == 2);        /* CB0/1 enabled */
    assert(qcom8gen3_smmu_find_cb(cbs, 3) == 2);        /* limit to 3 banks */
    uint32_t all_on[4] = { QCOM8GEN3_SMRU_SCTLR_M, QCOM8GEN3_SMRU_SCTLR_M, QCOM8GEN3_SMRU_SCTLR_M, QCOM8GEN3_SMRU_SCTLR_M };
    assert(qcom8gen3_smmu_find_cb(all_on, 4) == -1);

    /* guard: CB out of range */
    assert(qcom8gen3_smmu_build_route(0x0au, 0x111u, 40, 0, 0x80u, 0x10000000u,
                                0x576, 0, 14, &r) == -1);
    puts("qcom8gen3 SMMU route tests: ok");
    return 0;
}