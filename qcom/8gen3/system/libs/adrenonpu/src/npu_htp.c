/*
 * npu_htp.c - HTP graph dispatch to the cDSP over the FastRPC transport.
 *
 * The method ids and packed graph layout below are this port's experimental
 * skel ABI, NOT the Qualcomm QAIRT ABI.  They require a matching DSP skel.
 * There is currently no SDK-generated adapter for libQnnHtpV68Skel.so.
 * Default builds decline before opening any skel; a development build may
 * explicitly supply Q8B_HTP_PRIVATE_SKEL_URI for its matching private skel.
 * Installing the official library alone must never enable this protocol.
 *
 * I/O tensors use a registered QnnMem DMA buffer directly when the tensor
 * data pointer names one; otherwise the transport's copy path (pa == 0)
 * stages them in the invoke payload and copies outputs back.  If the signed
 * skel or an apps_smmu mapping is missing,
 * the caller (qnn_graph.c) still runs the CPU reference executor.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <stdlib.h>
#include <string.h>

/* Skel method ids (this port's HTP ABI). */
#define HTP_M_PREPARE 1u
#define HTP_M_EXECUTE 2u
#define HTP_M_RELEASE 3u

#define HTP_GRAPH_MAGIC   0x47505448u   /* "HTPG" */
#define HTP_GRAPH_VERSION 1u

static qnn_htp_infra_t g_infra;
static int g_infra_ready;
static uint32_t g_skel_handle;
static unsigned g_skel_epoch;

const qnn_htp_infra_t *qnn_htp_get_infra(void)
{
	if (!g_infra_ready) {
		g_infra.soc_model = CDSP_SOC_SC8280XP;
		g_infra.dsp_arch = CDSP_ARCH_V68;
		g_infra.nsp_cores = CDSP_NSP_CORES;
		g_infra.vtcm_kb = CDSP_VTCM_KB;
		g_infra.dma_mask_bits = 36;   /* NSP SMMU input address width */
		g_infra_ready = 1;
	}
	return &g_infra;
}

int qnn_htp_init(qnn_backend_obj_t *b)
{
#ifdef Q8B_HTP_PRIVATE_SKEL_URI
	uint32_t handle = 0;
	int rc;
#endif

	if (!b || !b->npu_available || !b->frpc_session)
		return -1;
	(void)qnn_htp_get_infra();
#ifndef Q8B_HTP_PRIVATE_SKEL_URI
	qnn_log(QNN_LOG_LVL_WARN,
	        "htp: QAIRT V68 ABI adapter unavailable; private graph "
	        "protocol disabled\n");
	return QNN_HTP_INIT_ABI_UNAVAILABLE;
#else
	rc = frpc_open(b->frpc_session, Q8B_HTP_PRIVATE_SKEL_URI, &handle);
	if (rc != 0 || !handle) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "htp: private skel open '%s' failed (rc=%d)\n",
		        Q8B_HTP_PRIVATE_SKEL_URI, rc);
		return -1;
	}
	g_skel_handle = handle;
	g_skel_epoch = frpc_mapping_epoch();
	qnn_log(QNN_LOG_LVL_INFO, "htp: skel handle %u on session %u\n",
	        g_skel_handle, b->frpc_session);
	return 0;
#endif
}

int qnn_htp_deinit(qnn_backend_obj_t *b)
{
	int rc;
	if (!g_skel_handle) return 0;
	/* A successful CLOSE/STOP retired the remote object already. */
	if (g_skel_epoch == frpc_mapping_epoch()) {
		if (!b || !b->frpc_session) return Q8B_NPU_PAS_INVALID;
		rc = frpc_close(b->frpc_session, g_skel_handle);
		if (rc && g_skel_epoch == frpc_mapping_epoch()) return rc;
	}
	g_skel_handle = 0;
	return 0;
}

int qnn_htp_power_vote(qnn_backend_obj_t *b, QnnHtp_PerfMode_t mode)
{
	if (!b || !b->npu_available || !b->frpc_session)
		return -1;
	return frpc_power_vote(b->frpc_session, (uint32_t)mode);
}

