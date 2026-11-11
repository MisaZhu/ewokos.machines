/* Persistent native QNN sessions. Metadata is parsed by the official DSP
 * System API and retained until context teardown. No graph IDs are assumed. */
#include "rpc.h"
#include <adrenonpu/npu_model.h>
#ifndef Q8B_DSP_LOADER_ONLY
#include <stdbool.h>
/* This SDK uses C's static_assert spelling without including assert.h.
 * The freestanding DSP adapter does not depend on a libc assert handler. */
#ifndef static_assert
#define static_assert _Static_assert
#endif
#include <QnnInterface.h>
#include <HTP/QnnHtpCommon.h>
#include <HTP/QnnHtpDevice.h>
#include <System/QnnSystemInterface.h>
#endif

extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *);
extern uint64_t HAP_perf_get_time_us(void);
#ifndef Q8B_DSP_LOADER_ONLY
/* Minimal external HAP ABI: response selector at 0, 8-byte union at 8.
 * Confirmed by this SDK's V66 getter caller and Q8B .35 firmware getter.
 * Keep the optional import weak so older firmware can still load the adapter. */
struct hap_clock_response { uint32_t type, padding; uint64_t value; };
extern int HAP_power_get(void *, struct hap_clock_response *)
#ifdef __APPLE__
    __attribute__((weak_import));
#else
    __attribute__((weak));
#endif
_Static_assert(sizeof(struct hap_clock_response)==16, "HAP response ABI");
_Static_assert(offsetof(struct hap_clock_response,value)==8, "HAP response value");
#endif

_Static_assert(sizeof(npu_model_tensor_t) == 128, "tensor wire ABI");
_Static_assert(sizeof(npu_model_info_t) == 1176, "model wire ABI");
_Static_assert(sizeof(npu_model_execution_t) == 48, "execution wire ABI");
_Static_assert(sizeof(npu_model_batch_execution_t) == 72, "batch execution wire ABI");
_Static_assert(sizeof(npu_model_core_clock_t) == 48, "core clock wire ABI");
_Static_assert(sizeof(npu_model_profile_event_t) == 128, "profile event wire ABI");
_Static_assert(sizeof(npu_model_profile_t) == 8248, "profile wire ABI");

#define MODEL_SLOTS 4u
struct model {
    uint64_t token;
#ifndef Q8B_DSP_LOADER_ONLY
    const QNN_INTERFACE_VER_TYPE *api;
    const QNN_SYSTEM_INTERFACE_VER_TYPE *sys;
    Qnn_BackendHandle_t backend;
    Qnn_DeviceHandle_t device;
    Qnn_ContextHandle_t context;
    QnnSystemContext_Handle_t system;
    Qnn_GraphHandle_t graph;
    Qnn_ProfileHandle_t profile;
    uint32_t profile_level, profile_valid, execution;
    const QnnHtpDevice_PerfInfrastructure_t *power_infra;
    uint32_t power_id;
    void *binary;
    uint32_t binary_bytes;
    Qnn_Tensor_t inputs[NPU_MODEL_MAX_TENSORS], outputs[NPU_MODEL_MAX_TENSORS];
    void *input_base[NPU_MODEL_MAX_TENSORS], *output_base[NPU_MODEL_MAX_TENSORS];
    npu_model_info_t metadata;
#endif
};
static struct model *models[MODEL_SLOTS];
static uint64_t next_token;

static struct model *find_model(uint64_t token) {
    for (unsigned i=0;i<MODEL_SLOTS;++i)
        if (models[i] && models[i]->token == token) return models[i];
    return NULL;
}
int q8b_model_open(uint64_t *handle) {
    for (unsigned i=0;i<MODEL_SLOTS;++i) if (!models[i]) {
        struct model *m = malloc(sizeof(*m));
        if (!m) return 2;
        *m = (struct model){0};
        if (!++next_token) ++next_token;
        m->token = next_token;
        models[i] = m;
        *handle = m->token;
        return 0;
    }
    return 2;
}

