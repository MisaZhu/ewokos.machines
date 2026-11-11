/*
 * qnn_backend.c - QnnBackend_* / QnnProperty_hasCapability /
 * QnnOpPackage_register implementation.
 *
 * Backend create is where the vendor libQnnHtp.so brings up its session:
 * it opens the cDSP through libcdsprpc (remote_session_open on the cdsp
 * domain) and queries the skel for the SoC capability set.  This port runs
 * the same sequence against the reconstructed transport (npu_cdsp.c PAS
 * boot + npu_fastrpc.c session), and - exactly like the libadreno bring-up
 * chain - treats an unavailable kernel service as a decline, not a crash:
 * the backend stays usable and graphs fall back to the CPU reference
 * executor until the kernel grows the cDSP PAS + FastRPC ABI.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <stdlib.h>
#include <string.h>

#define QNN_NPU_BUILD_ID "adrenonpu-1.0.0-q8b-sc8280xp"

/* NPU session probe: cDSP PAS boot followed by the FastRPC session open.
   Records identity even on decline so getPlatformInfo/capabilities stay
   truthful about the target SoC. */
static void npu_probe(qnn_backend_obj_t *b)
{
	bool was_booted = npu_cdsp_booted();
	int rc = npu_cdsp_probe();

	b->soc_model = CDSP_SOC_SC8280XP;
	b->dsp_arch = CDSP_ARCH_V68;
	b->npu_probe_rc = rc;
	b->npu_available = (rc == NPU_PROBE_OK);
	b->cdsp_owned = (!was_booted || rc == NPU_PROBE_OK) && npu_cdsp_booted();
	b->frpc_session = b->npu_available ? frpc_session_get() : 0u;
	/* A compatible graph ABI is required in addition to a live transport. */
	if (b->npu_available) {
		int htp_rc = qnn_htp_init(b);

		if (htp_rc != 0) {
			/* Release the session and PAS image owned by this probe when
			 * no compatible graph executor can use them. */
			int stop_rc = npu_cdsp_shutdown();
			b->cdsp_owned = stop_rc != 0;
			b->npu_available = 0;
			b->npu_probe_rc = htp_rc == QNN_HTP_INIT_ABI_UNAVAILABLE ?
				NPU_PROBE_HTP_ABI_MISSING : NPU_PROBE_RPC_FAILED;
			if (!stop_rc) b->frpc_session = 0u;
		}
	}
	if (!b->npu_available)
		qnn_log(QNN_LOG_LVL_WARN,
		        "HTP backend: hardware execution unavailable (rc=%d); graphs will "
		        "run on the CPU reference executor\n", b->npu_probe_rc);
	else
		qnn_log(QNN_LOG_LVL_INFO,
		        "HTP backend: cDSP session %u up (soc=%u arch=v%u)\n",
		        b->frpc_session, b->soc_model, b->dsp_arch);
}

