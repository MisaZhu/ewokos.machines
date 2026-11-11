/*
 * qnn_interface.c - QnnInterface_getProviders / getApiVersion and the
 * provider function table.
 *
 * The vendor SDK returns one QnnInterface_t per loaded backend library with
 * a versioned union of component function pointers; qnn_core.h flattens
 * that into a single QnnProvider_t { backendId, functionTable }, so this
 * file exposes exactly one provider - the HTP backend, the only backend
 * libQnnHtp.so ships - whose table wires every declared API to the
 * implementations in this library.
 */
#include <adrenonpu/qnn_core_internal.h>

static const char *const QNN_HTP_BACKEND_ID = "HTP";

static qnn_function_table_t g_table = {
	/* QnnBackend */
	QnnBackend_create,
	QnnBackend_free,
	QnnBackend_getApiVersion,
	QnnBackend_getBuildId,
	QnnBackend_getSupportedOperations,
	QnnBackend_validateOpConfig,
	QnnBackend_registerOpPackage,
	QnnProperty_hasCapability,
	/* QnnDevice */
	QnnDevice_create,
	QnnDevice_free,
	QnnDevice_getInfrastructure,
	QnnDevice_getPlatformInfo,
	QnnDevice_freePlatformInfo,
	/* QnnContext */
	QnnContext_create,
	QnnContext_createFromBinary,
	QnnContext_createFromBinaryWithSignal,
	QnnContext_createFromBinaryWithCallback,
	QnnContext_createFromBinaryListAsync,
	QnnContext_finalize,
	QnnContext_free,
	QnnContext_setConfig,
	QnnContext_getProperty,
	QnnContext_getBinarySize,
	QnnContext_getBinary,
	QnnContext_validateBinary,
	/* QnnGraph */
	QnnGraph_create,
	QnnGraph_retrieve,
	QnnGraph_addNode,
	QnnGraph_finalize,
	QnnGraph_execute,
	QnnGraph_executeAsync,
	QnnGraph_getProperty,
	QnnGraph_free,
	/* QnnTensor */
	QnnTensor_createContextTensor,
	QnnTensor_createGraphTensor,
	QnnTensor_free,
	QnnTensor_setData,
	QnnTensor_getData,
	QnnTensor_getShape,
	QnnTensor_getDataType,
	QnnTensor_getTensorType,
	QnnTensor_getQuantParams,
	/* QnnMem */
	QnnMem_register,
	QnnMem_unregister,
	QnnMem_getBufferFd,
	/* QnnSignal */
	QnnSignal_create,
	QnnSignal_wait,
	QnnSignal_free,
	/* QnnProfile */
	QnnProfile_create,
	QnnProfile_setConfig,
	QnnProfile_getData,
	QnnProfile_free,
	/* QnnLog */
	QnnLog_initialize,
	QnnLog_setCallback,
	QnnLog_free,
	/* QnnOpPackage */
	QnnOpPackage_register,
	/* HTP private extensions */
	QnnHtpContext_SetPerfConfig,
	QnnHtpContext_SetVtcmLimit,
	QnnHtpContext_RegisterSharedBuffer,
	QnnHtpContext_SetConcurrentGroup,
	QnnHtpGraph_SetPrecisionHint,
	QnnHtpGraph_SetSkipDataFlush,
	QnnHtpDevice_SetDspCoreMask,
	QnnHtpProfile_EnableHwCounters,
};

static const QnnProvider_t g_providers[1] = {
	{ QNN_HTP_BACKEND_ID, &g_table },
};

Qnn_ErrorHandle_t QnnInterface_getProviders(const QnnProvider_t **providers,
                                            uint32_t *numProviders)
{
	if (!providers || !numProviders)
		return QNN_ERROR;
	*providers = g_providers;
	*numProviders = 1;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnInterface_getApiVersion(Qnn_Version_t *version)
{
	if (!version)
		return QNN_ERROR;
	version->major = QNN_API_VER_MAJOR;
	version->minor = QNN_API_VER_MINOR;
	version->patch = QNN_API_VER_PATCH;
	return QNN_SUCCESS;
}