#ifndef Q8B_DSP_LOADER_ONLY
static void init_info(npu_sdk_dsp_info_t *info, uint32_t stage) {
    *info = (npu_sdk_dsp_info_t){.magic=NPU_SDK_DSP_INFO_MAGIC, .stage=stage,
        .backend_id=QNN_BACKEND_ID_HTP, .major=QNN_API_VERSION_MAJOR,
        .minor=QNN_API_VERSION_MINOR, .patch=QNN_API_VERSION_PATCH};
}
static int copy_name(char *out, size_t capacity, const char *name) {
    if (!name) return -1;
    size_t i=0;
    for (;i<capacity;++i) { out[i]=name[i]; if (!name[i]) return 0; }
    return -1;
}

/* On a failed free, retain the handle and its referenced memory for retry or
 * process/PAS teardown. Never free the serialized context while it is live. */
static void unload(struct model *m, npu_sdk_dsp_info_t *out) {
    init_info(out, 30);
    if (m->context) {
        out->stage=31; out->status=m->api->contextFree(m->context, NULL);
        if (out->status) return;
        m->context=NULL; m->graph=NULL;
        m->profile_valid=0;
    }
    if (m->profile) {
        out->stage=36; out->status=m->api->profileFree(m->profile);
        if (out->status) return;
        m->profile=NULL; m->profile_level=0;
    }
    if (m->power_id) {
        out->stage=37; out->status=m->power_infra->destroyPowerConfigId(m->power_id);
        if (out->status) return;
        m->power_id=0; m->power_infra=NULL;
    }
    m->power_infra=NULL;
    if (m->device) {
        out->stage=32; out->status=m->api->deviceFree(m->device);
        if (out->status) return;
        m->device=NULL;
    }
    if (m->backend) {
        out->stage=33; out->status=m->api->backendFree(m->backend);
        if (out->status) return;
        m->backend=NULL;
    }
    if (m->system) {
        out->stage=34; out->status=m->sys->systemContextFree(m->system);
        if (out->status) return;
        m->system=NULL;
    }
    for (unsigned i=0;i<NPU_MODEL_MAX_TENSORS;++i) {
        free(m->input_base[i]); m->input_base[i]=NULL;
        free(m->output_base[i]); m->output_base[i]=NULL;
    }
    free(m->binary); m->binary=NULL; m->binary_bytes=0;
    m->metadata=(npu_model_info_t){0};
    out->stage=35;
}

static Qnn_ErrorHandle_t interfaces(struct model *m) {
    const QnnInterface_t **providers=NULL;
    uint32_t count=0;
    Qnn_ErrorHandle_t rc=QnnInterface_getProviders(&providers, &count);
    if (rc) return rc;
    for (uint32_t i=0;providers && i<count;++i) {
        const QnnInterface_t *p=providers[i];
        if (p && p->backendId==QNN_BACKEND_ID_HTP &&
                p->apiVersion.coreApiVersion.major==QNN_API_VERSION_MAJOR &&
                p->apiVersion.coreApiVersion.minor>=QNN_API_VERSION_MINOR) {
            m->api=&p->QNN_INTERFACE_VER_NAME; break;
        }
    }
    const QnnSystemInterface_t **systems=NULL;
    rc=QnnSystemInterface_getProviders(&systems, &count);
    if (rc) return rc;
    for (uint32_t i=0;systems && i<count;++i) {
        const QnnSystemInterface_t *p=systems[i];
        if (p && p->systemApiVersion.major==QNN_SYSTEM_API_VERSION_MAJOR &&
                p->systemApiVersion.minor>=QNN_SYSTEM_API_VERSION_MINOR) {
            m->sys=&p->QNN_SYSTEM_INTERFACE_VER_NAME; break;
        }
    }
    if (!m->api || !m->sys || !m->api->contextCreateFromBinary || !m->api->graphRetrieve ||
            !m->api->graphExecute || !m->api->backendCreate || !m->api->backendFree ||
            !m->api->deviceCreate || !m->api->deviceFree || !m->api->contextFree ||
            !m->sys->systemContextCreate || !m->sys->systemContextGetBinaryInfo ||
            !m->sys->systemContextFree || !m->sys->systemTensorGetMemoryFootprint)
        return QNN_COMMON_ERROR_NOT_SUPPORTED;
    return 0;
}

