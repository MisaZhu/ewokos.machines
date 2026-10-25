#include <adrenonpu/npu_model.h>
#include <adrenonpu/npu_fastrpc.h>
#include <string.h>
#include <stdlib.h>

static int valid_info(const npu_sdk_dsp_info_t *info) {
    return info->magic==NPU_SDK_DSP_INFO_MAGIC;
}
static int valid_tensors(const npu_model_tensor_t *t, uint32_t count, uint32_t bytes) {
    if (!count || count>NPU_MODEL_MAX_TENSORS || !bytes || bytes>NPU_MODEL_MAX_BYTES) return 0;
    uint32_t end=0;
    for (uint32_t i=0;i<count;++i) {
        if (t[i].rank>NPU_MODEL_MAX_RANK || !t[i].bytes || t[i].bytes>bytes-end ||
                t[i].offset!=end || !memchr(t[i].name,0,sizeof(t[i].name))) return 0;
        for (uint32_t j=0;j<t[i].rank;++j) if (!t[i].dimensions[j]) return 0;
        end+=t[i].bytes;
    }
    return end==bytes;
}

int npu_model_load(uint32_t session, uint32_t handle, const void *binary,
        uint32_t binary_size, const char *graph_name, npu_model_info_t *info) {
    if (!binary || !binary_size || binary_size>NPU_MODEL_MAX_BYTES || !graph_name || !info)
        return Q8B_NPU_PAS_INVALID;
    size_t name_size=0;
    while (name_size<NPU_MODEL_NAME_BYTES && graph_name[name_size]) ++name_size;
    if (name_size==NPU_MODEL_NAME_BYTES) return Q8B_NPU_PAS_INVALID;
    npu_model_info_t reply={0};
    frpc_rbuf_t args[3]={
        {.data=(void *)binary,.size=binary_size,.fd=-1},
        {.data=(void *)graph_name,.size=(uint32_t)name_size+1,.fd=-1},
        {.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true}
    };
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(7,2,1),args,3,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply.info) || reply.abi!=NPU_MODEL_ABI) return Q8B_NPU_PAS_INVALID;
    if (!reply.info.status && (reply.info.stage!=29 ||
            !memchr(reply.graph_name,0,sizeof(reply.graph_name)) ||
            !valid_tensors(reply.inputs,reply.num_inputs,reply.input_bytes) ||
            !valid_tensors(reply.outputs,reply.num_outputs,reply.output_bytes))) return Q8B_NPU_PAS_INVALID;
    *info=reply;
    return 0;
}
int npu_model_execute(uint32_t session, uint32_t handle,
        const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
        npu_model_execution_t *execution) {
    if (!input || !input_bytes || input_bytes>NPU_MODEL_MAX_BYTES || !output ||
            !output_bytes || output_bytes>NPU_MODEL_MAX_BYTES || !execution) return Q8B_NPU_PAS_INVALID;
    npu_model_execution_t reply={0};
    frpc_rbuf_t args[3]={
        {.data=(void *)input,.size=input_bytes,.fd=-1},
        {.data=output,.size=output_bytes,.fd=-1,.is_out=true},
        {.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true}
    };
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(8,1,2),args,3,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply.info) || (!reply.info.status &&
            (reply.info.stage!=41 || reply.executions!=1))) return Q8B_NPU_PAS_INVALID;
    *execution=reply;
    return 0;
}
int npu_model_execute_batch(uint32_t session, uint32_t handle, uint32_t count,
        const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
        npu_model_batch_execution_t *execution) {
    if (!count || count>NPU_MODEL_MAX_BATCH || !input || !output || !execution ||
            !input_bytes || input_bytes>NPU_MODEL_MAX_BYTES || input_bytes%count ||
            !output_bytes || output_bytes>NPU_MODEL_MAX_BYTES || output_bytes%count)
        return Q8B_NPU_PAS_INVALID;
    npu_model_batch_execution_t reply={0};
    frpc_rbuf_t args[4]={
        {.data=&count,.size=sizeof(count),.fd=-1},
        {.data=(void *)input,.size=input_bytes,.fd=-1},
        {.data=output,.size=output_bytes,.fd=-1,.is_out=true},
        {.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true}
    };
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(13,2,2),args,4,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply.info) || reply.abi!=NPU_MODEL_BATCH_ABI ||
            reply.requested!=count || reply.reserved || reply.executions>count)
        return Q8B_NPU_PAS_INVALID;
    if (!reply.info.status && (reply.info.stage!=72 || reply.executions!=count))
        return Q8B_NPU_PAS_INVALID;
    if (reply.info.status && (reply.info.stage!=71 || reply.executions==count))
        return Q8B_NPU_PAS_INVALID;
    if (reply.executions) {
        uint64_t average=reply.execute_total_us/reply.executions;
        uint64_t ceiling=average+(reply.execute_total_us%reply.executions!=0);
        if (reply.execute_min_us>reply.execute_max_us || reply.execute_min_us>average ||
                reply.execute_max_us<ceiling || reply.execute_max_us>reply.execute_total_us)
            return Q8B_NPU_PAS_INVALID;
    } else if (reply.execute_total_us || reply.execute_min_us || reply.execute_max_us)
        return Q8B_NPU_PAS_INVALID;
    *execution=reply;
    return 0;
}

