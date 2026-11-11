/*
 * qnn_core_internal.h - internal object model behind the QNN C API declared
 * in <adrenonpu/qnn_core.h>.
 *
 * Handles are the raw pointer values of heap objects cast to uint64_t; every
 * object starts with a (magic, type) pair so a stale or foreign handle is
 * rejected instead of dereferenced.  This mirrors how the vendor libQnnHtp.so
 * keeps opaque handles while staying O(1) on validation.
 *
 * Object graph: one qnn_backend_obj_t owns the NPU session state; devices,
 * contexts and profiles reference it.  A context owns its graphs and the
 * shared tensor pool (QnnTensor_createContextTensor); graph tensors created
 * through QnnTensor_createGraphTensor join the same pool and are additionally
 * tracked on the graph's input/output lists, mirroring the QNN v2 model where
 * every tensor is context-scoped and graphs merely reference them.
 *
 * Execution is dual-path, mirroring the real HTP backend:
 *   1. npu_htp.c dispatches the finalized graph to the cDSP over the
 *      reconstructed FastRPC transport (npu_fastrpc.c) after the cDSP PAS
 *      boot (npu_cdsp.c).
 *   2. npu_ref_exec.c is the in-process CPU reference executor (the role the
 *      vendor SDK's libQnnCpu.so plays); it runs whenever the cDSP transport
 *      is unavailable, so the API stays exercisable off silicon.
 */
#ifndef QNN_CORE_INTERNAL_H
#define QNN_CORE_INTERNAL_H

#include <adrenonpu/qnn_core.h>
#include <adrenonpu/qnn_htp_ext.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "qnn_ops.h"
#include "npu_fastrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QNN_OBJ_MAGIC   0x514E4E30u   /* "QNN0" */

enum {
	QNN_OBJ_BACKEND = 1,
	QNN_OBJ_DEVICE,
	QNN_OBJ_CONTEXT,
	QNN_OBJ_GRAPH,
	QNN_OBJ_TENSOR,
	QNN_OBJ_SIGNAL,
	QNN_OBJ_PROFILE,
	QNN_OBJ_MEMREG
};

typedef struct {
	uint32_t magic;
	uint32_t type;
} qnn_obj_head_t;

/* Per-backend string slots for registered op packages. */
#define QNN_MAX_OP_PACKAGES 4
/* Static key/value pairs returned by QnnDevice_getPlatformInfo. */
#define QNN_PLATFORM_INFO_MAX 12

/* --- HTP private tuning state (set through qnn_htp_ext.h) --- */
typedef struct {
	QnnHtp_PerfMode_t      perf_mode;      /* default QNN_HTP_PERF_MODE_BURST */
	uint64_t               vtcm_limit;     /* 0 = backend default             */
	uint32_t               concurrent_group;
	uint32_t               dsp_core_mask;  /* device level                    */
	QnnHtp_PrecisionHint_t precision;      /* graph level                     */
	bool                   skip_flush;     /* graph level                     */
	bool                   hw_counters;    /* profile level                   */
} qnn_htp_state_t;

typedef struct qnn_shared_buf {
	struct qnn_shared_buf *next;
	int      fd;
	uint64_t offset;
	uint64_t size;
	void    *host;       /* exposed pointer at offset within allocation  */
	void    *alloc_host; /* page-aligned DMA allocation base             */
	uint64_t pa;         /* allocation-base PA, cDSP SMMU map target     */
	uint32_t map_size;   /* rounded DMA mapping/allocation extent        */
	uint64_t devaddr;    /* consolidated cDSP address, when mapped      */
	uint32_t map_session;/* session owning devaddr                       */
	unsigned map_epoch; /* successful CLOSE/STOP retires this reference */
	uint32_t refcount;   /* shared across contexts, like an ion dma-buf */
} qnn_shared_buf_t;

