/*
 * qnn_device.c - QnnDevice_* implementation.
 *
 * The vendor HTP backend uses QnnDevice for multi-device SoCs; on SC8280XP
 * there is exactly one HTP device (2 NSP cores behind one cDSP), so create
 * mostly records tuning state (core mask) and getPlatformInfo reports the
 * static SoC/NSP capability set.  getInfrastructure hands out the HTP
 * device infrastructure blob the vendor SDK passes to context creation;
 * here it is the qnn_htp_infra_t filled at backend probe time.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static platform info strings (values filled per backend at create). */
static char g_soc_model_s[16];
static char g_dsp_arch_s[16];
static char g_nsp_cores_s[8];
static char g_vtcm_s[16];

static const Qnn_ConfigParam_t g_platform_info[QNN_PLATFORM_INFO_MAX] = {
	{ "soc.model",        g_soc_model_s },
	{ "soc.vendor",       "Qualcomm"    },
	{ "dsp.arch",         g_dsp_arch_s  },
	{ "dsp.nsp_cores",    g_nsp_cores_s },
	{ "dsp.vtcm_kb",      g_vtcm_s      },
	{ "backend.id",       "HTP"         },
	{ "backend.build",    "adrenonpu-1.0.0-q8b-sc8280xp" },
};
#define PLATFORM_INFO_COUNT 7u

static void fill_platform_strings(qnn_backend_obj_t *b)
{
	snprintf(g_soc_model_s, sizeof(g_soc_model_s), "%u", b->soc_model);
	snprintf(g_dsp_arch_s, sizeof(g_dsp_arch_s), "%u", b->dsp_arch);
	snprintf(g_nsp_cores_s, sizeof(g_nsp_cores_s), "%u",
	         (unsigned)CDSP_NSP_CORES);
	snprintf(g_vtcm_s, sizeof(g_vtcm_s), "%u", (unsigned)CDSP_VTCM_KB);
}

Qnn_ErrorHandle_t QnnDevice_create(Qnn_BackendHandle_t backend,
                                   const Qnn_ConfigParam_t *cfg, uint32_t nCfg,
                                   Qnn_DeviceHandle_t *dev)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_device_obj_t *d;

	if (!b || !dev || (b->native_mode && nCfg))
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;

	d = (qnn_device_obj_t *)qnn_obj_alloc(QNN_OBJ_DEVICE, sizeof(*d));
	if (!d)
		return QNN_ERROR;
	d->backend = b;
	if (qnn_cfg_store(d->config_keys, d->config_vals, &d->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS) {
		qnn_cfg_free(d->config_keys, d->config_vals, &d->num_config);
		qnn_obj_free(d);
		return QNN_ERROR;
	}
	d->htp = b->htp_defaults;
	d->htp.dsp_core_mask = (uint32_t)qnn_cfg_get_int(
	                cfg, nCfg, "dsp_core_mask",
	                (int)b->htp_defaults.dsp_core_mask);
	fill_platform_strings(b);

	if (b->native_mode) b->native_devices++;
	*dev = (Qnn_DeviceHandle_t)(uintptr_t)d;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnDevice_free(Qnn_DeviceHandle_t dev)
{
	qnn_device_obj_t *d = (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE);

	if (!d)
		return QNN_ERROR;
	if (d->backend && d->backend->native_mode) d->backend->native_devices--;
	qnn_cfg_free(d->config_keys, d->config_vals, &d->num_config);
	qnn_obj_free(d);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnDevice_getInfrastructure(Qnn_DeviceHandle_t dev,
                                              void **infra)
{
	qnn_device_obj_t *d = (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE);
	const qnn_htp_infra_t *htp;

	if (!d || !infra)
		return QNN_ERROR;
	if (d->backend && d->backend->native_mode) { *infra = NULL; return QNN_SUCCESS; }
	htp = qnn_htp_get_infra();
	/* The vendor API returns SUCCESS with *infra == NULL for backends
	   without an infrastructure blob; do the same when the HTP layer
	   never initialized. */
	*infra = htp ? (void *)htp : NULL;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnDevice_getPlatformInfo(Qnn_DeviceHandle_t dev,
                                            const Qnn_ConfigParam_t **info,
                                            uint32_t *nInfo)
{
	qnn_device_obj_t *d = (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE);

	if (!d || !info || !nInfo)
		return QNN_ERROR;
	fill_platform_strings(d->backend);
	*info = g_platform_info;
	*nInfo = PLATFORM_INFO_COUNT;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnDevice_freePlatformInfo(Qnn_DeviceHandle_t dev,
                                             const Qnn_ConfigParam_t *info,
                                             uint32_t nInfo)
{
	qnn_device_obj_t *d = (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE);

	(void)info;
	(void)nInfo;
	if (!d)
		return QNN_ERROR;
	/* getPlatformInfo hands out static storage - nothing to release. */
	return QNN_SUCCESS;
}