/* Packed graph descriptor size for prepare (structure only, no data). */
static uint64_t htp_pack_size(qnn_graph_obj_t *g)
{
	uint64_t sz = 20;   /* magic, version, num_nodes, num_ins, num_outs */
	uint32_t i, j;

	for (i = 0; i < g->num_nodes; i++) {
		qnn_node_obj_t *n = &g->nodes[i];

		sz += 8 + sizeof(qnn_op_attrs_t);          /* kind, io counts + attrs */
		for (j = 0; j < n->num_ins + n->num_outs; j++) {
			qnn_tensor_obj_t *t = (j < n->num_ins) ? n->ins[j] :
			                      n->outs[j - n->num_ins];
			/* dtype, rank, dims[rank] */
			sz += 8 + (uint64_t)(t ? t->rank : 0) * 4u;
		}
	}
	return sz;
}

static void put_u32(uint8_t *p, uint64_t *o, uint32_t v)
{
	memcpy(p + *o, &v, 4);
	*o += 4;
}

static void htp_pack_tensor(uint8_t *p, uint64_t *o, qnn_tensor_obj_t *t)
{
	uint32_t i;

	put_u32(p, o, t ? (uint32_t)t->dtype : 0u);
	put_u32(p, o, t ? t->rank : 0u);
	for (i = 0; t && i < t->rank; i++)
		put_u32(p, o, t->dims[i]);
}

/* Bind a tensor to the zero-copy transport only while its QnnMem registration
 * still owns a live private FRPC_MMAP reference.  A registration can remain
 * CPU-usable after SMMU bring-up declines, so fall back to the copy form in
 * that case and let graph_run() retain its normal CPU reference fallback. */
static int htp_bind_tensor_buf(qnn_backend_obj_t *b, qnn_tensor_obj_t *t,
                               bool is_out, frpc_rbuf_t *r)
{
	qnn_shared_buf_t *sb;
	uintptr_t tensor, start, end;
	uint64_t alloc_offset;

	if (!b || !t || !t->data || !r || t->data_bytes > UINT32_MAX)
		return -1;
	memset(r, 0, sizeof(*r));
	r->size = (uint32_t)t->data_bytes;
	r->fd = -1;
	if (t->data_bytes == 0)
		return 0;
	sb = qnn_shared_buf_find(b, t->data);
	tensor = (uintptr_t)t->data;
	start = sb ? (uintptr_t)sb->host : 0;
	end = (sb && sb->size <= UINTPTR_MAX - start) ?
		start + (uintptr_t)sb->size : 0;
	if (sb && sb->devaddr && sb->pa && end && tensor >= start &&
		tensor <= end && t->data_bytes <= end - tensor) {
		alloc_offset = sb->offset + (uint64_t)(tensor - start);
		if (alloc_offset > UINT32_MAX)
			return -1;
		r->pa = sb->pa;
		r->offset = (uint32_t)alloc_offset;
		/* The DSP writes directly into a registered output buffer. */
		r->is_out = false;
		return 0;
	}
	r->data = t->data;
	r->is_out = is_out;
	return 0;
}

static void htp_pack_graph(qnn_graph_obj_t *g, uint8_t *p)
{
	uint64_t o = 0;
	uint32_t i, j;

	put_u32(p, &o, HTP_GRAPH_MAGIC);
	put_u32(p, &o, HTP_GRAPH_VERSION);
	put_u32(p, &o, g->num_nodes);
	put_u32(p, &o, g->num_graph_ins);
	put_u32(p, &o, g->num_graph_outs);
	for (i = 0; i < g->num_nodes; i++) {
		qnn_node_obj_t *n = &g->nodes[i];

		put_u32(p, &o, n->op_kind);
		put_u32(p, &o, (n->num_ins << 16) | n->num_outs);
		memcpy(p + o, &n->attrs, sizeof(n->attrs));
		o += sizeof(n->attrs);
		for (j = 0; j < n->num_ins; j++)
			htp_pack_tensor(p, &o, n->ins[j]);
		for (j = 0; j < n->num_outs; j++)
			htp_pack_tensor(p, &o, n->outs[j]);
	}
}