#define FIELD(t, field) ((t)->version==QNN_TENSOR_VERSION_1 ? (t)->v1.field : (t)->v2.field)
static void *tensor_data(Qnn_Tensor_t *t) { return FIELD(t, clientBuf.data); }
static uint64_t prepare_tensor(struct model *m, const Qnn_Tensor_t *source, Qnn_Tensor_t *target,
        npu_model_tensor_t *desc, void **base, uint32_t *total) {
    if (source->version!=QNN_TENSOR_VERSION_1 && source->version!=QNN_TENSOR_VERSION_2)
        return NPU_MODEL_ERROR_METADATA;
    uint32_t rank=FIELD(source,rank);
    if (rank>NPU_MODEL_MAX_RANK || (rank && !FIELD(source,dimensions)) ||
            FIELD(source,dataFormat)!=QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER)
        return NPU_MODEL_ERROR_LIMIT;
    if (source->version==QNN_TENSOR_VERSION_2 && source->v2.isDynamicDimensions)
        for (uint32_t i=0;i<rank;++i) if (source->v2.isDynamicDimensions[i])
            return QNN_COMMON_ERROR_NOT_SUPPORTED;
    for (uint32_t i=0;i<rank;++i) {
        uint32_t n=FIELD(source,dimensions)[i];
        if (!n) return NPU_MODEL_ERROR_METADATA;
        desc->dimensions[i]=n;
    }
    if (copy_name(desc->name,sizeof(desc->name),FIELD(source,name))) return NPU_MODEL_ERROR_LIMIT;
    uint64_t bytes=0;
    Qnn_ErrorHandle_t rc=m->sys->systemTensorGetMemoryFootprint(*source,&bytes);
    if (rc) return rc;
    if (!bytes || bytes>NPU_MODEL_MAX_BYTES || *total>NPU_MODEL_MAX_BYTES-bytes)
        return NPU_MODEL_ERROR_LIMIT;
    void *allocation=malloc((size_t)bytes+127);
    if (!allocation) return QNN_COMMON_ERROR_MEM_ALLOC;
    *base=allocation;
    void *data=(void *)(((uintptr_t)allocation+127)&~(uintptr_t)127);
    *target=*source;
    Qnn_ClientBuffer_t buffer={data,(uint32_t)bytes};
    if (target->version==QNN_TENSOR_VERSION_1) {
        target->v1.memType=QNN_TENSORMEMTYPE_RAW; target->v1.clientBuf=buffer;
    } else {
        target->v2.memType=QNN_TENSORMEMTYPE_RAW; target->v2.clientBuf=buffer;
    }
    desc->id=FIELD(source,id); desc->data_type=FIELD(source,dataType); desc->rank=rank;
    desc->bytes=(uint32_t)bytes; desc->offset=*total;
    Qnn_QuantizeParams_t q=FIELD(source,quantizeParams);
    desc->quant_encoding=q.quantizationEncoding;
    if (q.quantizationEncoding==QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
        desc->scale=q.scaleOffsetEncoding.scale; desc->quant_offset=q.scaleOffsetEncoding.offset;
    }
    *total+=(uint32_t)bytes;
    return 0;
}

