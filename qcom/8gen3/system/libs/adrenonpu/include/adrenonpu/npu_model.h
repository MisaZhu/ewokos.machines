/* Native official QNN context sessions. No CPU fallback; wire values are
 * independent of host pointers and of the experimental npu_htp graph ABI. */
#ifndef ADRENONPU_NPU_MODEL_H
#define ADRENONPU_NPU_MODEL_H
#include <adrenonpu/npu_sdk.h>

#define NPU_MODEL_ABI 1u
#define NPU_MODEL_MAX_TENSORS 4u
#define NPU_MODEL_MAX_RANK 8u
#define NPU_MODEL_NAME_BYTES 96u
#define NPU_MODEL_MAX_BYTES (4u * 1024u * 1024u)
#define NPU_MODEL_ERROR_STATE 0x80010001ull
#define NPU_MODEL_ERROR_METADATA 0x80010002ull
#define NPU_MODEL_ERROR_LIMIT 0x80010003ull
#define NPU_MODEL_PROFILE_ABI 1u
#define NPU_MODEL_MAX_PROFILE_EVENTS 64u
#define NPU_MODEL_PROFILE_ROOT 0xffffffffu
#define NPU_MODEL_PROFILE_TRUNCATED_EVENTS 1u
#define NPU_MODEL_PROFILE_TRUNCATED_NAME 2u
#define NPU_MODEL_PROFILE_BASIC 1u
#define NPU_MODEL_PROFILE_DETAILED 2u
#define NPU_MODEL_POWER_DEFAULT 0u
#define NPU_MODEL_POWER_NOMINAL 1u
#define NPU_MODEL_POWER_TURBO 2u
#define NPU_MODEL_BATCH_ABI 1u
#define NPU_MODEL_MAX_BATCH 1000u
#define NPU_MODEL_CORE_CLOCK_ABI 1u

typedef struct {
    uint32_t id, data_type, rank, dimensions[NPU_MODEL_MAX_RANK];
    uint32_t bytes, offset, quant_encoding;
    float scale;
    int32_t quant_offset;
    char name[64];
} npu_model_tensor_t;

typedef struct {
    npu_sdk_dsp_info_t info;
    uint32_t abi, num_inputs, num_outputs, input_bytes, output_bytes, reserved;
    char graph_name[NPU_MODEL_NAME_BYTES];
    npu_model_tensor_t inputs[NPU_MODEL_MAX_TENSORS];
    npu_model_tensor_t outputs[NPU_MODEL_MAX_TENSORS];
} npu_model_info_t;

typedef struct {
    npu_sdk_dsp_info_t info;
    uint32_t executions, reserved;
    uint64_t execute_us;
} npu_model_execution_t;

/* Sequential graphExecute calls for distinct concatenated samples in one RPC.
 * Timers sum only graphExecute, not copies/RPC. On QNN failure, executions is
 * the successful prefix; callers must reject the whole output buffer. */
typedef struct {
    npu_sdk_dsp_info_t info;
    uint32_t abi, requested, executions, reserved;
    uint64_t execute_total_us, execute_min_us, execute_max_us;
} npu_model_batch_execution_t;

/* Instantaneous DSP CPU core clock from HAP_power_get(type=4), in Hz.
 * Timestamp is the DSP HAP microsecond clock, taken after the getter returns.
 * This does not measure HMX frequency, power, or an execution average. */
typedef struct {
    npu_sdk_dsp_info_t info;
    uint32_t abi, core_hz;
    uint64_t sampled_us;
} npu_model_core_clock_t;

/* Pointer-free snapshot of official QnnProfile events for the last successful
 * execute. Units/types are the original QNN values; no cycle conversion.
 * parent is an earlier event index, or PROFILE_ROOT. */
typedef struct {
    uint64_t id, value;
    uint32_t parent, type, unit, depth;
    char identifier[96];
} npu_model_profile_event_t;
typedef struct {
    npu_sdk_dsp_info_t info;
    uint32_t abi, level, count, flags, execution, reserved;
    npu_model_profile_event_t events[NPU_MODEL_MAX_PROFILE_EVENTS];
} npu_model_profile_t;

/* One context per native module handle; graph_name="" selects a single graph.
 * A module can be unloaded/reloaded. Load during an active context fails.
 * Tensor files concatenate tensors at the byte offsets returned in metadata.
 * Raw representations and quantization come from the official context. */
int npu_model_load(uint32_t session, uint32_t handle, const void *binary,
        uint32_t binary_size, const char *graph_name, npu_model_info_t *info);
int npu_model_execute(uint32_t session, uint32_t handle,
        const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
        npu_model_execution_t *execution);
int npu_model_execute_batch(uint32_t session, uint32_t handle, uint32_t count,
        const void *input, uint32_t input_bytes, void *output, uint32_t output_bytes,
        npu_model_batch_execution_t *execution);
int npu_model_unload(uint32_t session, uint32_t handle, npu_sdk_dsp_info_t *info);
/* level=0 disables; 1=basic, 2=detailed. Requires a loaded context.
 * Unsupported or failed requests return an error, never fabricated metrics.
 * Enabling/changing the level invalidates any prior snapshot. */
int npu_model_profile_set(uint32_t session, uint32_t handle, uint32_t level,
        npu_sdk_dsp_info_t *info);
int npu_model_profile_get(uint32_t session, uint32_t handle, npu_model_profile_t *profile);
/* Optional official HTP PerfInfrastructure request using an owned nonzero
 * power client. DEFAULT destroys that client. Corners are requests, not
 * measured frequencies; the firmware/platform may reject them. */
int npu_model_power_set(uint32_t session, uint32_t handle, uint32_t mode,
        npu_sdk_dsp_info_t *info);
/* Optional firmware getter; requires a loaded context. Unsupported getters
 * and failed/zero readbacks return an error status, never a nominal estimate. */
int npu_model_core_clock_get(uint32_t session, uint32_t handle,
        npu_model_core_clock_t *clock);
#endif