int npu_model_unload(uint32_t session, uint32_t handle, npu_sdk_dsp_info_t *info) {
    if (!info) return Q8B_NPU_PAS_INVALID;
    npu_sdk_dsp_info_t reply={0};
    frpc_rbuf_t arg={.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true};
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(9,0,1),&arg,1,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply) || (!reply.status && reply.stage!=35)) return Q8B_NPU_PAS_INVALID;
    *info=reply;
    return 0;
}

int npu_model_profile_set(uint32_t session, uint32_t handle, uint32_t level,
        npu_sdk_dsp_info_t *info) {
    if (!info || level>NPU_MODEL_PROFILE_DETAILED) return Q8B_NPU_PAS_INVALID;
    npu_sdk_dsp_info_t reply={0};
    frpc_rbuf_t args[2]={
        {.data=&level,.size=sizeof(level),.fd=-1},
        {.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true}
    };
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(10,1,1),args,2,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply) || (!reply.status && reply.stage!=53)) return Q8B_NPU_PAS_INVALID;
    *info=reply;
    return 0;
}

int npu_model_profile_get(uint32_t session, uint32_t handle, npu_model_profile_t *profile) {
    if (!profile) return Q8B_NPU_PAS_INVALID;
    npu_model_profile_t *reply=calloc(1,sizeof(*reply));
    if (!reply) return Q8B_NPU_PAS_FAILED;
    frpc_rbuf_t arg={.data=reply,.size=sizeof(*reply),.fd=-1,.is_out=true};
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(11,0,1),&arg,1,NULL,0,&rv);
    if (rc || rv) { free(reply); return rc ? rc : rv; }
    if (!valid_info(&reply->info) || reply->abi!=NPU_MODEL_PROFILE_ABI) goto invalid;
    if (!reply->info.status) {
        if (reply->info.stage!=55 || !reply->execution ||
                reply->level<NPU_MODEL_PROFILE_BASIC || reply->level>NPU_MODEL_PROFILE_DETAILED ||
                reply->count>NPU_MODEL_MAX_PROFILE_EVENTS ||
                (reply->flags&~(NPU_MODEL_PROFILE_TRUNCATED_EVENTS|NPU_MODEL_PROFILE_TRUNCATED_NAME))) goto invalid;
        for (uint32_t i=0;i<reply->count;++i) {
            const npu_model_profile_event_t *e=&reply->events[i];
            if (!memchr(e->identifier,0,sizeof(e->identifier))) goto invalid;
            if (e->parent==NPU_MODEL_PROFILE_ROOT) {
                if (e->depth) goto invalid;
            } else if (e->parent>=i || e->depth!=reply->events[e->parent].depth+1) goto invalid;
        }
    }
    *profile=*reply;
    free(reply);
    return 0;
invalid:
    free(reply);
    return Q8B_NPU_PAS_INVALID;
}

int npu_model_power_set(uint32_t session, uint32_t handle, uint32_t mode,
        npu_sdk_dsp_info_t *info) {
    if (!info || mode>NPU_MODEL_POWER_TURBO) return Q8B_NPU_PAS_INVALID;
    npu_sdk_dsp_info_t reply={0};
    frpc_rbuf_t args[2]={
        {.data=&mode,.size=sizeof(mode),.fd=-1},
        {.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true}
    };
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(12,1,1),args,2,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply) || (!reply.status && reply.stage!=63)) return Q8B_NPU_PAS_INVALID;
    *info=reply;
    return 0;
}

int npu_model_core_clock_get(uint32_t session, uint32_t handle,
        npu_model_core_clock_t *clock) {
    if (!clock) return Q8B_NPU_PAS_INVALID;
    npu_model_core_clock_t reply={0};
    frpc_rbuf_t arg={.data=&reply,.size=sizeof(reply),.fd=-1,.is_out=true};
    int32_t rv=0;
    int rc=frpc_invoke(session,handle,frpc_scalar(14,0,1),&arg,1,NULL,0,&rv);
    if (rc || rv) return rc ? rc : rv;
    if (!valid_info(&reply.info) || reply.abi!=NPU_MODEL_CORE_CLOCK_ABI ||
            (!reply.info.status && (reply.info.stage!=81 || !reply.core_hz)) ||
            (reply.info.status && (reply.info.stage!=80 || reply.core_hz || reply.sampled_us)))
        return Q8B_NPU_PAS_INVALID;
    *clock=reply;
    return 0;
}