static void load_model(struct model *m, void *binary, uint32_t binary_size,
        const char *requested, npu_model_info_t *reply) {
    *reply=(npu_model_info_t){.abi=NPU_MODEL_ABI};
    npu_sdk_dsp_info_t *out=&reply->info;
    init_info(out,20);
    if (m->binary || m->context || m->system || m->backend || m->device || m->profile || m->power_id) {
        out->status=NPU_MODEL_ERROR_STATE; return;
    }
    out->status=interfaces(m);
    if (out->status) return;
    m->metadata=(npu_model_info_t){.abi=NPU_MODEL_ABI};
    m->binary=malloc(binary_size);
    if (!m->binary) { out->status=QNN_COMMON_ERROR_MEM_ALLOC; return; }
    m->binary_bytes=binary_size;
    memcpy(m->binary,binary,binary_size);
    out->stage=21; out->status=m->sys->systemContextCreate(&m->system);
    if (out->status) goto fail;
    const QnnSystemContext_BinaryInfo_t *info=NULL;
    Qnn_ContextBinarySize_t info_size=0;
    out->stage=22;
    out->status=m->sys->systemContextGetBinaryInfo(m->system,m->binary,binary_size,&info,&info_size);
    if (out->status) goto fail;
    uint32_t count=0, backend=0;
    QnnSystemContext_GraphInfo_t *graphs=NULL;
    if (!info) { out->status=NPU_MODEL_ERROR_METADATA; goto fail; }
    switch (info->version) {
#define BINARY_CASE(v) case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_##v: \
        count=info->contextBinaryInfoV##v.numGraphs; graphs=info->contextBinaryInfoV##v.graphs; \
        backend=info->contextBinaryInfoV##v.backendId; break
        BINARY_CASE(1); BINARY_CASE(2); BINARY_CASE(3);
#undef BINARY_CASE
    default: out->status=NPU_MODEL_ERROR_METADATA; goto fail;
    }
    if (!graphs || !count || count>1024 || backend!=QNN_BACKEND_ID_HTP) {
        out->status=NPU_MODEL_ERROR_METADATA; goto fail;
    }
    out->stage=23;
    const char *name=NULL;
    Qnn_Tensor_t *inputs=NULL,*outputs=NULL;
    for (uint32_t i=0;i<count;++i) {
        uint32_t ni=0,no=0;
        const char *candidate=NULL;
        Qnn_Tensor_t *in=NULL,*on=NULL;
        switch (graphs[i].version) {
#define GRAPH_CASE(v) case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_##v: \
            candidate=graphs[i].graphInfoV##v.graphName; ni=graphs[i].graphInfoV##v.numGraphInputs; \
            no=graphs[i].graphInfoV##v.numGraphOutputs; in=graphs[i].graphInfoV##v.graphInputs; \
            on=graphs[i].graphInfoV##v.graphOutputs; break
            GRAPH_CASE(1); GRAPH_CASE(2); GRAPH_CASE(3);
#undef GRAPH_CASE
        default: out->status=NPU_MODEL_ERROR_METADATA; goto fail;
        }
        if (candidate && ((!requested[0] && count==1) || !strcmp(requested,candidate))) {
            if (name) { out->status=NPU_MODEL_ERROR_METADATA; goto fail; }
            name=candidate; inputs=in; outputs=on;
            m->metadata.num_inputs=ni; m->metadata.num_outputs=no;
        }
    }
    if (!name || !inputs || !outputs) { out->status=NPU_MODEL_ERROR_METADATA; goto fail; }
    if (!m->metadata.num_inputs || !m->metadata.num_outputs ||
            m->metadata.num_inputs>NPU_MODEL_MAX_TENSORS || m->metadata.num_outputs>NPU_MODEL_MAX_TENSORS ||
            copy_name(m->metadata.graph_name,sizeof(m->metadata.graph_name),name)) {
        out->status=NPU_MODEL_ERROR_LIMIT; goto fail;
    }
    out->stage=24;
    for (uint32_t i=0;i<m->metadata.num_inputs;++i) {
        out->status=prepare_tensor(m,&inputs[i],&m->inputs[i],&m->metadata.inputs[i],
                &m->input_base[i],&m->metadata.input_bytes);
        if (out->status) goto fail;
    }
    for (uint32_t i=0;i<m->metadata.num_outputs;++i) {
        out->status=prepare_tensor(m,&outputs[i],&m->outputs[i],&m->metadata.outputs[i],
                &m->output_base[i],&m->metadata.output_bytes);
        if (out->status) goto fail;
    }
    out->stage=25; out->status=m->api->backendCreate(NULL,NULL,&m->backend);
    if (out->status) goto fail;
    out->stage=26; out->status=m->api->deviceCreate(NULL,NULL,&m->device);
    if (out->status) goto fail;
    out->stage=27;
    out->status=m->api->contextCreateFromBinary(m->backend,m->device,NULL,m->binary,binary_size,&m->context,NULL);
    if (out->status) goto fail;
    out->stage=28; out->status=m->api->graphRetrieve(m->context,name,&m->graph);
    if (out->status) goto fail;
    out->stage=29;
    m->metadata.info=*out;
    *reply=m->metadata;
    return;
fail: {
    npu_sdk_dsp_info_t cleanup;
    unload(m,&cleanup); /* preserve the original error; failed cleanup retains state */
    }
}