Qnn_ErrorHandle_t QnnBackend_create(const Qnn_ConfigParam_t *config,
                                    uint32_t numConfig,
                                    Qnn_BackendHandle_t *backend)
{
	qnn_backend_obj_t *b;

	if (!backend || qnn_native_transport_owned())
		return QNN_ERROR;
	if (numConfig > 0 && !config)
		return QNN_ERROR;

	b = (qnn_backend_obj_t *)qnn_obj_alloc(QNN_OBJ_BACKEND, sizeof(*b));
	if (!b)
		return QNN_ERROR;

	if (qnn_cfg_store(b->config_keys, b->config_vals, &b->num_config,
	                  config, numConfig, 8) != QNN_SUCCESS) {
		qnn_cfg_free(b->config_keys, b->config_vals, &b->num_config);
		qnn_obj_free(b);
		return QNN_ERROR;
	}

	/* HTP tuning defaults (qnn_htp_ext.h semantics). */
	b->htp_defaults.perf_mode = (QnnHtp_PerfMode_t)qnn_cfg_get_int(
	                config, numConfig, "perf_mode", QNN_HTP_PERF_MODE_BURST);
	b->htp_defaults.vtcm_limit = (uint64_t)qnn_cfg_get_int(
	                config, numConfig, "vtcm_limit", 0);
	b->htp_defaults.dsp_core_mask = (uint32_t)qnn_cfg_get_int(
	                config, numConfig, "dsp_core_mask",
	                (1u << CDSP_NSP_CORES) - 1u);
	b->htp_defaults.precision = QNN_HTP_PRECISION_DEFAULT;

	const char *execution = qnn_cfg_get(config, numConfig, "execution_backend");
	if (execution && strcmp(execution, "htp_native") != 0) {
		qnn_cfg_free(b->config_keys, b->config_vals, &b->num_config);
		qnn_obj_free(b);
		return QNN_ERROR;
	}
	b->native_mode = execution != NULL;
	if (b->native_mode) for (uint32_t i = 0; i < numConfig; i++) {
		if (!config[i].key || strcmp(config[i].key, "execution_backend")) {
			qnn_cfg_free(b->config_keys, b->config_vals, &b->num_config);
			qnn_obj_free(b); return QNN_ERROR;
		}
	}
	if (b->native_mode) {
		b->soc_model = CDSP_SOC_SC8280XP;
		b->dsp_arch = CDSP_ARCH_V68;
		b->npu_probe_rc = NPU_PROBE_HTP_ABI_MISSING;
	} else npu_probe(b);

	*backend = (Qnn_BackendHandle_t)(uintptr_t)b;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnBackend_free(Qnn_BackendHandle_t backend)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	uint32_t i;

	if (!b)
		return QNN_ERROR;
	if (b->native_mode && (b->native_devices || qnn_native_backend_in_use(b))) return QNN_ERROR;
	/* Keep the backend handle and its remaining ownership list retryable
	 * when an AP mapping cannot be revoked. Do this before module/config
	 * teardown so a failed buffer cleanup does not partially deinit them. */
	while (b->shared) {
		qnn_shared_buf_t *sb = b->shared;

		if (qnn_shared_free_storage(sb) != QNN_SUCCESS)
			return QNN_ERROR;
		b->shared = sb->next;
		free(sb);
	}
	if (b->native_mode && qnn_native_backend_close(b) != 0) return QNN_ERROR;
	if (!b->native_mode && b->npu_available && qnn_htp_deinit(b) != 0)
		return QNN_ERROR;
	if (b->cdsp_owned || b->npu_available) {
		/* Shared mappings must be released while the FastRPC session is
		 * still alive.  Then stop the PAS image owned by this backend. */
		if (npu_cdsp_shutdown() != 0) return QNN_ERROR;
		b->cdsp_owned = 0;
	}
	qnn_cfg_free(b->config_keys, b->config_vals, &b->num_config);
	for (i = 0; i < b->num_op_packages; i++)
		free(b->op_packages[i]);
	qnn_obj_free(b);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnBackend_getApiVersion(Qnn_BackendHandle_t backend,
                                           Qnn_Version_t *ver)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	if (!b || !ver)
		return QNN_ERROR;
	ver->major = QNN_API_VER_MAJOR;
	ver->minor = QNN_API_VER_MINOR;
	ver->patch = QNN_API_VER_PATCH;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnBackend_getBuildId(Qnn_BackendHandle_t backend,
                                        const char **buildId)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	if (!b || !buildId)
		return QNN_ERROR;
	*buildId = QNN_NPU_BUILD_ID;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnBackend_getSupportedOperations(Qnn_BackendHandle_t backend,
                                                    const char **ops,
                                                    uint32_t *numOps)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	const qnn_op_info_t *table = qnn_op_table();
	uint32_t n = qnn_op_count();
	uint32_t i;

	if (!b || !ops || !numOps)
		return QNN_ERROR;
	if (b->native_mode) { *numOps = 0; return QNN_SUCCESS; }
	for (i = 0; i < n && i < *numOps; i++)
		ops[i] = table[i].name;
	*numOps = n < *numOps ? n : *numOps;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnBackend_validateOpConfig(Qnn_BackendHandle_t backend,
                                              const char *opName,
                                              const Qnn_ConfigParam_t *cfg,
                                              uint32_t nCfg)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	const char *keys[16];
	uint32_t i;

	if (!b || !opName || b->native_mode)
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;
	for (i = 0; i < nCfg && i < 16; i++)
		keys[i] = cfg[i].key;
	return qnn_op_validate_config(opName, keys, nCfg < 16 ? nCfg : 16) ==
	       QNN_SUCCESS ? QNN_SUCCESS : QNN_ERROR;
}

Qnn_ErrorHandle_t QnnBackend_registerOpPackage(Qnn_BackendHandle_t backend,
                                               const char *pkgPath)
{
	return QnnOpPackage_register(backend, pkgPath);
}

Qnn_ErrorHandle_t QnnOpPackage_register(Qnn_BackendHandle_t backend,
                                        const char *pkgPath)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	if (!b || !pkgPath || !*pkgPath || b->native_mode)
		return QNN_ERROR;
	if (b->num_op_packages >= QNN_MAX_OP_PACKAGES)
		return QNN_ERROR;
	/* The vendor stack dlopen()s the package .so and pulls its
	   QnnOpPackage_getProviders entry; EwokOS user space has no dynamic
	   loader for foreign ELFs, so the path is recorded and the ops stay
	   unresolved (validateOpConfig rejects them) until a package loader
	   exists.  Registration itself succeeds, matching the SDK where a
	   registered-but-unloadable package only fails at first use. */
	b->op_packages[b->num_op_packages] = qnn_strdup_safe(pkgPath);
	if (!b->op_packages[b->num_op_packages])
		return QNN_ERROR;
	b->num_op_packages++;
	qnn_log(QNN_LOG_LVL_INFO, "op package registered: %s\n", pkgPath);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnProperty_hasCapability(Qnn_BackendHandle_t backend,
                                            const char *cap, bool *out)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	if (!b || !cap || !out)
		return QNN_ERROR;
	if (strcmp(cap, "HTP") == 0 ||
	    strcmp(cap, "npu.backend.htp") == 0) {
		*out = true;              /* this provider IS the HTP backend */
	} else if (strcmp(cap, "npu.cdsp.available") == 0) {
		*out = b->npu_available != 0;
	} else if (strcmp(cap, "npu.cpu_reference") == 0) {
		*out = !b->native_mode;
	} else if (strcmp(cap, "npu.native_context") == 0) {
		*out = b->native_mode;
	} else if (strcmp(cap, "context_binary") == 0 ||
	           strcmp(cap, "npu.context.serialize") == 0) {
		*out = true;
	} else if (strcmp(cap, "shared_buffer") == 0 ||
	           strcmp(cap, "npu.mem.ion") == 0) {
		*out = !b->native_mode;
	} else if (strcmp(cap, "async_execution") == 0) {
		*out = true;
	} else if (strcmp(cap, "op_package_dl") == 0) {
		*out = false;             /* no foreign-ELF loader (see above) */
	} else {
		*out = false;
	}
	return QNN_SUCCESS;
}