int qnn_htp_prepare(qnn_graph_obj_t *g)
{
	qnn_backend_obj_t *b;
	npu_buf_t desc;
	frpc_rbuf_t rbufs[2];
	uint32_t prepared = 0;
	uint64_t sz;
	uint32_t sc;
	int32_t rv = 0;
	int rc;

	if (!g || !g->ctx)
		return -1;
	b = g->ctx->backend;
	if (!b || !b->npu_available || !g_skel_handle)
		return -1;

	sz = htp_pack_size(g);
	if (sz == 0 || sz > UINT32_MAX)
		return -1;
	if (npu_buf_alloc(&desc, (uint32_t)sz) != 0)
		return -1;
	htp_pack_graph(g, (uint8_t *)desc.va);
	npu_dcache_flush(desc.va, desc.size);

	memset(rbufs, 0, sizeof(rbufs));
	/* in: graph descriptor (registered dma buffer, sent by PA) */
	rbufs[0].pa = desc.pa;
	rbufs[0].data = desc.va;
	rbufs[0].size = desc.size;
	rbufs[0].fd = -1;
	rbufs[0].is_out = false;
	/* out: prepared-graph id */
	rbufs[1].pa = 0;
	rbufs[1].data = &prepared;
	rbufs[1].size = 4;
	rbufs[1].fd = -1;
	rbufs[1].is_out = true;

	/* One input graph descriptor and one output prepared-graph id. */
	sc = frpc_scalar(HTP_M_PREPARE, 1u, 1u);
	rc = frpc_invoke(b->frpc_session, g_skel_handle, sc, rbufs, 2,
	                 NULL, 0, &rv);
	npu_buf_free(&desc);
	if (rc != 0 || rv != 0 || prepared == 0) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "htp: prepare '%s' declined (rc=%d rv=%d)\n",
		        g->name ? g->name : "?", rc, rv);
		return -1;
	}
	g->htp_prepared = prepared;
	qnn_log(QNN_LOG_LVL_INFO, "htp: graph '%s' prepared id=%u\n",
	        g->name ? g->name : "?", prepared);
	return 0;
}

int qnn_htp_execute(qnn_graph_obj_t *g, qnn_tensor_obj_t *const *ins,
                    uint32_t num_ins, qnn_tensor_obj_t *const *outs,
                    uint32_t num_outs)
{
	qnn_backend_obj_t *b;
	frpc_rbuf_t rbufs[FRPC_MAX_RBUFS];
	uint64_t prepared_arg;
	uint32_t nr = 0;
	uint32_t sc;
	int32_t rv = 0;
	uint32_t i;
	int rc;

	if (!g || !g->ctx || !g->htp_prepared)
		return -1;
	b = g->ctx->backend;
	if (!b || !b->npu_available || !g_skel_handle)
		return -1;
	if ((num_ins && !ins) || (num_outs && !outs))
		return -1;
	if (num_ins + num_outs + 1 > FRPC_MAX_RBUFS)
		return -1;

	memset(rbufs, 0, sizeof(rbufs));
	/* buffer 0: the prepared-graph id (by value). */
	prepared_arg = g->htp_prepared;
	rbufs[nr].pa = 0;
	rbufs[nr].data = &prepared_arg;
	rbufs[nr].size = 8;
	rbufs[nr].fd = -1;
	rbufs[nr].is_out = false;
	nr++;
	for (i = 0; i < num_ins; i++) {
		qnn_tensor_obj_t *t = ins[i];

		if (!t || htp_bind_tensor_buf(b, t, false, &rbufs[nr]) != 0)
			return -1;
		nr++;
	}
	for (i = 0; i < num_outs; i++) {
		qnn_tensor_obj_t *t = outs[i];

		if (!t || htp_bind_tensor_buf(b, t, true, &rbufs[nr]) != 0)
			return -1;
		nr++;
	}

	sc = frpc_scalar(HTP_M_EXECUTE, 1u + num_ins, num_outs);
	rc = frpc_invoke(b->frpc_session, g_skel_handle, sc, rbufs, nr,
	                 NULL, 0, &rv);
	if (rc != 0 || rv != 0) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "htp: execute '%s' declined (rc=%d rv=%d)\n",
		        g->name ? g->name : "?", rc, rv);
		return -1;
	}
	return 0;
}

void qnn_htp_graph_release(qnn_graph_obj_t *g)
{
	qnn_backend_obj_t *b;
	frpc_rbuf_t rbuf;
	uint64_t prepared_arg;
	uint32_t sc;
	int32_t rv = 0;

	if (!g || !g->htp_prepared || !g->ctx)
		return;
	b = g->ctx->backend;
	if (!b || !b->npu_available || !g_skel_handle) {
		g->htp_prepared = 0;
		return;
	}
	memset(&rbuf, 0, sizeof(rbuf));
	prepared_arg = g->htp_prepared;
	rbuf.pa = 0;
	rbuf.data = &prepared_arg;
	rbuf.size = 8;
	rbuf.fd = -1;
	rbuf.is_out = false;
	sc = frpc_scalar(HTP_M_RELEASE, 1u, 0u);
	(void)frpc_invoke(b->frpc_session, g_skel_handle, sc, &rbuf, 1,
	                  NULL, 0, &rv);
	g->htp_prepared = 0;
}
