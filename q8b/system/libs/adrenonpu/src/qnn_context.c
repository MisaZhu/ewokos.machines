/*
 * qnn_context.c - QnnContext_* implementation plus the "QNNB" v1 context
 * binary (de)serializer behind getBinary / createFromBinary / validateBinary.
 * QNNB is this port's local format, not an official QAIRT V68 context. The
 * latter uses the explicit htp_native mode through the npu_model adapter.
 *
 * The vendor HTP backend serializes a finalized context (its prepared graphs
 * and baked STATIC weights) into an opaque blob that a later process can load
 * without re-running graph preparation on the cDSP.  This port reproduces that
 * round-trip with a small self-describing format: it is produced and consumed
 * on the same little-endian aarch64 target, so records are stored in native
 * byte order (documented, not wire-portable - the vendor blob is likewise
 * backend- and arch-specific).
 *
 * QNNB v1 layout (all integers native endian):
 *   header (64 bytes)
 *     [0]  char[4]  magic "QNNB"
 *     [4]  u32      version (1)
 *     [8]  u32      flags (0)
 *     [12] u32      num_tensors   (context-wide pool)
 *     [16] u32      num_graphs
 *     [20] u64      total_size    (== byte length of the whole blob)
 *     [28] u32      soc_model
 *     [32] u32      dsp_arch
 *     [36] u32      htp.perf_mode
 *     [40] u64      htp.vtcm_limit
 *     [48] u32      htp.concurrent_group
 *     [52] u32      htp.dsp_core_mask
 *     [56] u32      htp.precision
 *     [60] u32      htp.skip_flush
 *   tensor record * num_tensors
 *     str name; u32 ttype; u32 dtype; u32 rank; u32 dims[rank];
 *     f32 scale; u32 zeroPoint; u64 data_bytes; (pad8 + data_bytes) if any
 *   graph record * num_graphs
 *     str name; u32 num_in + in idx[]; u32 num_out + out idx[];
 *     u32 num_nodes + node record[]; graph htp state; u32 finalized
 *   node record
 *     str op_name; u32 op_kind; u32 num_ins + idx[]; u32 num_outs + idx[];
 *     qnn_op_attrs_t attrs (fixed struct)
 * A "str" is u32 length followed by that many bytes (no NUL).  Tensor indices
 * are positions in the context tensor pool, resolved on load.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <stdlib.h>
#include <string.h>

#define QNN_CTX_MAGIC0   'Q'
#define QNN_CTX_MAGIC1   'N'
#define QNN_CTX_MAGIC2   'N'
#define QNN_CTX_MAGIC3   'B'
#define QNN_CTX_VERSION  1u
#define QNN_CTX_HDR_SIZE 64u
#define QNN_CTX_TSZ_OFF  20u   /* offset of the u64 total_size field */

/* ------------------------------------------------------------------ */
/* growable writer                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
	uint8_t *p;
	uint64_t len;
	uint64_t cap;
	int      oom;
} bw_t;

static void bw_init(bw_t *w) { w->p = NULL; w->len = 0; w->cap = 0; w->oom = 0; }

static int bw_reserve(bw_t *w, uint64_t add)
{
	uint64_t nc;
	uint8_t *np;

	if (w->oom)
		return -1;
	if (w->len + add <= w->cap)
		return 0;
	nc = w->cap ? w->cap * 2 : 2048;
	while (nc < w->len + add)
		nc *= 2;
	np = (uint8_t *)realloc(w->p, (size_t)nc);
	if (!np) {
		w->oom = 1;
		return -1;
	}
	w->p = np;
	w->cap = nc;
	return 0;
}

static void bw_bytes(bw_t *w, const void *src, uint64_t n)
{
	if (n == 0)
		return;
	if (bw_reserve(w, n))
		return;
	memcpy(w->p + w->len, src, (size_t)n);
	w->len += n;
}
static void bw_u32(bw_t *w, uint32_t v) { bw_bytes(w, &v, 4); }
static void bw_u64(bw_t *w, uint64_t v) { bw_bytes(w, &v, 8); }
static void bw_f32(bw_t *w, float v)    { bw_bytes(w, &v, 4); }

static void bw_str(bw_t *w, const char *s)
{
	uint32_t n = s ? (uint32_t)strlen(s) : 0u;

	bw_u32(w, n);
	if (n)
		bw_bytes(w, s, n);
}

/* Pad the stream so the next payload starts on an `align` boundary. */
static void bw_pad(bw_t *w, uint32_t align)
{
	uint32_t m = (uint32_t)(w->len % align);
	uint8_t z[8] = { 0 };

	if (m)
		bw_bytes(w, z, align - m);
}