typedef struct qnn_backend_obj {
	qnn_obj_head_t head;
	char    *config_keys[8];
	char    *config_vals[8];
	uint32_t num_config;
	char    *op_packages[QNN_MAX_OP_PACKAGES];
	uint32_t num_op_packages;
	/* NPU session state (probed once per backend create). */
	int      npu_available;      /* cDSP session + compatible graph ABI */
	int      cdsp_owned;         /* cleanup needed even after probe decline */
	int      npu_probe_rc;       /* detailed bring-up result (0 = up)    */
	uint32_t frpc_session;       /* FastRPC session id (npu_fastrpc.h)   */
	uint32_t soc_model;          /* QNN socModel, SC8280X = 37          */
	uint32_t dsp_arch;           /* QnnHtpDevice_Arch_t, V68 = 68       */
	qnn_htp_state_t htp_defaults;
	qnn_shared_buf_t *shared;    /* registered QnnMem / shared buffers   */
	bool native_mode;           /* explicit official opaque-context path */
	uint32_t native_devices;
	struct qnn_native_backend *native;
} qnn_backend_obj_t;

typedef struct qnn_device_obj {
	qnn_obj_head_t head;
	qnn_backend_obj_t *backend;
	char    *config_keys[8];
	char    *config_vals[8];
	uint32_t num_config;
	qnn_htp_state_t htp;         /* dsp_core_mask lives here */
} qnn_device_obj_t;

typedef struct qnn_graph_obj qnn_graph_obj_t;

typedef struct qnn_tensor_obj {
	qnn_obj_head_t head;
	struct qnn_context_obj *ctx;   /* every tensor is context-scoped   */
	char     *name;
	Qnn_TensorType_t ttype;
	Qnn_DataType_t   dtype;
	uint32_t rank;
	uint32_t dims[8];
	Qnn_QuantParams_t quant;
	void    *data;                 /* client buffer alias or owned     */
	uint64_t data_bytes;
	bool     data_owned;           /* free(data) on tensor free        */
	bool     is_graph_input;       /* bound at QnnGraph_execute        */
	bool     is_graph_output;
} qnn_tensor_obj_t;

typedef struct qnn_node_obj {
	char     *op_name;
	uint32_t  op_kind;             /* QNN_OP_KIND_* (qnn_ops.h)      */
	qnn_tensor_obj_t **ins;
	uint32_t  num_ins;
	qnn_tensor_obj_t **outs;
	uint32_t  num_outs;
	qnn_op_attrs_t attrs;          /* parsed from the addNode config */
} qnn_node_obj_t;

typedef struct qnn_ref_exec qnn_ref_exec_t;

struct qnn_graph_obj {
	qnn_obj_head_t head;
	struct qnn_context_obj *ctx;
	char     *name;
	qnn_node_obj_t *nodes;
	uint32_t num_nodes;
	uint32_t node_cap;
	qnn_tensor_obj_t **tensors;    /* every tensor referenced by name  */
	uint32_t num_tensors;
	uint32_t tensor_cap;
	qnn_tensor_obj_t **graph_ins;  /* APP_WRITE / READWRITE            */
	uint32_t num_graph_ins;
	qnn_tensor_obj_t **graph_outs; /* APP_READ / READWRITE             */
	uint32_t num_graph_outs;
	qnn_htp_state_t htp;           /* inherited from the context       */
	uint64_t htp_prepared;         /* cDSP-side prepared-graph handle  */
	bool     finalized;
	bool     from_binary;
	qnn_ref_exec_t *ref;           /* CPU plan + intermediate buffers  */
	bool native_graph;            /* official context: never CPU fallback */
};

struct qnn_context_obj {
	qnn_obj_head_t head;
	qnn_backend_obj_t *backend;
	qnn_device_obj_t  *device;     /* may be NULL                      */
	char    *config_keys[8];
	char    *config_vals[8];
	uint32_t num_config;
	qnn_graph_obj_t **graphs;
	uint32_t num_graphs;
	uint32_t graph_cap;
	qnn_tensor_obj_t **tensors;    /* context-wide tensor pool         */
	uint32_t num_tensors;
	uint32_t tensor_cap;
	qnn_htp_state_t htp;
	bool     finalized;
	struct qnn_native_context *native;
};

typedef struct qnn_context_obj qnn_context_obj_t;

typedef struct qnn_signal_obj {
	qnn_obj_head_t head;
	volatile int state;            /* 0 pending, 1 done, 2 cancelled */
} qnn_signal_obj_t;

