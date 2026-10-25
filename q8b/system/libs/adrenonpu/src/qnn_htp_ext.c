/*
 * qnn_htp_ext.c - HTP backend private extensions (qnn_htp_ext.h).
 *
 * These mirror the QnnHtp* tuning surface of the vendor libQnnHtp.so: perf
 * tier, VTCM limit, concurrency group and DSP core mask steer the cDSP DCVS /
 * NSP scheduling; precision hint and skip-flush are graph-level zero-copy
 * knobs; shared-buffer registration maps a dma region for cDSP access.  Each
 * call records the knob on the target object and, when the cDSP session is
 * live, pushes it through the reconstructed transport (power vote / mmap).
 * With no session the state is still retained so a later prepare() honors it.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>

Qnn_ErrorHandle_t QnnHtpContext_SetPerfConfig(QnnHtp_ContextHandle_t ctx,
                                              QnnHtp_PerfMode_t perfMode)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);

	if (!c || (c->backend && c->backend->native_mode))
		return QNN_ERROR;
	if ((unsigned)perfMode > (unsigned)QNN_HTP_PERF_MODE_TURBO)
		return QNN_ERROR;
	c->htp.perf_mode = perfMode;
	if (c->backend && c->backend->npu_available)
		qnn_htp_power_vote(c->backend, perfMode);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpContext_SetVtcmLimit(QnnHtp_ContextHandle_t ctx,
                                             uint64_t vtcmLimitBytes)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint64_t cap = (uint64_t)CDSP_VTCM_KB * 1024u;

	if (!c || (c->backend && c->backend->native_mode))
		return QNN_ERROR;
	/* Clamp to the physical VTCM of one NSP (512 KB on V68). */
	if (vtcmLimitBytes > cap)
		vtcmLimitBytes = cap;
	c->htp.vtcm_limit = vtcmLimitBytes;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpContext_RegisterSharedBuffer(QnnHtp_ContextHandle_t ctx,
        int fd, uint64_t offset, uint64_t size, void **outHostPtr)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	qnn_shared_buf_t *sb;

	if (!c || (c->backend && c->backend->native_mode) || !outHostPtr || size == 0)
		return QNN_ERROR;
	sb = qnn_shared_buf_alloc(c->backend, fd, offset, size);
	if (!sb)
		return QNN_ERROR;
	*outHostPtr = sb->host;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpContext_SetConcurrentGroup(QnnHtp_ContextHandle_t ctx,
                                                   uint32_t groupId)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);

	if (!c || (c->backend && c->backend->native_mode))
		return QNN_ERROR;
	c->htp.concurrent_group = groupId;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpGraph_SetPrecisionHint(QnnHtp_GraphHandle_t graph,
                                               QnnHtp_PrecisionHint_t hint)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);

	if (!g || g->native_graph)
		return QNN_ERROR;
	if ((unsigned)hint > (unsigned)QNN_HTP_PRECISION_STRICT)
		return QNN_ERROR;
	g->htp.precision = hint;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpGraph_SetSkipDataFlush(QnnHtp_GraphHandle_t graph,
                                               bool skip)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);

	if (!g || g->native_graph)
		return QNN_ERROR;
	g->htp.skip_flush = skip;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpDevice_SetDspCoreMask(QnnHtp_DeviceHandle_t dev,
                                              uint32_t coreMask)
{
	qnn_device_obj_t *d = (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE);
	uint32_t valid = (1u << CDSP_NSP_CORES) - 1u;

	if (!d || (d->backend && d->backend->native_mode))
		return QNN_ERROR;
	if (coreMask == 0 || (coreMask & ~valid) != 0)
		return QNN_ERROR;
	d->htp.dsp_core_mask = coreMask;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnHtpProfile_EnableHwCounters(Qnn_ProfileHandle_t prof,
                                                 bool enable)
{
	qnn_profile_obj_t *p = (qnn_profile_obj_t *)
		qnn_obj_from_handle(prof, QNN_OBJ_PROFILE);

	if (!p || (p->backend && p->backend->native_mode))
		return QNN_ERROR;
	p->hw_counters = enable;
	/* Detailed level implies per-op timing is wanted alongside counters. */
	if (enable && p->level < 2)
		p->level = 2;
	return QNN_SUCCESS;
}