/* ------------------------------------------------------------------ */
/* bounds-checked reader                                               */
/* ------------------------------------------------------------------ */
typedef struct {
	const uint8_t *p;
	uint64_t len;
	uint64_t off;
	int      err;
} br_t;

static void br_init(br_t *r, const uint8_t *p, uint64_t len)
{
	r->p = p; r->len = len; r->off = 0; r->err = 0;
}

static int br_need(br_t *r, uint64_t n)
{
	if (r->err)
		return -1;
	if (r->off > r->len || n > r->len - r->off) {
		r->err = 1;
		return -1;
	}
	return 0;
}

static uint32_t br_u32(br_t *r)
{
	uint32_t v = 0;

	if (br_need(r, 4))
		return 0;
	memcpy(&v, r->p + r->off, 4);
	r->off += 4;
	return v;
}

static uint64_t br_u64(br_t *r)
{
	uint64_t v = 0;

	if (br_need(r, 8))
		return 0;
	memcpy(&v, r->p + r->off, 8);
	r->off += 8;
	return v;
}

static float br_f32(br_t *r)
{
	float v = 0.0f;

	if (br_need(r, 4))
		return 0.0f;
	memcpy(&v, r->p + r->off, 4);
	r->off += 4;
	return v;
}

static const uint8_t *br_bytes(br_t *r, uint64_t n)
{
	const uint8_t *q;

	if (n == 0)
		return r->p + r->off;
	if (br_need(r, n))
		return NULL;
	q = r->p + r->off;
	r->off += n;
	return q;
}

/* Read a length-prefixed string; returns a malloc'd NUL-terminated copy. */
static char *br_str(br_t *r)
{
	uint32_t n = br_u32(r);
	const uint8_t *q;
	char *s;

	if (r->err || n > 0x10000u) {
		r->err = 1;
		return NULL;
	}
	q = br_bytes(r, n);
	if ((!q && n) || (n && memchr(q, '\0', n))) {
		r->err = 1;
		return NULL;
	}
	s = (char *)malloc((size_t)n + 1);
	if (!s) {
		r->err = 1;
		return NULL;
	}
	if (n)
		memcpy(s, q, n);
	s[n] = '\0';
	return s;
}

static void br_skip_pad(br_t *r, uint32_t align)
{
	uint32_t m = (uint32_t)(r->off % align);

	if (m && br_need(r, align - m) == 0)
		r->off += align - m;
}

/* ------------------------------------------------------------------ */
/* header (de)serialization                                            */
/* ------------------------------------------------------------------ */
static void bw_header(bw_t *w, qnn_context_obj_t *c)
{
	uint32_t soc = c->backend ? c->backend->soc_model : CDSP_SOC_SC8280XP;
	uint32_t arch = c->backend ? c->backend->dsp_arch : CDSP_ARCH_V68;

	bw_bytes(w, "QNNB", 4);
	bw_u32(w, QNN_CTX_VERSION);
	bw_u32(w, 0u);                 /* flags */
	bw_u32(w, c->num_tensors);
	bw_u32(w, c->num_graphs);
	bw_u64(w, 0u);                 /* total_size, patched at the end */
	bw_u32(w, soc);
	bw_u32(w, arch);
	bw_u32(w, (uint32_t)c->htp.perf_mode);
	bw_u64(w, c->htp.vtcm_limit);
	bw_u32(w, c->htp.concurrent_group);
	bw_u32(w, c->htp.dsp_core_mask);
	bw_u32(w, (uint32_t)c->htp.precision);
	bw_u32(w, c->htp.skip_flush ? 1u : 0u);
}

static void br_header(br_t *r, uint32_t *nt, uint32_t *ng, qnn_htp_state_t *h)
{
	br_bytes(r, 4);                  /* magic (validated by caller) */
	br_u32(r);                       /* version */
	br_u32(r);                       /* flags   */
	*nt = br_u32(r);
	*ng = br_u32(r);
	br_u64(r);                       /* total_size */
	br_u32(r);                       /* soc_model  */
	br_u32(r);                       /* dsp_arch   */
	memset(h, 0, sizeof(*h));
	h->perf_mode = (QnnHtp_PerfMode_t)br_u32(r);
	h->vtcm_limit = br_u64(r);
	h->concurrent_group = br_u32(r);
	h->dsp_core_mask = br_u32(r);
	h->precision = (QnnHtp_PrecisionHint_t)br_u32(r);
	h->skip_flush = br_u32(r) ? true : false;
	h->hw_counters = false;
}