typedef struct {
	uint32_t idx;
	uint64_t usec;
	char     name[28];
} qnn_prof_rec_t;

typedef struct qnn_profile_obj {
	qnn_obj_head_t head;
	qnn_backend_obj_t *backend;
	uint32_t level;                /* 0 off, 1 basic, 2 detailed */
	qnn_prof_rec_t *recs;
	uint32_t num_recs;
	uint32_t rec_cap;
	bool     hw_counters;          /* QnnHtpProfile_EnableHwCounters */
	char    *config_keys[8];
	char    *config_vals[8];
	uint32_t num_config;
} qnn_profile_obj_t;

typedef struct qnn_memreg_obj {
	qnn_obj_head_t head;
	qnn_backend_obj_t *backend;
	qnn_shared_buf_t *sb;
} qnn_memreg_obj_t;

/* --- handle plumbing (qnn_handle.c) --- */
void       *qnn_obj_alloc(uint32_t type, size_t size);
void       *qnn_obj_from_handle(Qnn_Handle_t h, uint32_t type);
void        qnn_obj_free(void *obj);
char       *qnn_strdup_safe(const char *s);
int         qnn_cfg_get_int(const Qnn_ConfigParam_t *cfg, uint32_t n,
                            const char *key, int dflt);
const char *qnn_cfg_get(const Qnn_ConfigParam_t *cfg, uint32_t n,
                        const char *key);
void        qnn_cfg_free(char **keys, char **vals, uint32_t *n);
int         qnn_cfg_store(char **keys, char **vals, uint32_t *n,
                          const Qnn_ConfigParam_t *cfg, uint32_t ncfg,
                          uint32_t cap);
/* Rotating static buffers for getProperty-style const char* returns. */
const char *qnn_fmt_static(const char *fmt, ...);
/* Monotonic wall clock in microseconds (profiling). */
uint64_t    qnn_time_usec(void);
uint64_t    qnn_tensor_elements(const qnn_tensor_obj_t *t);
uint32_t    qnn_dtype_size(Qnn_DataType_t dt);

/* --- logging (qnn_log.c) --- */
enum {
	QNN_LOG_LVL_ERROR = 0,
	QNN_LOG_LVL_WARN  = 1,
	QNN_LOG_LVL_INFO  = 2,
	QNN_LOG_LVL_DEBUG = 3
};
void qnn_log(uint32_t level, const char *fmt, ...);

/* --- shared DMA buffer registration (qnn_mem.c) --- */
qnn_shared_buf_t *qnn_shared_buf_alloc(qnn_backend_obj_t *b, int fd,
                                       uint64_t offset, uint64_t size);
int  qnn_shared_buf_free(qnn_backend_obj_t *b, void *host);
qnn_shared_buf_t *qnn_shared_buf_find(qnn_backend_obj_t *b, void *host);
/* Failure retains the complete entry and DMA for retry; success also
 * handles references retired by successful transport CLOSE/secure stop. */
int qnn_shared_free_storage(qnn_shared_buf_t *sb);

/* --- signals (qnn_signal.c) --- */
void qnn_signal_mark(Qnn_SignalHandle_t sig, int state);

/* --- profiling (qnn_profile.c) --- */
void qnn_profile_record(Qnn_ProfileHandle_t prof, uint32_t idx,
                        const char *op, uint64_t usec);

/* --- context binary (de)serialization (qnn_context.c) ---
   Format "QNNB" v1: see the header comment in qnn_context.c.  Returns a
   malloc'd buffer (caller frees) or NULL; *out_size receives the length. */
uint8_t *qnn_ctx_serialize(struct qnn_context_obj *c, uint64_t *out_size);
struct qnn_context_obj *qnn_ctx_deserialize(qnn_backend_obj_t *b,
        qnn_device_obj_t *dev, const uint8_t *buf, uint64_t size);
bool qnn_ctx_binary_validate(const uint8_t *buf, uint64_t size);

/* Official opaque-context bridge. Local QNN C API, not the SDK binary ABI. */
int qnn_native_context_load(qnn_context_obj_t *c, const void *binary,
                           uint64_t size, const char *graph_name);
