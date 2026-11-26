/* NPU-owned IPCC notifications; register semantics from qcom-ipcc.c. */
#ifndef QCOM8GEN3_NPU_IPCC_H
#define QCOM8GEN3_NPU_IPCC_H
#include <stdint.h>
#define QCOM8GEN3_NPU_IPCC_GLINK 0x00060000u
#define QCOM8GEN3_NPU_IPCC_SMP2P 0x00060002u

/* Leave other clients pending for their owner. A bounded drain also avoids
 * monopolizing the CPU if the remote continuously sends notifications. */
static inline uint32_t qcom8gen3_npu_ipcc_take(uint32_t (*recv)(void *),
        void (*clear)(void *, uint32_t), void *data) {
    uint32_t signals = 0;
    for (unsigned n = 0; n < 32; n++) {
        uint32_t id = recv(data);
        if (id != QCOM8GEN3_NPU_IPCC_GLINK && id != QCOM8GEN3_NPU_IPCC_SMP2P) break;
        clear(data, id);
        signals |= id == QCOM8GEN3_NPU_IPCC_SMP2P ? 4u : 1u;
    }
    return signals;
}
#endif