/* Index of tensor `t` in the context pool, or -1. */
static int32_t tensor_index(qnn_context_obj_t *c, qnn_tensor_obj_t *t)
{
	uint32_t i;

	for (i = 0; i < c->num_tensors; i++) {
		if (c->tensors[i] == t)
			return (int32_t)i;
	}
	return -1;
}

/* Weights (STATIC tensors) are baked into the binary; runtime APP_WRITE /
   APP_READ activations are supplied at execute time and left out. */
static bool tensor_has_persistent_data(const qnn_tensor_obj_t *t)
{
	return t->ttype == QNN_TENSOR_TYPE_STATIC && t->data &&
	       t->data_bytes > 0;
}

uint8_t *qnn_ctx_serialize(qnn_context_obj_t *c, uint64_t *out_size)
{
	bw_t w;
	uint64_t total;
	uint32_t i, j, k;

	if (!c || !out_size || (c->backend && c->backend->native_mode))
		return NULL;
	bw_init(&w);
	bw_header(&w, c);

	for (i = 0; i < c->num_tensors; i++) {
		qnn_tensor_obj_t *t = c->tensors[i];
		bool has = tensor_has_persistent_data(t);

		bw_str(&w, t->name ? t->name : "");
		bw_u32(&w, (uint32_t)t->ttype);
		bw_u32(&w, (uint32_t)t->dtype);
		bw_u32(&w, t->rank);
		for (j = 0; j < t->rank && j < 8u; j++)
			bw_u32(&w, t->dims[j]);
		bw_f32(&w, t->quant.scale);
		bw_u32(&w, (uint32_t)t->quant.zeroPoint);
		bw_u64(&w, has ? t->data_bytes : 0u);
		if (has) {
			bw_pad(&w, 8);
			bw_bytes(&w, t->data, t->data_bytes);
		}
	}

	for (i = 0; i < c->num_graphs; i++) {
		qnn_graph_obj_t *g = c->graphs[i];

		bw_str(&w, g->name ? g->name : "");
		bw_u32(&w, g->num_graph_ins);
		for (j = 0; j < g->num_graph_ins; j++)
			bw_u32(&w, (uint32_t)tensor_index(c, g->graph_ins[j]));
		bw_u32(&w, g->num_graph_outs);
		for (j = 0; j < g->num_graph_outs; j++)
			bw_u32(&w, (uint32_t)tensor_index(c, g->graph_outs[j]));
		bw_u32(&w, g->num_nodes);
		for (j = 0; j < g->num_nodes; j++) {
			qnn_node_obj_t *n = &g->nodes[j];

			bw_str(&w, n->op_name ? n->op_name : "");
			bw_u32(&w, n->op_kind);
			bw_u32(&w, n->num_ins);
			for (k = 0; k < n->num_ins; k++)
				bw_u32(&w,
				       (uint32_t)tensor_index(c, n->ins[k]));
			bw_u32(&w, n->num_outs);
			for (k = 0; k < n->num_outs; k++)
				bw_u32(&w,
				       (uint32_t)tensor_index(c, n->outs[k]));
			bw_bytes(&w, &n->attrs, sizeof(n->attrs));
		}
		bw_u32(&w, (uint32_t)g->htp.perf_mode);
		bw_u64(&w, g->htp.vtcm_limit);
		bw_u32(&w, (uint32_t)g->htp.precision);
		bw_u32(&w, g->htp.skip_flush ? 1u : 0u);
		bw_u32(&w, g->finalized ? 1u : 0u);
	}

	if (w.oom) {
		free(w.p);
		return NULL;
	}
	total = w.len;
	memcpy(w.p + QNN_CTX_TSZ_OFF, &total, 8);
	*out_size = w.len;
	return w.p;
}

/* Walk one graph record.  When `c` is non-NULL the graph is materialized into
   it (pool[] maps this binary's tensor indices to live objects); when NULL the
   record is only validated.  Returns QNN_SUCCESS / QNN_ERROR. */
