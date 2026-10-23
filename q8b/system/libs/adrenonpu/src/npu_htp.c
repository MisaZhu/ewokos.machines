/*
 * npu_htp.c - HTP graph dispatch to the cDSP over the FastRPC transport.
 *
 * This is the piece the vendor libQnnHtp.so implements against the closed
 * HTP skel: after the cDSP session is up (npu_cdsp.c) it resolves the HTP
 * skel object (remote_handle64_open on "file://libqnn_htp_v68_skel.so") and
 * marshals each finalized graph to it - prepare (structure -> opaque prepared
 * id), execute (id + I/O buffers), release.  The method ids and the packed
 * graph layout below are this port's skel ABI; the invoke framing rides
 * frpc_invoke() exactly as drivers/misc/fastrpc.c expects.
 *
 * I/O tensors are passed through the transport's copy path (pa == 0), which
 * stages them in the invoke payload and copies outputs back - universally
 * correct.  The zero-copy path (registering tensor memory with QnnMem and
 * sending its PA) is the documented next optimization once the kernel
 * transport exists.  On the current kernel every entry point declines and the
 * caller (qnn_graph.c) runs the CPU reference executor instead.
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
	uint32_t handle = 0;
	int rc;

	if (!b || !b->npu_available || !b->frpc_session)
		return -1;
	(void)qnn_htp_get_infra();
	rc = frpc_open(b->frpc_session, CDSP_HTP_SKEL_URI, &handle);
	if (rc != 0) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "htp: skel open '%s' failed (rc=%d)\n",
		        CDSP_HTP_SKEL_URI, rc);
		return -1;
	}
	g_skel_handle = handle;
	qnn_log(QNN_LOG_LVL_INFO, "htp: skel handle %u on session %u\n",
	        g_skel_handle, b->frpc_session);
	return 0;
}

void qnn_htp_deinit(qnn_backend_obj_t *b)
{
	if (b && b->frpc_session && g_skel_handle)
		frpc_close(b->frpc_session, g_skel_handle);
	g_skel_handle = 0;
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

	/* sc: method id in the attrs field, 1 return, 2 buffers. */
	sc = frpc_scalar(HTP_M_PREPARE, 1u, 0u, 2u);
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
	if (num_ins + num_outs + 1 > FRPC_MAX_RBUFS)
		return -1;

	memset(rbufs, 0, sizeof(rbufs));
	/* buffer 0: the prepared-graph id (by value). */
	rbufs[nr].pa = 0;
	rbufs[nr].data = &g->htp_prepared;
	rbufs[nr].size = 8;
	rbufs[nr].fd = -1;
	rbufs[nr].is_out = false;
	nr++;
	for (i = 0; i < num_ins; i++) {
		qnn_tensor_obj_t *t = ins[i];

		if (!t || !t->data)
			return -1;
		rbufs[nr].pa = 0;
		rbufs[nr].data = t->data;
		rbufs[nr].size = (uint32_t)t->data_bytes;
		rbufs[nr].fd = -1;
		rbufs[nr].is_out = false;
		nr++;
	}
	for (i = 0; i < num_outs; i++) {
		qnn_tensor_obj_t *t = outs[i];

		if (!t || !t->data)
			return -1;
		rbufs[nr].pa = 0;
		rbufs[nr].data = t->data;
		rbufs[nr].size = (uint32_t)t->data_bytes;
		rbufs[nr].fd = -1;
		rbufs[nr].is_out = true;   /* copied back by frpc_invoke */
		nr++;
	}

	sc = frpc_scalar(HTP_M_EXECUTE, 1u, 0u, nr);
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
	rbuf.pa = 0;
	rbuf.data = &g->htp_prepared;
	rbuf.size = 8;
	rbuf.fd = -1;
	rbuf.is_out = false;
	sc = frpc_scalar(HTP_M_RELEASE, 1u, 0u, 1u);
	(void)frpc_invoke(b->frpc_session, g_skel_handle, sc, &rbuf, 1,
	                  NULL, 0, &rv);
	g->htp_prepared = 0;
}
