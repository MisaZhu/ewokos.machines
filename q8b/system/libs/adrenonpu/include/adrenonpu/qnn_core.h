#ifndef QNN_CORE_H
#define QNN_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Basic type definitions
// ============================================================================
typedef int32_t Qnn_ErrorHandle_t;
typedef uint64_t Qnn_Handle_t;

typedef Qnn_Handle_t Qnn_BackendHandle_t;
typedef Qnn_Handle_t Qnn_DeviceHandle_t;
typedef Qnn_Handle_t Qnn_ContextHandle_t;
typedef Qnn_Handle_t Qnn_GraphHandle_t;
typedef Qnn_Handle_t Qnn_TensorHandle_t;
typedef Qnn_Handle_t Qnn_SignalHandle_t;
typedef Qnn_Handle_t Qnn_ProfileHandle_t;

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} Qnn_Version_t;

typedef enum {
    QNN_TENSOR_TYPE_UNDEFINED,
    QNN_TENSOR_TYPE_STATIC,
    QNN_TENSOR_TYPE_APP_WRITE,
    QNN_TENSOR_TYPE_APP_READ,
    QNN_TENSOR_TYPE_READWRITE
} Qnn_TensorType_t;

typedef enum {
    QNN_DATATYPE_UNDEFINED,
    QNN_DATATYPE_UINT8,
    QNN_DATATYPE_INT8,
    QNN_DATATYPE_UINT16,
    QNN_DATATYPE_INT16,
    QNN_DATATYPE_UINT32,
    QNN_DATATYPE_INT32,
    QNN_DATATYPE_FLOAT16,
    QNN_DATATYPE_FLOAT32
} Qnn_DataType_t;

typedef struct {
    float scale;
    int32_t zeroPoint;
} Qnn_QuantParams_t;

typedef struct {
    uint32_t rank;
    uint32_t* dims;
} Qnn_Shape_t;

typedef struct {
    const char* key;
    const char* value;
} Qnn_ConfigParam_t;

#define QNN_SUCCESS 0
#define QNN_ERROR  (-1)

// Provider struct returned by QnnInterface_getProviders
typedef struct QnnProvider_t {
    const char* backendId;
    void* functionTable;
} QnnProvider_t;

// ============================================================================
// QnnInterface.h
// ============================================================================
Qnn_ErrorHandle_t QnnInterface_getProviders(const QnnProvider_t** providers, uint32_t* numProviders);
Qnn_ErrorHandle_t QnnInterface_getApiVersion(Qnn_Version_t* version);

// ============================================================================
// QnnBackend.h
// ============================================================================
Qnn_ErrorHandle_t QnnBackend_create(const Qnn_ConfigParam_t* config, uint32_t numConfig,
                                    Qnn_BackendHandle_t* backend);
Qnn_ErrorHandle_t QnnBackend_free(Qnn_BackendHandle_t backend);
Qnn_ErrorHandle_t QnnBackend_getApiVersion(Qnn_BackendHandle_t backend, Qnn_Version_t* ver);
Qnn_ErrorHandle_t QnnBackend_getBuildId(Qnn_BackendHandle_t backend, const char** buildId);
Qnn_ErrorHandle_t QnnBackend_getSupportedOperations(Qnn_BackendHandle_t backend, const char** ops, uint32_t* numOps);
Qnn_ErrorHandle_t QnnBackend_validateOpConfig(Qnn_BackendHandle_t backend, const char* opName, const Qnn_ConfigParam_t* cfg, uint32_t nCfg);
Qnn_ErrorHandle_t QnnBackend_registerOpPackage(Qnn_BackendHandle_t backend, const char* pkgPath);
Qnn_ErrorHandle_t QnnProperty_hasCapability(Qnn_BackendHandle_t backend, const char* cap, bool* out);

// ============================================================================
// QnnDevice.h
// ============================================================================
Qnn_ErrorHandle_t QnnDevice_create(Qnn_BackendHandle_t backend, const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                   Qnn_DeviceHandle_t* dev);
Qnn_ErrorHandle_t QnnDevice_free(Qnn_DeviceHandle_t dev);
Qnn_ErrorHandle_t QnnDevice_getInfrastructure(Qnn_DeviceHandle_t dev, void** infra);
Qnn_ErrorHandle_t QnnDevice_getPlatformInfo(Qnn_DeviceHandle_t dev, const Qnn_ConfigParam_t** info, uint32_t* nInfo);
Qnn_ErrorHandle_t QnnDevice_freePlatformInfo(Qnn_DeviceHandle_t dev, const Qnn_ConfigParam_t* info, uint32_t nInfo);