static void execute(struct model *m, const void *input, void *output, npu_model_execution_t *result) {
    *result=(npu_model_execution_t){0};
    init_info(&result->info,40);
    m->profile_valid=0;
    for (uint32_t i=0;i<m->metadata.num_inputs;++i) {
        npu_model_tensor_t *t=&m->metadata.inputs[i];
        memcpy(tensor_data(&m->inputs[i]),(const uint8_t *)input+t->offset,t->bytes);
    }
    for (uint32_t i=0;i<m->metadata.num_outputs;++i)
        memset(tensor_data(&m->outputs[i]),0xa5,m->metadata.outputs[i].bytes);
    uint64_t begin=HAP_perf_get_time_us();
    result->info.status=m->api->graphExecute(m->graph,m->inputs,m->metadata.num_inputs,
            m->outputs,m->metadata.num_outputs,m->profile,NULL);
    result->execute_us=HAP_perf_get_time_us()-begin;
    if (result->info.status) return;
    if (!++m->execution) ++m->execution;
    m->profile_valid=m->profile!=NULL;
    for (uint32_t i=0;i<m->metadata.num_outputs;++i) {
        npu_model_tensor_t *t=&m->metadata.outputs[i];
        memcpy((uint8_t *)output+t->offset,tensor_data(&m->outputs[i]),t->bytes);
    }
    result->executions=1; result->info.stage=41;
}

static void profile_set(struct model *m, uint32_t level, npu_sdk_dsp_info_t *out) {
    init_info(out,50);
    if (!m->context || !m->backend) { out->status=NPU_MODEL_ERROR_STATE; return; }
    if (level>QNN_PROFILE_LEVEL_DETAILED) { out->status=QNN_COMMON_ERROR_INVALID_ARGUMENT; return; }
    if (!m->api->profileCreate || !m->api->profileFree || !m->api->profileGetEvents ||
            !m->api->profileGetSubEvents || !m->api->profileGetEventData) {
        out->status=QNN_COMMON_ERROR_NOT_SUPPORTED; return;
    }
    m->profile_valid=0;
    if (m->profile) {
        out->stage=51; out->status=m->api->profileFree(m->profile);
        if (out->status) return;
        m->profile=NULL; m->profile_level=0;
    }
    if (level) {
        out->stage=52; out->status=m->api->profileCreate(m->backend,level,&m->profile);
        if (out->status) return;
        if (!m->profile) { out->status=NPU_MODEL_ERROR_METADATA; return; }
        m->profile_level=level;
    }
    out->stage=53;
}