static int ctx_walk_graph(br_t *r, qnn_context_obj_t *c,
                          qnn_tensor_obj_t *const *pool,
                          const uint8_t *types, uint32_t npool)
{
	char *gname = br_str(r);
	qnn_graph_obj_t *g = NULL;
	uint32_t nin, nout, nnodes, j, k;
	qnn_htp_state_t h = {0};
	uint32_t skip, finalized;

	if (r->err) { free(gname); return QNN_ERROR; }
	if (c) {
		g = qnn_graph_obj_create(c, gname);
		if (!g) { free(gname); return QNN_ERROR; }
	}
	free(gname);

	nin = br_u32(r);
	if (nin > npool) r->err = 1;
	for (j = 0; j < nin && !r->err; j++) {
		uint32_t idx = br_u32(r);
		if (r->err || idx >= npool ||
		    (types[idx] != QNN_TENSOR_TYPE_APP_WRITE &&
		     types[idx] != QNN_TENSOR_TYPE_READWRITE)) {
			r->err = 1;
			break;
		}
		if (c && qnn_graph_add_boundary(g, pool[idx]) != QNN_SUCCESS)
			r->err = 1;
	}
	nout = br_u32(r);
	if (nout > npool) r->err = 1;
	for (j = 0; j < nout && !r->err; j++) {
		uint32_t idx = br_u32(r);
		if (r->err || idx >= npool ||
		    (types[idx] != QNN_TENSOR_TYPE_APP_READ &&
		     types[idx] != QNN_TENSOR_TYPE_READWRITE)) {
			r->err = 1;
			break;
		}
		if (c && qnn_graph_add_boundary(g, pool[idx]) != QNN_SUCCESS)
			r->err = 1;
	}

	nnodes = br_u32(r);
	for (j = 0; j < nnodes && !r->err; j++) {
		char *op = br_str(r);
		uint32_t kind = br_u32(r), nins = br_u32(r), nouts;
		qnn_tensor_obj_t *ins[QNN_NODE_MAX_INPUTS] = {0};
		qnn_tensor_obj_t *outs[QNN_NODE_MAX_OUTPUTS] = {0};
		qnn_op_attrs_t attrs;
		const uint8_t *ab;
		const qnn_op_info_t *info = qnn_op_lookup(op);

		if (!info || kind != info->kind || nins < info->min_ins ||
		    nins > info->max_ins || nins > QNN_NODE_MAX_INPUTS) r->err = 1;
		for (k = 0; k < nins && !r->err; k++) {
			uint32_t idx = br_u32(r);
			if (idx >= npool) r->err = 1;
			else if (c) ins[k] = pool[idx];
		}
		nouts = br_u32(r);
		if (info && (nouts < info->min_outs || nouts > info->max_outs ||
		             nouts > QNN_NODE_MAX_OUTPUTS)) r->err = 1;
		for (k = 0; k < nouts && !r->err; k++) {
			uint32_t idx = br_u32(r);
			if (idx >= npool) r->err = 1;
			else if (c) outs[k] = pool[idx];
		}
		ab = br_bytes(r, sizeof(attrs));
		if (ab) memcpy(&attrs, ab, sizeof(attrs));
		if (r->err) { free(op); return QNN_ERROR; }
		if (c && !qnn_graph_node_append(g, op, kind, &attrs, ins, nins, outs, nouts)) {
			free(op);
			return QNN_ERROR;
		}
		free(op);
	}

	h.perf_mode = (QnnHtp_PerfMode_t)br_u32(r);
	h.vtcm_limit = br_u64(r);
	h.precision = (QnnHtp_PrecisionHint_t)br_u32(r);
	skip = br_u32(r);
	finalized = br_u32(r);
	if ((unsigned)h.perf_mode > QNN_HTP_PERF_MODE_TURBO ||
	    (unsigned)h.precision > QNN_HTP_PRECISION_STRICT || skip > 1 || finalized > 1)
		r->err = 1;
	if (r->err) return QNN_ERROR;
	if (c && g) {
		g->from_binary = true;
		g->htp.perf_mode = h.perf_mode;
		g->htp.vtcm_limit = h.vtcm_limit;
		g->htp.precision = h.precision;
		g->htp.skip_flush = skip != 0;
		g->finalized = finalized != 0;
	}
	return QNN_SUCCESS;
}

/* One parser enforces the same format on validation, single and list load.
 * Materialization failures leave partial objects owned by the caller's
 * context, which is destroyed before returning an error. */
