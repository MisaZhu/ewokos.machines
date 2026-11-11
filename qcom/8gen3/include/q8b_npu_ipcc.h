/* NPU-owned IPCC notifications; register semantics from qcom-ipcc.c. */
#ifndef Q8B_NPU_IPCC_H
#define Q8B_NPU_IPCC_H
#include <stdint.h>
#define Q8B_NPU_IPCC_GLINK 0x00060000u
#define Q8B_NPU_IPCC_SMP2P 0x00060002u

/* Leave other clients pending for their owner. A bounded drain also avoids
 * monopolizing the CPU if the remote continuously sends notifications. */
static inline uint32_t q8b_npu_ipcc_take(uint32_t (*recv)(void *),
        void (*clear)(void *, uint32_t), void *data) {
    uint32_t signals = 0;
    for (unsigned n = 0; n < 32; n++) {
        uint32_t id = recv(data);
        if (id != Q8B_NPU_IPCC_GLINK && id != Q8B_NPU_IPCC_SMP2P) break;
        clear(data, id);
        signals |= id == Q8B_NPU_IPCC_SMP2P ? 4u : 1u;
    }
    return signals;
}
#endif
