/* FastRPC entry for opaque QAIRT contexts. No embedded graphs or self-tests. */
#include <QnnInterface.h>
#include <HTP/QnnHtpCommon.h>
#include <adrenonpu/npu_sdk.h>
#include "rpc.h"

void q8b_dsp_init(void) {}
void q8b_dsp_fini(void) {}

__attribute__((visibility("default")))
int q8b_npu_skel_handle_invoke(uint64_t handle, uint32_t sc, remote_arg *args) {
    sc &= 0x1fffffffu;
    if (!args) return 14;
    if (sc == 0x00020001u) {
        if (!args[0].buf.data || args[0].buf.size < 4 ||
                !args[1].buf.data || !args[1].buf.size) return 14;
        return q8b_model_open(&args[2].handle);
    }
    if (sc == 0x01000010u) return q8b_model_close(args[0].handle);
    unsigned method = (sc >> 24) & 31u;
    if (method >= 7 && method <= 14)
        return q8b_model_invoke(handle, sc, args);
    if (sc != 0x02000100u || !args[0].buf.data ||
            args[0].buf.size != sizeof(npu_sdk_dsp_info_t)) return 14;
    npu_sdk_dsp_info_t *out = args[0].buf.data;
    *out = (npu_sdk_dsp_info_t){.magic=NPU_SDK_DSP_INFO_MAGIC, .stage=1};
    const QnnInterface_t **providers = NULL;
    uint32_t count = 0;
    out->status = QnnInterface_getProviders(&providers, &count);
    if (out->status) return 0;
    for (uint32_t i = 0; providers && i < count; i++) {
        const QnnInterface_t *p = providers[i];
        if (!p || p->backendId != QNN_BACKEND_ID_HTP ||
                p->apiVersion.coreApiVersion.major != QNN_API_VERSION_MAJOR ||
                p->apiVersion.coreApiVersion.minor < QNN_API_VERSION_MINOR) continue;
        out->backend_id = p->backendId;
        out->major = p->apiVersion.coreApiVersion.major;
        out->minor = p->apiVersion.coreApiVersion.minor;
        out->patch = p->apiVersion.coreApiVersion.patch;
        out->stage = 2;
        return 0;
    }
    out->status = QNN_COMMON_ERROR_NOT_SUPPORTED;
    return 0;
}