static int ctx_load(qnn_context_obj_t *c, const uint8_t *buf, uint64_t size)
{
	br_t r;
	qnn_htp_state_t h;
	uint32_t nt = 0, ng = 0, i, version, flags;
	uint64_t total;
	qnn_tensor_obj_t **pool = NULL;
	uint8_t *types = NULL;

	if (!buf || size < QNN_CTX_HDR_SIZE || size > SIZE_MAX) return QNN_ERROR;
	if (memcmp(buf, "QNNB", 4)) return QNN_ERROR;
	memcpy(&version, buf + 4, 4);
	memcpy(&flags, buf + 8, 4);
	memcpy(&total, buf + QNN_CTX_TSZ_OFF, 8);
	if (version != QNN_CTX_VERSION || flags != 0 || total != size) return QNN_ERROR;
	br_init(&r, buf, size);
	br_header(&r, &nt, &ng, &h);
	if (r.err || nt > 0x100000u || ng > 0x10000u ||
	    nt > (size - QNN_CTX_HDR_SIZE) / 32u ||
	    ng > (size - QNN_CTX_HDR_SIZE) / 40u) return QNN_ERROR;
	if (nt) {
		if (c) pool = (qnn_tensor_obj_t **)calloc(nt, sizeof(*pool));
		types = (uint8_t *)calloc(nt, sizeof(*types));
		if ((c && !pool) || !types) { free(pool); free(types); return QNN_ERROR; }
	}
	for (i = 0; i < nt && !r.err; i++) {
		char *name = br_str(&r);
		uint32_t ttype = br_u32(&r), dtype = br_u32(&r), rank = br_u32(&r);
		uint32_t dims[8] = {0}, d;
		float scale;
		int32_t zp;
		uint64_t data_bytes, tensor_bytes = qnn_dtype_size((Qnn_DataType_t)dtype);
		if (ttype > QNN_TENSOR_TYPE_READWRITE || !tensor_bytes || rank > 8u) r.err = 1;
		for (d = 0; d < rank && !r.err; d++) {
			dims[d] = br_u32(&r);
			if (!dims[d] || tensor_bytes > SIZE_MAX / dims[d]) r.err = 1;
			else tensor_bytes *= dims[d];
		}
		scale = br_f32(&r);
		zp = (int32_t)br_u32(&r);
		data_bytes = br_u64(&r);
		if (data_bytes && (ttype != QNN_TENSOR_TYPE_STATIC || data_bytes != tensor_bytes))
			r.err = 1;
		if (r.err) { free(name); break; }
		if (c) {
			Qnn_Shape_t shape = {rank, dims};
			Qnn_QuantParams_t qp = {scale, zp};
			pool[i] = qnn_tensor_create_common(c, NULL, name,
			                (Qnn_TensorType_t)ttype, (Qnn_DataType_t)dtype, &shape, &qp);
			if (!pool[i]) r.err = 1;
		}
		types[i] = (uint8_t)ttype;
		free(name);
		if (data_bytes) {
			const uint8_t *dp;
			br_skip_pad(&r, 8);
			dp = br_bytes(&r, data_bytes);
			if (c && !r.err) {
				if (qnn_tensor_reserve_data(pool[i], data_bytes) != QNN_SUCCESS) r.err = 1;
				else memcpy(pool[i]->data, dp, (size_t)data_bytes);
			}
		}
	}
	if (c && !r.err) c->htp = h;
	for (i = 0; i < ng && !r.err; i++) {
		if (ctx_walk_graph(&r, c, pool, types, nt) != QNN_SUCCESS) r.err = 1;
	}
	free(pool);
	free(types);
	if (c && !r.err) {
		c->finalized = true;
		for (i = 0; i < c->num_graphs; i++)
			if (!c->graphs[i]->finalized) c->finalized = false;
	}
	return r.err || r.off != r.len ? QNN_ERROR : QNN_SUCCESS;
}

bool qnn_ctx_binary_validate(const uint8_t *buf, uint64_t size)
{
	return ctx_load(NULL, buf, size) == QNN_SUCCESS;
}

