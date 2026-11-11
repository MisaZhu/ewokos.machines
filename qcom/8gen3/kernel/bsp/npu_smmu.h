#ifndef Q8B_NPU_SMMU_H
#define Q8B_NPU_SMMU_H

#include <stdint.h>

int q8b_npu_smmu_enable(void);
int q8b_npu_smmu_ready(void);
int64_t q8b_npu_smmu_read_id(uint32_t offset);
int q8b_npu_smmu_map(uint64_t pa, uint64_t iova, uint32_t size);
int q8b_npu_smmu_unmap(uint64_t iova, uint32_t size);
int q8b_npu_smmu_stop(void);
void q8b_npu_smmu_dump_fault(void);

#endif