static uint64_t append_events(struct model *m, npu_model_profile_t *out,
        const QnnProfile_EventId_t *ids, uint32_t count, uint32_t parent) {
    if (count && !ids) return NPU_MODEL_ERROR_METADATA;
    for (uint32_t i=0;i<count;++i) {
        if (out->count==NPU_MODEL_MAX_PROFILE_EVENTS) {
            out->flags|=NPU_MODEL_PROFILE_TRUNCATED_EVENTS; break;
        }
        /* Reject cycles, without recursive traversal on the small DSP stack. */
        for (uint32_t p=parent;p!=NPU_MODEL_PROFILE_ROOT;p=out->events[p].parent)
            if (out->events[p].id==ids[i]) return NPU_MODEL_ERROR_METADATA;
        QnnProfile_EventData_t data=QNN_PROFILE_EVENT_DATA_INIT;
        uint64_t rc=m->api->profileGetEventData(ids[i],&data);
        if (rc) return rc;
        npu_model_profile_event_t *event=&out->events[out->count++];
        event->id=ids[i]; event->value=data.value; event->type=data.type; event->unit=data.unit;
        event->parent=parent;
        event->depth=parent==NPU_MODEL_PROFILE_ROOT ? 0 : out->events[parent].depth+1;
        if (data.identifier) {
            uint32_t j=0;
            while (j<sizeof(event->identifier)-1 && data.identifier[j]) {
                event->identifier[j]=data.identifier[j]; ++j;
            }
            if (data.identifier[j]) out->flags|=NPU_MODEL_PROFILE_TRUNCATED_NAME;
        }
    }
    return 0;
}

static void profile_get(struct model *m, npu_model_profile_t *out) {
    memset(out,0,sizeof(*out));
    out->abi=NPU_MODEL_PROFILE_ABI; init_info(&out->info,54);
    if (!m->context || !m->profile || !m->profile_valid) {
        out->info.status=NPU_MODEL_ERROR_STATE; return;
    }
    out->level=m->profile_level; out->execution=m->execution;
    const QnnProfile_EventId_t *ids=NULL;
    uint32_t count=0;
    out->info.status=m->api->profileGetEvents(m->profile,&ids,&count);
    if (out->info.status) return;
    out->info.status=append_events(m,out,ids,count,NPU_MODEL_PROFILE_ROOT);
    if (out->info.status) return;
    for (uint32_t i=0;i<out->count;++i) {
        out->info.status=m->api->profileGetSubEvents(out->events[i].id,&ids,&count);
        if (out->info.status) return;
        out->info.status=append_events(m,out,ids,count,i);
        if (out->info.status) return;
    }
    out->info.stage=55;
}