int qnn_native_context_release(qnn_context_obj_t *c);
int qnn_native_transport_owned(void);
int qnn_native_backend_in_use(qnn_backend_obj_t *b);
int qnn_native_backend_close(qnn_backend_obj_t *b);
int qnn_native_execute(qnn_graph_obj_t *g, Qnn_TensorHandle_t *in, uint32_t nin,
                       Qnn_TensorHandle_t *out, uint32_t nout, Qnn_ProfileHandle_t prof);
int qnn_native_binary(qnn_context_obj_t *c, uint8_t *buf, uint64_t capacity,
                      uint64_t *size);
int qnn_native_property(qnn_graph_obj_t *g, const char *key, const char **value);

/* --- graph internals (qnn_graph.c) --- */
qnn_graph_obj_t *qnn_graph_obj_create(struct qnn_context_obj *c,
                                      const char *name);
void qnn_graph_apply_tuning(qnn_graph_obj_t *g);
/* Append a fully-formed node (arrays are copied, tensors tracked). */
qnn_node_obj_t *qnn_graph_node_append(qnn_graph_obj_t *g, const char *op_name,
        uint32_t op_kind, const qnn_op_attrs_t *attrs,
        qnn_tensor_obj_t *const *ins, uint32_t num_ins,
        qnn_tensor_obj_t *const *outs, uint32_t num_outs);
/* Track a tensor on the graph and route it to the input/output boundary by
   its Qnn_TensorType_t (APP_WRITE->in, APP_READ->out, READWRITE->both). */
int  qnn_graph_add_boundary(qnn_graph_obj_t *g, qnn_tensor_obj_t *t);
int  qnn_graph_track_tensor(qnn_graph_obj_t *g, qnn_tensor_obj_t *t);
/* Free the node/tensor arrays owned by the graph (not the object itself). */
void qnn_graph_release_internals(qnn_graph_obj_t *g);

/* --- tensor internals (qnn_tensor.c) --- */
qnn_tensor_obj_t *qnn_tensor_create_common(struct qnn_context_obj *c,
        qnn_graph_obj_t *g, const char *name, Qnn_TensorType_t ttype,
        Qnn_DataType_t dtype, const Qnn_Shape_t *shape,
        const Qnn_QuantParams_t *qp);
/* Allocate an owned data buffer of `bytes` for the tensor (deserialize path
   and backend-allocated STATIC tensors); returns QNN_SUCCESS/QNN_ERROR. */
int  qnn_tensor_reserve_data(qnn_tensor_obj_t *t, uint64_t bytes);
/* Register a tensor into a context pool (grows c->tensors). */
int  qnn_ctx_track_tensor(struct qnn_context_obj *c, qnn_tensor_obj_t *t);
/* Register a graph into a context (grows c->graphs). */
int  qnn_ctx_track_graph(struct qnn_context_obj *c, qnn_graph_obj_t *g);

/* --- HTP dispatch (npu_htp.c) --- */
typedef struct {
	uint32_t soc_model;
	uint32_t dsp_arch;
	uint32_t nsp_cores;
	uint32_t vtcm_kb;
	uint64_t dma_mask_bits;
} qnn_htp_infra_t;

#define QNN_HTP_INIT_ABI_UNAVAILABLE (-2)
int  qnn_htp_init(qnn_backend_obj_t *b);   /* 0 = compatible skel ready */
int  qnn_htp_deinit(qnn_backend_obj_t *b);
const qnn_htp_infra_t *qnn_htp_get_infra(void);
int  qnn_htp_prepare(qnn_graph_obj_t *g);
int  qnn_htp_execute(qnn_graph_obj_t *g, qnn_tensor_obj_t *const *ins,
                     uint32_t num_ins, qnn_tensor_obj_t *const *outs,
                     uint32_t num_outs);
void qnn_htp_graph_release(qnn_graph_obj_t *g);
int  qnn_htp_power_vote(qnn_backend_obj_t *b, QnnHtp_PerfMode_t mode);

/* --- reference executor (npu_ref_exec.c) --- */
qnn_ref_exec_t *qnn_ref_build(qnn_graph_obj_t *g, char *err, size_t errsz);
int  qnn_ref_execute(qnn_graph_obj_t *g, qnn_ref_exec_t *plan,
                     qnn_tensor_obj_t *const *ins, uint32_t num_ins,
                     qnn_tensor_obj_t *const *outs, uint32_t num_outs,
                     Qnn_ProfileHandle_t prof, char *err, size_t errsz);