qnn_context_obj_t *qnn_ctx_deserialize(qnn_backend_obj_t *b,
        qnn_device_obj_t *dev, const uint8_t *buf, uint64_t size)
{
	qnn_context_obj_t *c;

	if (!b || !qnn_ctx_binary_validate(buf, size))
		return NULL;
	c = (qnn_context_obj_t *)qnn_obj_alloc(QNN_OBJ_CONTEXT, sizeof(*c));
	if (!c)
		return NULL;
	c->backend = b;
	c->device = dev;
	c->htp = b->htp_defaults;
	c->finalized = true;
	if (ctx_load(c, buf, size) != QNN_SUCCESS) {
		/* Partial load: tear down whatever was built. */
		QnnContext_free((Qnn_ContextHandle_t)(uintptr_t)c);
		return NULL;
	}
	return c;
}

/* ------------------------------------------------------------------ */
/* QnnContext_* API                                                    */
/* ------------------------------------------------------------------ */
static qnn_context_obj_t *ctx_alloc_common(qnn_backend_obj_t *b,
        qnn_device_obj_t *dev, const Qnn_ConfigParam_t *cfg, uint32_t nCfg)
{
	qnn_context_obj_t *c;

	c = (qnn_context_obj_t *)qnn_obj_alloc(QNN_OBJ_CONTEXT, sizeof(*c));
	if (!c)
		return NULL;
	c->backend = b;
	c->device = dev;
	/* Inherit tuning: device overrides backend defaults. */
	c->htp = dev ? dev->htp : b->htp_defaults;
	if (qnn_cfg_store(c->config_keys, c->config_vals, &c->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS) {
		qnn_cfg_free(c->config_keys, c->config_vals, &c->num_config);
		qnn_obj_free(c);
		return NULL;
	}
	return c;
}

Qnn_ErrorHandle_t QnnContext_create(Qnn_BackendHandle_t backend,
                                    Qnn_DeviceHandle_t dev,
                                    const Qnn_ConfigParam_t *cfg,
                                    uint32_t nCfg, Qnn_ContextHandle_t *ctx)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_device_obj_t *d = dev ? (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE) : NULL;
	qnn_context_obj_t *c;

	if (!b || !ctx || b->native_mode)
		return QNN_ERROR;
	if (dev && (!d || d->backend != b))
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;

	c = ctx_alloc_common(b, d, cfg, nCfg);
	if (!c)
		return QNN_ERROR;
	c->htp.perf_mode = (QnnHtp_PerfMode_t)qnn_cfg_get_int(cfg, nCfg,
	                "perf_mode", (int)c->htp.perf_mode);
	c->htp.vtcm_limit = (uint64_t)qnn_cfg_get_int(cfg, nCfg, "vtcm_limit",
	                (int)c->htp.vtcm_limit);
	*ctx = (Qnn_ContextHandle_t)(uintptr_t)c;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_createFromBinary(Qnn_BackendHandle_t backend,
        Qnn_DeviceHandle_t dev, const uint8_t *binBuf, uint64_t binSize,
        const Qnn_ConfigParam_t *cfg, uint32_t nCfg, Qnn_ContextHandle_t *ctx)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_device_obj_t *d = dev ? (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE) : NULL;
	qnn_context_obj_t *c;

	if (!b || !binBuf || !ctx || (dev && (!d || d->backend != b)))
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;

	if (b->native_mode) {
		for (uint32_t i = 0; i < nCfg; i++)
			if (!cfg[i].key || strcmp(cfg[i].key, "graph_name")) return QNN_ERROR;
		c = ctx_alloc_common(b, d, cfg, nCfg);
		if (!c) return QNN_ERROR;
		const char *name = qnn_cfg_get(cfg, nCfg, "graph_name");
		if (qnn_native_context_load(c, binBuf, binSize, name ? name : "")) {
			QnnContext_free((Qnn_ContextHandle_t)(uintptr_t)c);
			return QNN_ERROR;
		}
		*ctx = (Qnn_ContextHandle_t)(uintptr_t)c;
		return QNN_SUCCESS;
	}
	c = qnn_ctx_deserialize(b, d, binBuf, binSize);
	if (!c)
		return QNN_ERROR;
	/* Config supplied at load time still tunes the live context. */
	if (qnn_cfg_store(c->config_keys, c->config_vals, &c->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS) {
		QnnContext_free((Qnn_ContextHandle_t)(uintptr_t)c);
		return QNN_ERROR;
	}
	*ctx = (Qnn_ContextHandle_t)(uintptr_t)c;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_createFromBinaryWithSignal(Qnn_BackendHandle_t backend,
        Qnn_DeviceHandle_t dev, const uint8_t *binBuf, uint64_t binSize,
        const Qnn_ConfigParam_t *cfg, uint32_t nCfg, Qnn_SignalHandle_t sig,
        Qnn_ContextHandle_t *ctx)
{
	Qnn_ErrorHandle_t rc = QnnContext_createFromBinary(backend, dev, binBuf,
	                                   binSize, cfg, nCfg, ctx);

	/* The synchronous load completes before this wrapper returns; the signal is therefore already complete on return,
	   matching the vendor contract that the signal fires when load ends. */
	qnn_signal_mark(sig, rc == QNN_SUCCESS ? 1 : 2);
	return rc;
}

Qnn_ErrorHandle_t QnnContext_createFromBinaryWithCallback(
        Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
        const uint8_t *binBuf, uint64_t binSize, const Qnn_ConfigParam_t *cfg,
        uint32_t nCfg, void (*cb)(Qnn_ErrorHandle_t, void *), void *cbCtx,
        Qnn_ContextHandle_t *ctx)
{
	Qnn_ErrorHandle_t rc = QnnContext_createFromBinary(backend, dev, binBuf,
	                                   binSize, cfg, nCfg, ctx);

	if (cb)
		cb(rc, cbCtx);
	return rc;
}

Qnn_ErrorHandle_t QnnContext_createFromBinaryListAsync(
        Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t dev,
        const uint8_t **binBufs, const uint64_t *binSizes, uint32_t numBin,
        const Qnn_ConfigParam_t *cfg, uint32_t nCfg, Qnn_SignalHandle_t sig,
        Qnn_ContextHandle_t *ctx)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_device_obj_t *d = dev ? (qnn_device_obj_t *)
		qnn_obj_from_handle(dev, QNN_OBJ_DEVICE) : NULL;
	qnn_context_obj_t *c;
	uint32_t i;
	int rc = QNN_SUCCESS;

	if (!b || !binBufs || !binSizes || numBin == 0 || !ctx) {
		qnn_signal_mark(sig, 2);
		return QNN_ERROR;
	}
	if (dev && (!d || d->backend != b)) {
		qnn_signal_mark(sig, 2);
		return QNN_ERROR;
	}

	if (b->native_mode) {
		if (numBin != 1) { qnn_signal_mark(sig, 2); return QNN_ERROR; }
		return QnnContext_createFromBinaryWithSignal(backend, dev, binBufs[0],
			binSizes[0], cfg, nCfg, sig, ctx);
	}
	c = ctx_alloc_common(b, d, cfg, nCfg);
	if (!c) {
		qnn_signal_mark(sig, 2);
		return QNN_ERROR;
	}
	/* This local list API merges each QNNB into one context. Tensor indices
	 * stay relative to their source binary; it is not the QAIRT list ABI. */
	for (i = 0; i < numBin; i++) {
		if (!binBufs[i] ||
		    ctx_load(c, binBufs[i], binSizes[i]) != QNN_SUCCESS) {
			rc = QNN_ERROR;
			break;
		}
	}
	if (rc != QNN_SUCCESS) {
		QnnContext_free((Qnn_ContextHandle_t)(uintptr_t)c);
		qnn_signal_mark(sig, 2);
		return QNN_ERROR;
	}
	*ctx = (Qnn_ContextHandle_t)(uintptr_t)c;
	qnn_signal_mark(sig, 1);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_finalize(Qnn_ContextHandle_t ctx)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint32_t i;

	if (!c)
		return QNN_ERROR;
	/* Prepare every finalized graph: HTP dispatch when the cDSP is up,
	   otherwise the CPU reference plan is built lazily at execute. */
	for (i = 0; i < c->num_graphs; i++) {
		qnn_graph_obj_t *g = c->graphs[i];

		if (!g->finalized) {
			if (QnnGraph_finalize((Qnn_GraphHandle_t)(uintptr_t)g) != QNN_SUCCESS)
				return QNN_ERROR;
		}
		else if (c->backend && !c->backend->native_mode && c->backend->npu_available)
			qnn_htp_prepare(g);
	}
	c->finalized = true;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_free(Qnn_ContextHandle_t ctx)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint32_t i;

	if (!c)
		return QNN_ERROR;
	if (c->backend && c->backend->native_mode && qnn_native_context_release(c))
		return QNN_ERROR;
	for (i = 0; i < c->num_graphs; i++) {
		qnn_graph_obj_t *g = c->graphs[i];

		if (!g)
			continue;
		if (c->backend && !c->backend->native_mode && c->backend->npu_available)
			qnn_htp_graph_release(g);
		if (g->ref)
			qnn_ref_free(g->ref);
		qnn_graph_release_internals(g);
		qnn_obj_free(g);
	}
	for (i = 0; i < c->num_tensors; i++) {
		qnn_tensor_obj_t *t = c->tensors[i];

		if (!t)
			continue;
		if (t->data_owned)
			free(t->data);
		free(t->name);
		qnn_obj_free(t);
	}
	free(c->graphs);
	free(c->tensors);
	qnn_cfg_free(c->config_keys, c->config_vals, &c->num_config);
	qnn_obj_free(c);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_setConfig(Qnn_ContextHandle_t ctx,
                                       const Qnn_ConfigParam_t *cfg,
                                       uint32_t nCfg)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);

	if (!c || (c->backend && c->backend->native_mode))
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;
	qnn_cfg_free(c->config_keys, c->config_vals, &c->num_config);
	if (qnn_cfg_store(c->config_keys, c->config_vals, &c->num_config,
	                  cfg, nCfg, 8) != QNN_SUCCESS)
		return QNN_ERROR;
	c->htp.perf_mode = (QnnHtp_PerfMode_t)qnn_cfg_get_int(cfg, nCfg,
	                "perf_mode", (int)c->htp.perf_mode);
	c->htp.vtcm_limit = (uint64_t)qnn_cfg_get_int(cfg, nCfg, "vtcm_limit",
	                (int)c->htp.vtcm_limit);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_getProperty(Qnn_ContextHandle_t ctx,
                                         const char *key, const char **val)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);

	if (!c || !key || !val)
		return QNN_ERROR;
	if (strcmp(key, "num_graphs") == 0)
		*val = qnn_fmt_static("%u", c->num_graphs);
	else if (strcmp(key, "num_tensors") == 0)
		*val = qnn_fmt_static("%u", c->num_tensors);
	else if (strcmp(key, "finalized") == 0)
		*val = c->finalized ? "true" : "false";
	else if (strcmp(key, "backend.id") == 0)
		*val = "HTP";
	else if (strcmp(key, "soc.model") == 0)
		*val = qnn_fmt_static("%u",
		              c->backend ? c->backend->soc_model : 0u);
	else if (strcmp(key, "perf_mode") == 0)
		*val = qnn_fmt_static("%u", (unsigned)c->htp.perf_mode);
	else if (strcmp(key, "vtcm_limit") == 0)
		*val = qnn_fmt_static("%llu",
		              (unsigned long long)c->htp.vtcm_limit);
	else if (strcmp(key, "binary.format") == 0)
		*val = c->native ? "official-opaque" : "QNNB";
	else if (strcmp(key, "binary.version") == 0 && !c->native)
		*val = qnn_fmt_static("%u", QNN_CTX_VERSION);
	else
		return QNN_ERROR;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_getBinarySize(Qnn_ContextHandle_t ctx,
                                           uint64_t *outSize)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint8_t *blob;
	uint64_t sz = 0;

	if (!c || !outSize)
		return QNN_ERROR;
	if (c->native) return qnn_native_binary(c, NULL, 0, outSize);
	blob = qnn_ctx_serialize(c, &sz);
	if (!blob)
		return QNN_ERROR;
	free(blob);
	*outSize = sz;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_getBinary(Qnn_ContextHandle_t ctx, uint8_t *buf,
                                       uint64_t bufLen)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint8_t *blob;
	uint64_t sz = 0;

	if (!c || !buf)
		return QNN_ERROR;
	if (c->native) return qnn_native_binary(c, buf, bufLen, &sz);
	blob = qnn_ctx_serialize(c, &sz);
	if (!blob)
		return QNN_ERROR;
	if (bufLen < sz) {
		free(blob);
		return QNN_ERROR;
	}
	memcpy(buf, blob, (size_t)sz);
	free(blob);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnContext_validateBinary(Qnn_BackendHandle_t backend,
        Qnn_DeviceHandle_t dev, const uint8_t *binBuf, uint64_t binSize,
        bool *valid)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	(void)dev;
	if (!b || !valid)
		return QNN_ERROR;
	*valid = false;
	if (b->native_mode) return QNN_ERROR; /* SDK validation occurs during load. */
	*valid = qnn_ctx_binary_validate(binBuf, binSize);
	return QNN_SUCCESS;
}
