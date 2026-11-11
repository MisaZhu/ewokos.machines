/*
 * qnn_profile.c - QnnProfile_* implementation.
 *
 * Profiling collects per-execute timing records (and, when the HTP hardware
 * counters are enabled through qnn_htp_ext.h, marks the profile as HW-counter
 * mode).  getData serializes the records into a small "QPRF" blob so a caller
 * can dump timings the same way it reads the vendor profile buffer.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <stdlib.h>
#include <string.h>

#define QPRF_MAGIC0 'Q'
#define QPRF_MAGIC1 'P'
#define QPRF_MAGIC2 'R'
#define QPRF_MAGIC3 'F'
#define QPRF_VERSION 1u
#define QPRF_HDR 16u
#define QPRF_REC (4u + 8u + 28u)

void qnn_profile_record(Qnn_ProfileHandle_t prof, uint32_t idx,
                        const char *op, uint64_t usec)
{
	qnn_profile_obj_t *p = (qnn_profile_obj_t *)
		qnn_obj_from_handle(prof, QNN_OBJ_PROFILE);
	qnn_prof_rec_t *r;

	if (!p || p->level == 0)
		return;
	if (p->num_recs >= p->rec_cap) {
		uint32_t nc = p->rec_cap ? p->rec_cap * 2u : 16u;
		qnn_prof_rec_t *np = (qnn_prof_rec_t *)
			realloc(p->recs, (size_t)nc * sizeof(*np));

		if (!np)
			return;
		p->recs = np;
		p->rec_cap = nc;
	}
	r = &p->recs[p->num_recs++];
	r->idx = idx;
	r->usec = usec;
	memset(r->name, 0, sizeof(r->name));
	if (op) {
		strncpy(r->name, op, sizeof(r->name) - 1);
		r->name[sizeof(r->name) - 1] = '\0';
	}
}

Qnn_ErrorHandle_t QnnProfile_create(Qnn_BackendHandle_t backend,
                                    const Qnn_ConfigParam_t *cfg, uint32_t nCfg,
                                    Qnn_ProfileHandle_t *prof)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_profile_obj_t *p;

	if (!b || !prof)
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;
	p = (qnn_profile_obj_t *)qnn_obj_alloc(QNN_OBJ_PROFILE, sizeof(*p));
	if (!p)
		return QNN_ERROR;
	p->backend = b;
	p->level = (uint32_t)qnn_cfg_get_int(cfg, nCfg, "level", 1);
	if (qnn_cfg_store(p->config_keys, p->config_vals, &p->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS) {
		qnn_obj_free(p);
		return QNN_ERROR;
	}
	*prof = (Qnn_ProfileHandle_t)(uintptr_t)p;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnProfile_setConfig(Qnn_ProfileHandle_t prof,
                                       const Qnn_ConfigParam_t *cfg,
                                       uint32_t nCfg)
{
	qnn_profile_obj_t *p = (qnn_profile_obj_t *)
		qnn_obj_from_handle(prof, QNN_OBJ_PROFILE);

	if (!p)
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;
	qnn_cfg_free(p->config_keys, p->config_vals, &p->num_config);
	if (qnn_cfg_store(p->config_keys, p->config_vals, &p->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS)
		return QNN_ERROR;
	p->level = (uint32_t)qnn_cfg_get_int(cfg, nCfg, "level", (int)p->level);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnProfile_getData(Qnn_ProfileHandle_t prof, uint8_t *buf,
                                     uint64_t *bufSize)
{
	qnn_profile_obj_t *p = (qnn_profile_obj_t *)
		qnn_obj_from_handle(prof, QNN_OBJ_PROFILE);
	uint64_t need;
	uint64_t off;
	uint32_t i;

	if (!p || !bufSize)
		return QNN_ERROR;
	need = QPRF_HDR + (uint64_t)p->num_recs * QPRF_REC;
	/* Size query: no buffer supplied. */
	if (!buf) {
		*bufSize = need;
		return QNN_SUCCESS;
	}
	if (*bufSize < need) {
		*bufSize = need;
		return QNN_ERROR;
	}
	off = 0;
	buf[off++] = QPRF_MAGIC0;
	buf[off++] = QPRF_MAGIC1;
	buf[off++] = QPRF_MAGIC2;
	buf[off++] = QPRF_MAGIC3;
	memcpy(buf + off, &(uint32_t){ QPRF_VERSION }, 4); off += 4;
	memcpy(buf + off, &p->num_recs, 4); off += 4;
	memcpy(buf + off, &(uint32_t){ p->hw_counters ? 1u : 0u }, 4); off += 4;
	for (i = 0; i < p->num_recs; i++) {
		memcpy(buf + off, &p->recs[i].idx, 4);  off += 4;
		memcpy(buf + off, &p->recs[i].usec, 8); off += 8;
		memcpy(buf + off, p->recs[i].name, 28); off += 28;
	}
	*bufSize = off;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnProfile_free(Qnn_ProfileHandle_t prof)
{
	qnn_profile_obj_t *p = (qnn_profile_obj_t *)
		qnn_obj_from_handle(prof, QNN_OBJ_PROFILE);

	if (!p)
		return QNN_ERROR;
	free(p->recs);
	qnn_cfg_free(p->config_keys, p->config_vals, &p->num_config);
	qnn_obj_free(p);
	return QNN_SUCCESS;
}
