#ifndef QCOM8GEN3_NPU_SMMU_H
#define QCOM8GEN3_NPU_SMMU_H

#include <stdint.h>

int qcom8gen3_npu_smmu_enable(void);
int qcom8gen3_npu_smmu_ready(void);
int64_t qcom8gen3_npu_smmu_read_id(uint32_t offset);
int qcom8gen3_npu_smmu_map(uint64_t pa, uint64_t iova, uint32_t size);
int qcom8gen3_npu_smmu_unmap(uint64_t iova, uint32_t size);
int qcom8gen3_npu_smmu_stop(void);
void qcom8gen3_npu_smmu_dump_fault(void);

#endif