static void power_set(struct model *m, uint32_t mode, npu_sdk_dsp_info_t *out) {
    init_info(out,60);
    if (!m->context || !m->backend) { out->status=NPU_MODEL_ERROR_STATE; return; }
    if (mode>NPU_MODEL_POWER_TURBO) { out->status=QNN_COMMON_ERROR_INVALID_ARGUMENT; return; }
    m->profile_valid=0;
    if (!mode) {
        if (m->power_id) {
            out->stage=64; out->status=m->power_infra->destroyPowerConfigId(m->power_id);
            if (out->status) return;
            m->power_id=0; m->power_infra=NULL;
        }
        m->power_infra=NULL; out->stage=63; return;
    }
    if (!m->power_infra) {
        if (!m->api->deviceGetInfrastructure) { out->status=QNN_COMMON_ERROR_NOT_SUPPORTED; return; }
        QnnDevice_Infrastructure_t infra=NULL;
        out->stage=61; out->status=m->api->deviceGetInfrastructure(&infra);
        if (out->status) return;
        if (!infra || infra->infraType!=QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF ||
                !infra->perfInfra.createPowerConfigId || !infra->perfInfra.destroyPowerConfigId ||
                !infra->perfInfra.setPowerConfig) { out->status=QNN_COMMON_ERROR_NOT_SUPPORTED; return; }
        m->power_infra=&infra->perfInfra;
    }
    if (!m->power_id) {
        out->stage=62; out->status=m->power_infra->createPowerConfigId(0,0,&m->power_id);
        if (out->status) return;
        /* Client zero overrides process-wide votes. Never use it. */
        if (!m->power_id) { out->status=NPU_MODEL_ERROR_METADATA; return; }
    }
    QnnHtpPerfInfrastructure_VoltageCorner_t corner=mode==NPU_MODEL_POWER_TURBO ?
        DCVS_VOLTAGE_VCORNER_TURBO : DCVS_VOLTAGE_VCORNER_NOM;
    QnnHtpPerfInfrastructure_PowerConfig_t config=QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIG_INIT;
    config.option=QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
    config.dcvsV3Config=(QnnHtpPerfInfrastructure_DcvsV3_t){
        .contextId=m->power_id,.setDcvsEnable=1,.dcvsEnable=0,
        .powerMode=QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE,
        .setSleepLatency=1,.sleepLatency=100,.setSleepDisable=1,.sleepDisable=1,
        .setBusParams=1,.busVoltageCornerMin=corner,.busVoltageCornerTarget=corner,.busVoltageCornerMax=corner,
        .setCoreParams=1,.coreVoltageCornerMin=corner,.coreVoltageCornerTarget=corner,.coreVoltageCornerMax=corner};
    const QnnHtpPerfInfrastructure_PowerConfig_t *configs[]={&config,NULL};
    out->stage=65; out->status=m->power_infra->setPowerConfig(m->power_id,configs);
    if (!out->status) out->stage=63;
}
#endif

int q8b_model_close(uint64_t handle) {
    struct model *m=find_model(handle);
    if (!m) return 14;
#ifndef Q8B_DSP_LOADER_ONLY
    npu_sdk_dsp_info_t info;
    unload(m,&info);
    if (info.status) return 1;
#endif
    for (unsigned i=0;i<MODEL_SLOTS;++i) if (models[i]==m) models[i]=NULL;
    free(m);
    return 0;
}