// ============================================================================
// QnnContext.h
// ============================================================================
Qnn_ErrorHandle_t QnnContext_create(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                    const Qnn_ConfigParam_t* cfg, uint32_t nCfg, Qnn_ContextHandle_t* ctx);
Qnn_ErrorHandle_t QnnContext_createFromBinary(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                              const uint8_t* binBuf, uint64_t binSize,
                                              const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                              Qnn_ContextHandle_t* ctx);
Qnn_ErrorHandle_t QnnContext_createFromBinaryWithSignal(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                                        const uint8_t* binBuf, uint64_t binSize,
                                                        const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                                        Qnn_SignalHandle_t sig, Qnn_ContextHandle_t* ctx);
Qnn_ErrorHandle_t QnnContext_createFromBinaryWithCallback(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                                          const uint8_t* binBuf, uint64_t binSize,
                                                          const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                                          void (*cb)(Qnn_ErrorHandle_t, void*), void* cbCtx,
                                                          Qnn_ContextHandle_t* ctx);
Qnn_ErrorHandle_t QnnContext_createFromBinaryListAsync(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                                       const uint8_t** binBufs, const uint64_t* binSizes, uint32_t numBin,
                                                       const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                                       Qnn_SignalHandle_t sig, Qnn_ContextHandle_t* ctx);
Qnn_ErrorHandle_t QnnContext_finalize(Qnn_ContextHandle_t ctx);
Qnn_ErrorHandle_t QnnContext_free(Qnn_ContextHandle_t ctx);
Qnn_ErrorHandle_t QnnContext_setConfig(Qnn_ContextHandle_t ctx, const Qnn_ConfigParam_t* cfg, uint32_t nCfg);
Qnn_ErrorHandle_t QnnContext_getProperty(Qnn_ContextHandle_t ctx, const char* key, const char** val);
Qnn_ErrorHandle_t QnnContext_getBinarySize(Qnn_ContextHandle_t ctx, uint64_t* outSize);
Qnn_ErrorHandle_t QnnContext_getBinary(Qnn_ContextHandle_t ctx, uint8_t* buf, uint64_t bufLen);
Qnn_ErrorHandle_t QnnContext_validateBinary(Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
                                            const uint8_t* binBuf, uint64_t binSize, bool* valid);

// ============================================================================
// QnnGraph.h
// ============================================================================
Qnn_ErrorHandle_t QnnGraph_create(Qnn_ContextHandle_t ctx, const char* graphName, Qnn_GraphHandle_t* graph);
Qnn_ErrorHandle_t QnnGraph_retrieve(Qnn_ContextHandle_t ctx, const char* graphName, Qnn_GraphHandle_t* graph);
Qnn_ErrorHandle_t QnnGraph_addNode(Qnn_GraphHandle_t graph, const char* opName,
                                   Qnn_TensorHandle_t* inTensors, uint32_t numIn,
                                   Qnn_TensorHandle_t* outTensors, uint32_t numOut,
                                   const Qnn_ConfigParam_t* cfg, uint32_t nCfg);
Qnn_ErrorHandle_t QnnGraph_finalize(Qnn_GraphHandle_t graph);
Qnn_ErrorHandle_t QnnGraph_execute(Qnn_GraphHandle_t graph,
                                   Qnn_TensorHandle_t* inTensors, uint32_t numIn,
                                   Qnn_TensorHandle_t* outTensors, uint32_t numOut,
                                   Qnn_ProfileHandle_t prof);
Qnn_ErrorHandle_t QnnGraph_executeAsync(Qnn_GraphHandle_t graph,
                                        Qnn_TensorHandle_t* inTensors, uint32_t numIn,
                                        Qnn_TensorHandle_t* outTensors, uint32_t numOut,
                                        Qnn_ProfileHandle_t prof, Qnn_SignalHandle_t sig);
Qnn_ErrorHandle_t QnnGraph_getProperty(Qnn_GraphHandle_t graph, const char* key, const char** val);
Qnn_ErrorHandle_t QnnGraph_free(Qnn_GraphHandle_t graph);

