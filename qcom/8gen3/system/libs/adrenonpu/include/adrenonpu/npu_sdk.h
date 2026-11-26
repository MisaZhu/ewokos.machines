/* Native QCOM8GEN3 DSP adapter capability and version protocol. */
#ifndef ADRENONPU_NPU_SDK_H
#define ADRENONPU_NPU_SDK_H
#include <stdint.h>

/* Our own DSP adapter links the official native QNN backend. It does not
 * use the experimental graph envelope in npu_htp.c. */
#define NPU_SDK_NATIVE_URI \
    "file:///libqcom8gen3_npu.so?qcom8gen3_npu_skel_handle_invoke&_modver=1.0&_dom=cdsp"
#define NPU_SDK_DSP_INFO_MAGIC 0x51384450u
typedef struct {
    uint32_t magic, stage, backend_id, major, minor, patch;
    uint64_t status;
} npu_sdk_dsp_info_t;
int npu_sdk_dsp_query(uint32_t session, uint32_t handle, npu_sdk_dsp_info_t *info);
#endif