void qnn_ref_free(qnn_ref_exec_t *plan);

/* --- provider entry (qnn_interface.c) --- */
#define QNN_API_VER_MAJOR 1u
#define QNN_API_VER_MINOR 0u
#define QNN_API_VER_PATCH 0u

/* Typed view of QnnProvider_t.functionTable: pointers to the standard QNN
   component APIs, in the QnnInterface.h provider-table order.  The real SDK
   splits this per component (QnnBackendCreateFn_t etc.) and versions each
   sub-table; this port keeps one flat table because qnn_core.h exposes the
   component functions under their plain Qnn* names. */
typedef struct qnn_function_table {
	/* QnnBackend */
	Qnn_ErrorHandle_t (*backendCreate)(const Qnn_ConfigParam_t *, uint32_t,
	                                   Qnn_BackendHandle_t *);
	Qnn_ErrorHandle_t (*backendFree)(Qnn_BackendHandle_t);
	Qnn_ErrorHandle_t (*backendGetApiVersion)(Qnn_BackendHandle_t,
	                                          Qnn_Version_t *);
	Qnn_ErrorHandle_t (*backendGetBuildId)(Qnn_BackendHandle_t,
	                                       const char **);
	Qnn_ErrorHandle_t (*backendGetSupportedOperations)(Qnn_BackendHandle_t,
	                                                   const char **,
	                                                   uint32_t *);
	Qnn_ErrorHandle_t (*backendValidateOpConfig)(Qnn_BackendHandle_t,
	                                             const char *,
	                                             const Qnn_ConfigParam_t *,
	                                             uint32_t);
	Qnn_ErrorHandle_t (*backendRegisterOpPackage)(Qnn_BackendHandle_t,
	                                              const char *);
	Qnn_ErrorHandle_t (*propertyHasCapability)(Qnn_BackendHandle_t,
	                                           const char *, bool *);
	/* QnnDevice */
	Qnn_ErrorHandle_t (*deviceCreate)(Qnn_BackendHandle_t,
	                                  const Qnn_ConfigParam_t *, uint32_t,
	                                  Qnn_DeviceHandle_t *);
	Qnn_ErrorHandle_t (*deviceFree)(Qnn_DeviceHandle_t);
	Qnn_ErrorHandle_t (*deviceGetInfrastructure)(Qnn_DeviceHandle_t,
	                                             void **);
	Qnn_ErrorHandle_t (*deviceGetPlatformInfo)(Qnn_DeviceHandle_t,
	                                           const Qnn_ConfigParam_t **,
	                                           uint32_t *);
	Qnn_ErrorHandle_t (*deviceFreePlatformInfo)(Qnn_DeviceHandle_t,
	                                            const Qnn_ConfigParam_t *,
	                                            uint32_t);
	/* QnnContext */
	Qnn_ErrorHandle_t (*contextCreate)(Qnn_BackendHandle_t,
	                                   Qnn_DeviceHandle_t,
	                                   const Qnn_ConfigParam_t *, uint32_t,
	                                   Qnn_ContextHandle_t *);
	Qnn_ErrorHandle_t (*contextCreateFromBinary)(Qnn_BackendHandle_t,
	                                             Qnn_DeviceHandle_t,
	                                             const uint8_t *, uint64_t,
	                                             const Qnn_ConfigParam_t *,
	                                             uint32_t,
	                                             Qnn_ContextHandle_t *);
	Qnn_ErrorHandle_t (*contextCreateFromBinaryWithSignal)(
	                           Qnn_BackendHandle_t, Qnn_DeviceHandle_t,
	                           const uint8_t *, uint64_t,
	                           const Qnn_ConfigParam_t *, uint32_t,
	                           Qnn_SignalHandle_t, Qnn_ContextHandle_t *);
	Qnn_ErrorHandle_t (*contextCreateFromBinaryWithCallback)(
	                           Qnn_BackendHandle_t, Qnn_DeviceHandle_t,
	                           const uint8_t *, uint64_t,
	                           const Qnn_ConfigParam_t *, uint32_t,
	                           void (*)(Qnn_ErrorHandle_t, void *), void *,
	                           Qnn_ContextHandle_t *);
	Qnn_ErrorHandle_t (*contextCreateFromBinaryListAsync)(
	                           Qnn_BackendHandle_t, Qnn_DeviceHandle_t,
	                           const uint8_t **, const uint64_t *, uint32_t,
	                           const Qnn_ConfigParam_t *, uint32_t,
	                           Qnn_SignalHandle_t, Qnn_ContextHandle_t *);
	Qnn_ErrorHandle_t (*contextFinalize)(Qnn_ContextHandle_t);
	Qnn_ErrorHandle_t (*contextFree)(Qnn_ContextHandle_t);
	Qnn_ErrorHandle_t (*contextSetConfig)(Qnn_ContextHandle_t,
	                                      const Qnn_ConfigParam_t *,
	                                      uint32_t);
	Qnn_ErrorHandle_t (*contextGetProperty)(Qnn_ContextHandle_t,
	                                        const char *, const char **);
	Qnn_ErrorHandle_t (*contextGetBinarySize)(Qnn_ContextHandle_t,
	                                          uint64_t *);
	Qnn_ErrorHandle_t (*contextGetBinary)(Qnn_ContextHandle_t, uint8_t *,
	                                      uint64_t);
	Qnn_ErrorHandle_t (*contextValidateBinary)(Qnn_BackendHandle_t,
	                                           Qnn_DeviceHandle_t,
	                                           const uint8_t *, uint64_t,
	                                           bool *);
	/* QnnGraph */
	Qnn_ErrorHandle_t (*graphCreate)(Qnn_ContextHandle_t, const char *,
	                                 Qnn_GraphHandle_t *);
	Qnn_ErrorHandle_t (*graphRetrieve)(Qnn_ContextHandle_t, const char *,
	                                   Qnn_GraphHandle_t *);
	Qnn_ErrorHandle_t (*graphAddNode)(Qnn_GraphHandle_t, const char *,
	                                  Qnn_TensorHandle_t *, uint32_t,
	                                  Qnn_TensorHandle_t *, uint32_t,
	                                  const Qnn_ConfigParam_t *, uint32_t);
	Qnn_ErrorHandle_t (*graphFinalize)(Qnn_GraphHandle_t);
	Qnn_ErrorHandle_t (*graphExecute)(Qnn_GraphHandle_t,
	                                  Qnn_TensorHandle_t *, uint32_t,
	                                  Qnn_TensorHandle_t *, uint32_t,
	                                  Qnn_ProfileHandle_t);
	Qnn_ErrorHandle_t (*graphExecuteAsync)(Qnn_GraphHandle_t,
	                                       Qnn_TensorHandle_t *, uint32_t,
	                                       Qnn_TensorHandle_t *, uint32_t,
	                                       Qnn_ProfileHandle_t,
	                                       Qnn_SignalHandle_t);
	Qnn_ErrorHandle_t (*graphGetProperty)(Qnn_GraphHandle_t, const char *,
	                                      const char **);
	Qnn_ErrorHandle_t (*graphFree)(Qnn_GraphHandle_t);
	/* QnnTensor */
	Qnn_ErrorHandle_t (*tensorCreateContextTensor)(Qnn_ContextHandle_t,
	                           const char *, Qnn_TensorType_t,
	                           Qnn_DataType_t, const Qnn_Shape_t *,
	                           const Qnn_QuantParams_t *,
	                           Qnn_TensorHandle_t *);
	Qnn_ErrorHandle_t (*tensorCreateGraphTensor)(Qnn_GraphHandle_t,
	                           const char *, Qnn_TensorType_t,
	                           Qnn_DataType_t, const Qnn_Shape_t *,
	                           const Qnn_QuantParams_t *,
	                           Qnn_TensorHandle_t *);
	Qnn_ErrorHandle_t (*tensorFree)(Qnn_TensorHandle_t);
	Qnn_ErrorHandle_t (*tensorSetData)(Qnn_TensorHandle_t, void *,
	                                   uint64_t);
	Qnn_ErrorHandle_t (*tensorGetData)(Qnn_TensorHandle_t, void **,
	                                   uint64_t *);
	Qnn_ErrorHandle_t (*tensorGetShape)(Qnn_TensorHandle_t, Qnn_Shape_t *);
	Qnn_ErrorHandle_t (*tensorGetDataType)(Qnn_TensorHandle_t,
	                                       Qnn_DataType_t *);
	Qnn_ErrorHandle_t (*tensorGetTensorType)(Qnn_TensorHandle_t,
	                                         Qnn_TensorType_t *);
	Qnn_ErrorHandle_t (*tensorGetQuantParams)(Qnn_TensorHandle_t,
	                                          Qnn_QuantParams_t *);
	/* QnnMem */
	Qnn_ErrorHandle_t (*memRegister)(Qnn_BackendHandle_t, int, uint64_t,
	                                 uint64_t, void **);
	Qnn_ErrorHandle_t (*memUnregister)(Qnn_BackendHandle_t, void *);
	Qnn_ErrorHandle_t (*memGetBufferFd)(Qnn_BackendHandle_t, void *,
	                                    int *);
	/* QnnSignal */
	Qnn_ErrorHandle_t (*signalCreate)(Qnn_BackendHandle_t,
	                                  Qnn_SignalHandle_t *);
	Qnn_ErrorHandle_t (*signalWait)(Qnn_SignalHandle_t, uint32_t);
	Qnn_ErrorHandle_t (*signalFree)(Qnn_SignalHandle_t);
	/* QnnProfile */
	Qnn_ErrorHandle_t (*profileCreate)(Qnn_BackendHandle_t,
	                                   const Qnn_ConfigParam_t *, uint32_t,
	                                   Qnn_ProfileHandle_t *);
	Qnn_ErrorHandle_t (*profileSetConfig)(Qnn_ProfileHandle_t,
	                                      const Qnn_ConfigParam_t *,
	                                      uint32_t);
	Qnn_ErrorHandle_t (*profileGetData)(Qnn_ProfileHandle_t, uint8_t *,
	                                    uint64_t *);
	Qnn_ErrorHandle_t (*profileFree)(Qnn_ProfileHandle_t);
	/* QnnLog */
	Qnn_ErrorHandle_t (*logInitialize)(QnnLog_Callback_t, void *);
	Qnn_ErrorHandle_t (*logSetCallback)(QnnLog_Callback_t, void *);
	Qnn_ErrorHandle_t (*logFree)(void);
	/* QnnOpPackage */
	Qnn_ErrorHandle_t (*opPackageRegister)(Qnn_BackendHandle_t,
	                                       const char *);
	/* HTP private extensions (qnn_htp_ext.h) */
	Qnn_ErrorHandle_t (*htpContextSetPerfConfig)(QnnHtp_ContextHandle_t,
	                                               QnnHtp_PerfMode_t);
	Qnn_ErrorHandle_t (*htpContextSetVtcmLimit)(QnnHtp_ContextHandle_t,
	                                            uint64_t);
	Qnn_ErrorHandle_t (*htpContextRegisterSharedBuffer)(
	                           QnnHtp_ContextHandle_t, int, uint64_t,
	                           uint64_t, void **);
	Qnn_ErrorHandle_t (*htpContextSetConcurrentGroup)(
	                           QnnHtp_ContextHandle_t, uint32_t);
	Qnn_ErrorHandle_t (*htpGraphSetPrecisionHint)(QnnHtp_GraphHandle_t,
	                                              QnnHtp_PrecisionHint_t);
	Qnn_ErrorHandle_t (*htpGraphSetSkipDataFlush)(QnnHtp_GraphHandle_t,
	                                              bool);
	Qnn_ErrorHandle_t (*htpDeviceSetDspCoreMask)(QnnHtp_DeviceHandle_t,
	                                             uint32_t);
	Qnn_ErrorHandle_t (*htpProfileEnableHwCounters)(Qnn_ProfileHandle_t,
	                                                bool);
} qnn_function_table_t;

#ifdef __cplusplus
}
#endif

#endif /* QNN_CORE_INTERNAL_H */