// ============================================================================
// QnnTensor.h
// ============================================================================
Qnn_ErrorHandle_t QnnTensor_createContextTensor(Qnn_ContextHandle_t ctx, const char* name,
                                               Qnn_TensorType_t tType, Qnn_DataType_t dType,
                                               const Qnn_Shape_t* shape, const Qnn_QuantParams_t* qp,
                                               Qnn_TensorHandle_t* tensor);
Qnn_ErrorHandle_t QnnTensor_createGraphTensor(Qnn_GraphHandle_t graph, const char* name,
                                             Qnn_TensorType_t tType, Qnn_DataType_t dType,
                                             const Qnn_Shape_t* shape, const Qnn_QuantParams_t* qp,
                                             Qnn_TensorHandle_t* tensor);
Qnn_ErrorHandle_t QnnTensor_free(Qnn_TensorHandle_t tensor);
Qnn_ErrorHandle_t QnnTensor_setData(Qnn_TensorHandle_t tensor, void* data, uint64_t bytes);
Qnn_ErrorHandle_t QnnTensor_getData(Qnn_TensorHandle_t tensor, void** data, uint64_t* bytes);
Qnn_ErrorHandle_t QnnTensor_getShape(Qnn_TensorHandle_t tensor, Qnn_Shape_t* shape);
Qnn_ErrorHandle_t QnnTensor_getDataType(Qnn_TensorHandle_t tensor, Qnn_DataType_t* dtype);
Qnn_ErrorHandle_t QnnTensor_getTensorType(Qnn_TensorHandle_t tensor, Qnn_TensorType_t* ttype);
Qnn_ErrorHandle_t QnnTensor_getQuantParams(Qnn_TensorHandle_t tensor, Qnn_QuantParams_t* qp);

// ============================================================================
// QnnMem.h (ION/DMA fd memory registration)
// ============================================================================
Qnn_ErrorHandle_t QnnMem_register(Qnn_BackendHandle_t backend, int fd, uint64_t offset, uint64_t size, void** hostPtr);
Qnn_ErrorHandle_t QnnMem_unregister(Qnn_BackendHandle_t backend, void* hostPtr);
Qnn_ErrorHandle_t QnnMem_getBufferFd(Qnn_BackendHandle_t backend, void* hostPtr, int* fd);

// ============================================================================
// QnnSignal.h
// ============================================================================
Qnn_ErrorHandle_t QnnSignal_create(Qnn_BackendHandle_t backend, Qnn_SignalHandle_t* sig);
Qnn_ErrorHandle_t QnnSignal_wait(Qnn_SignalHandle_t sig, uint32_t timeoutMs);
Qnn_ErrorHandle_t QnnSignal_free(Qnn_SignalHandle_t sig);

// ============================================================================
// QnnProfile.h
// ============================================================================
Qnn_ErrorHandle_t QnnProfile_create(Qnn_BackendHandle_t backend, const Qnn_ConfigParam_t* cfg, uint32_t nCfg,
                                    Qnn_ProfileHandle_t* prof);
Qnn_ErrorHandle_t QnnProfile_setConfig(Qnn_ProfileHandle_t prof, const Qnn_ConfigParam_t* cfg, uint32_t nCfg);
Qnn_ErrorHandle_t QnnProfile_getData(Qnn_ProfileHandle_t prof, uint8_t* buf, uint64_t* bufSize);
Qnn_ErrorHandle_t QnnProfile_free(Qnn_ProfileHandle_t prof);

// ============================================================================
// QnnLog.h
// ============================================================================
typedef void (*QnnLog_Callback_t)(uint32_t level, const char* msg, void* userData);
Qnn_ErrorHandle_t QnnLog_initialize(QnnLog_Callback_t cb, void* userData);
Qnn_ErrorHandle_t QnnLog_setCallback(QnnLog_Callback_t cb, void* userData);
Qnn_ErrorHandle_t QnnLog_free(void);

// ============================================================================
// QnnOpPackage.h
// ============================================================================
Qnn_ErrorHandle_t QnnOpPackage_register(Qnn_BackendHandle_t backend, const char* pkgPath);

#ifdef __cplusplus
}
#endif

#endif