int q8b_model_invoke(uint64_t handle, uint32_t sc, remote_arg *args) {
    struct model *m=find_model(handle);
    if (!m || !args) return 14;
#ifdef Q8B_DSP_LOADER_ONLY
    (void)sc; return 20;
#else
    if (sc==0x0e000100u) {
        if (!args[0].buf.data || args[0].buf.size!=sizeof(npu_model_core_clock_t)) return 14;
        npu_model_core_clock_t *out=args[0].buf.data;
        *out=(npu_model_core_clock_t){.abi=NPU_MODEL_CORE_CLOCK_ABI};
        init_info(&out->info,80);
        if (!m->context || !m->graph) { out->info.status=NPU_MODEL_ERROR_STATE; return 0; }
        if (!HAP_power_get) { out->info.status=QNN_COMMON_ERROR_NOT_SUPPORTED; return 0; }
        struct hap_clock_response response={.type=4};
        int status=HAP_power_get(NULL,&response);
        if (status) { out->info.status=(uint32_t)status; return 0; }
        if (response.type!=4 || !response.value || response.value>UINT32_MAX) {
            out->info.status=NPU_MODEL_ERROR_METADATA; return 0;
        }
        out->core_hz=(uint32_t)response.value;
        out->sampled_us=HAP_perf_get_time_us();
        out->info.stage=81;
        return 0;
    }
    if (sc==0x07020100u) {
        if (!args[0].buf.data || !args[0].buf.size || args[0].buf.size>NPU_MODEL_MAX_BYTES ||
                !args[1].buf.data || !args[1].buf.size || args[1].buf.size>NPU_MODEL_NAME_BYTES ||
                ((char *)args[1].buf.data)[args[1].buf.size-1] ||
                !args[2].buf.data || args[2].buf.size!=sizeof(npu_model_info_t)) return 14;
        load_model(m,args[0].buf.data,args[0].buf.size,args[1].buf.data,args[2].buf.data);
        return 0;
    }
    if (sc==0x08010200u) {
        if (!args[2].buf.data || args[2].buf.size!=sizeof(npu_model_execution_t)) return 14;
        npu_model_execution_t *out=args[2].buf.data;
        *out=(npu_model_execution_t){0}; init_info(&out->info,40);
        m->profile_valid=0;
        if (!m->context || !m->graph) { out->info.status=NPU_MODEL_ERROR_STATE; return 0; }
        if (!args[0].buf.data || args[0].buf.size!=m->metadata.input_bytes ||
                !args[1].buf.data || args[1].buf.size!=m->metadata.output_bytes) return 14;
        execute(m,args[0].buf.data,args[1].buf.data,out);
        return 0;
    }
    if (sc==0x0d020200u) {
        m->profile_valid=0;
        if (!args[0].buf.data || args[0].buf.size!=sizeof(uint32_t) ||
                !args[3].buf.data || args[3].buf.size!=sizeof(npu_model_batch_execution_t)) return 14;
        uint32_t count; memcpy(&count,args[0].buf.data,sizeof(count));
        if (!count || count>NPU_MODEL_MAX_BATCH || !args[1].buf.data || !args[2].buf.data ||
                args[1].buf.size>NPU_MODEL_MAX_BYTES || args[2].buf.size>NPU_MODEL_MAX_BYTES) return 14;
        npu_model_batch_execution_t *out=args[3].buf.data;
        *out=(npu_model_batch_execution_t){.abi=NPU_MODEL_BATCH_ABI,.requested=count};
        init_info(&out->info,71);
        if (!m->context || !m->graph) { out->info.status=NPU_MODEL_ERROR_STATE; return 0; }
        /* Validate all byte products before executing any sample. */
        if (args[1].buf.size!=(uint64_t)count*m->metadata.input_bytes ||
                args[2].buf.size!=(uint64_t)count*m->metadata.output_bytes) return 14;
        for (uint32_t i=0;i<count;++i) {
            npu_model_execution_t one;
            execute(m,(const uint8_t *)args[1].buf.data+i*m->metadata.input_bytes,
                    (uint8_t *)args[2].buf.data+i*m->metadata.output_bytes,&one);
            if (one.info.status) { out->info.status=one.info.status; return 0; }
            out->executions++;
            out->execute_total_us+=one.execute_us;
            if (!i || one.execute_us<out->execute_min_us) out->execute_min_us=one.execute_us;
            if (one.execute_us>out->execute_max_us) out->execute_max_us=one.execute_us;
        }
        out->info.stage=72;
        return 0;
    }
    if (sc==0x09000100u) {
        if (!args[0].buf.data || args[0].buf.size!=sizeof(npu_sdk_dsp_info_t)) return 14;
        unload(m,args[0].buf.data);
        return 0;
    }
    if (sc==0x0a010100u) {
        if (!args[0].buf.data || args[0].buf.size!=sizeof(uint32_t) ||
                !args[1].buf.data || args[1].buf.size!=sizeof(npu_sdk_dsp_info_t)) return 14;
        uint32_t level; memcpy(&level,args[0].buf.data,sizeof(level));
        profile_set(m,level,args[1].buf.data);
        return 0;
    }
    if (sc==0x0b000100u) {
        if (!args[0].buf.data || args[0].buf.size!=sizeof(npu_model_profile_t)) return 14;
        profile_get(m,args[0].buf.data);
        return 0;
    }
    if (sc==0x0c010100u) {
        if (!args[0].buf.data || args[0].buf.size!=sizeof(uint32_t) ||
                !args[1].buf.data || args[1].buf.size!=sizeof(npu_sdk_dsp_info_t)) return 14;
        uint32_t mode; memcpy(&mode,args[0].buf.data,sizeof(mode));
        power_set(m,mode,args[1].buf.data);
        return 0;
    }
    return 14;
#endif
}
