/* Query the official backend behind the native Q8B DSP adapter. */
#include <adrenonpu/npu_fastrpc.h>
#include <adrenonpu/npu_sdk.h>
#include <string.h>

static int dsp_probe(uint32_t session, uint32_t handle, unsigned method, npu_sdk_dsp_info_t *info)
{
    if (!info) return Q8B_NPU_PAS_INVALID;
    npu_sdk_dsp_info_t result = {0};
    frpc_rbuf_t arg = {.data=&result, .size=sizeof(result), .fd=-1, .is_out=true};
    int32_t rv = 0;
    int rc = frpc_invoke(session, handle, frpc_scalar(method, 0, 1), &arg, 1, NULL, 0, &rv);
    if (rc || rv) return rc ? rc : rv;
    if (result.magic != NPU_SDK_DSP_INFO_MAGIC) return Q8B_NPU_PAS_INVALID;
    *info = result;
    return 0;
}

int npu_sdk_dsp_query(uint32_t session, uint32_t handle, npu_sdk_dsp_info_t *info)
{
    return dsp_probe(session, handle, 2, info);
}
