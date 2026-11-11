/*
 * npu_ref_exec.c - in-process CPU reference executor.
 *
 * This is the role the vendor SDK's libQnnCpu.so plays: a portable software
 * implementation of the op package so a graph built through the QNN C API is
 * runnable without the cDSP.  qnn_graph.c uses it whenever the HTP transport
 * is unavailable or as a fallback if an
 * HTP execute declines.
 *
 * Conventions:
 *   - Compute is done in float: inputs are dequantized through their
 *     Qnn_QuantParams_t, results are requantized on store, so both float and
 *     quantized graphs work through the same kernels.
 *   - 4-D spatial tensors (Conv2d, Pool) are NHWC, the HTP backend's native
 *     layout; Conv2d weights are [KH, KW, Cin, Cout].
 *   - Elementwise ops broadcast NumPy-style over trailing dims.
 *   - Intermediate tensors the client never bound get an owned scratch buffer
 *     allocated at plan build time and freed with the plan.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define REF_MAX_DIM 8

typedef struct {
	qnn_tensor_obj_t *t;
	void             *buf;
	uint64_t          bytes;
} ref_slot_t;

struct qnn_ref_exec {
	ref_slot_t *slots;
	uint32_t    num_slots;
	uint32_t    slot_cap;
};

/* ------------------------------------------------------------------ */
/* half <-> float                                                      */
/* ------------------------------------------------------------------ */
static float half_to_float(uint16_t h)
{
	uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t man = h & 0x3ffu;
	uint32_t f;

	if (exp == 0) {
		if (man == 0) {
			f = sign;
		} else {
			/* Subnormal: value = (man/1024) * 2^-14; take the
			   magnitude bits from the computed float and re-apply
			   the half's sign. */
			float m = (float)man / 1024.0f;
			float v = m * powf(2.0f, -14.0f);
			uint32_t tmp;

			memcpy(&tmp, &v, 4);
			f = sign | (tmp & 0x7fffffffu);
		}
	} else if (exp == 31) {
		f = sign | 0x7f800000u | (man << 13);
	} else {
		f = sign | ((exp + 112u) << 23) | (man << 13);
	}
	{
		float out;

		memcpy(&out, &f, 4);
		return out;
	}
}

static uint16_t float_to_half(float v)
{
	uint32_t f;
	int32_t exp;
	uint32_t sign, man;

	memcpy(&f, &v, 4);
	sign = (f >> 16) & 0x8000u;
	exp = (int32_t)((f >> 23) & 0xffu) - 127 + 15;
	man = f & 0x7fffffu;
	if (((f >> 23) & 0xffu) == 0xffu)
		return (uint16_t)(sign | 0x7c00u | (man ? 0x200u : 0u));
	if (exp >= 31)
		return (uint16_t)(sign | 0x7c00u);
	if (exp <= 0)
		return (uint16_t)sign;
	return (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
}

/* ------------------------------------------------------------------ */
/* element access (float view over any dtype)                          */
/* ------------------------------------------------------------------ */
static float rd_elem(const void *base, uint64_t i, Qnn_DataType_t dt,
                     const Qnn_QuantParams_t *q)
{
	switch (dt) {
	case QNN_DATATYPE_FLOAT32: {
		float v; memcpy(&v, (const uint8_t *)base + i * 4u, 4); return v;
	}
	case QNN_DATATYPE_FLOAT16: {
		uint16_t h; memcpy(&h, (const uint8_t *)base + i * 2u, 2);
		return half_to_float(h);
	}
	case QNN_DATATYPE_UINT8:
		return ((float)((const uint8_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	case QNN_DATATYPE_INT8:
		return ((float)((const int8_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	case QNN_DATATYPE_UINT16:
		return ((float)((const uint16_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	case QNN_DATATYPE_INT16:
		return ((float)((const int16_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	case QNN_DATATYPE_UINT32:
		return ((float)((const uint32_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	case QNN_DATATYPE_INT32:
		return ((float)((const int32_t *)base)[i] - (float)q->zeroPoint) *
		       q->scale;
	default:
		return 0.0f;
	}
}

static void wr_elem(void *base, uint64_t i, Qnn_DataType_t dt,
                    const Qnn_QuantParams_t *q, float v)
{
	switch (dt) {
	case QNN_DATATYPE_FLOAT32:
		memcpy((uint8_t *)base + i * 4u, &v, 4);
		break;
	case QNN_DATATYPE_FLOAT16: {
		uint16_t h = float_to_half(v);

		memcpy((uint8_t *)base + i * 2u, &h, 2);
		break;
	}
	case QNN_DATATYPE_UINT8: {
		int32_t r = (int32_t)lrintf(v / q->scale) + q->zeroPoint;

		((uint8_t *)base)[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
		break;
	}
	case QNN_DATATYPE_INT8: {
		int32_t r = (int32_t)lrintf(v / q->scale) + q->zeroPoint;

		((int8_t *)base)[i] = (int8_t)(r < -128 ? -128 : (r > 127 ? 127 : r));
		break;
	}
	case QNN_DATATYPE_UINT16: {
		int32_t r = (int32_t)lrintf(v / q->scale) + q->zeroPoint;

		((uint16_t *)base)[i] = (uint16_t)(r < 0 ? 0 : r);
		break;
	}
	case QNN_DATATYPE_INT16: {
		int32_t r = (int32_t)lrintf(v / q->scale) + q->zeroPoint;

		((int16_t *)base)[i] = (int16_t)(r < -32768 ? -32768 : r);
		break;
	}
	case QNN_DATATYPE_UINT32: {
		int64_t r = (int64_t)llrintf(v / q->scale) + q->zeroPoint;

		((uint32_t *)base)[i] = (uint32_t)(r < 0 ? 0 : r);
		break;
	}
	case QNN_DATATYPE_INT32: {
		int64_t r = (int64_t)llrintf(v / q->scale) + q->zeroPoint;

		((int32_t *)base)[i] = (int32_t)r;
		break;
	}
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* plan slots + tensor resolution                                      */
/* ------------------------------------------------------------------ */
static void *ref_resolve(qnn_ref_exec_t *plan, qnn_tensor_obj_t *t)
{
	uint32_t i;

	if (!t)
		return NULL;
	if (t->data)
		return t->data;
	for (i = 0; plan && i < plan->num_slots; i++)
		if (plan->slots[i].t == t)
			return plan->slots[i].buf;
	return NULL;
}

static int ref_slot_alloc(qnn_ref_exec_t *plan, qnn_tensor_obj_t *t)
{
	uint64_t bytes;
	void *buf;
	uint32_t i;

	if (!t || ref_resolve(plan, t))
		return 0;                       /* already has a buffer */
	bytes = qnn_tensor_elements(t) * (uint64_t)qnn_dtype_size(t->dtype);
	if (bytes == 0)
		return -1;
	buf = calloc(1, (size_t)bytes);
	if (!buf)
		return -1;
	if (plan->num_slots >= plan->slot_cap) {
		uint32_t nc = plan->slot_cap ? plan->slot_cap * 2u : 16u;
		ref_slot_t *np = (ref_slot_t *)
			realloc(plan->slots, (size_t)nc * sizeof(*np));

		if (!np) {
			free(buf);
			return -1;
		}
		plan->slots = np;
		plan->slot_cap = nc;
	}
	i = plan->num_slots++;
	plan->slots[i].t = t;
	plan->slots[i].buf = buf;
	plan->slots[i].bytes = bytes;
	return 0;
}

/* Dequantize a tensor into a fresh float array (caller frees). */
static float *to_float(qnn_ref_exec_t *plan, qnn_tensor_obj_t *t)
{
	uint64_t n = qnn_tensor_elements(t);
	const void *base = ref_resolve(plan, t);
	float *f;
	uint64_t i;

	if (!base || n == 0)
		return NULL;
	f = (float *)malloc((size_t)n * sizeof(float));
	if (!f)
		return NULL;
	for (i = 0; i < n; i++)
		f[i] = rd_elem(base, i, t->dtype, &t->quant);
	return f;
}

/* Quantize a float array into the tensor's resolved buffer. */
static int from_float(qnn_ref_exec_t *plan, qnn_tensor_obj_t *t,
                      const float *f, uint64_t n)
{
	void *base = ref_resolve(plan, t);
	uint64_t i;

	if (!base || !f)
		return -1;
	for (i = 0; i < n; i++)
		wr_elem(base, i, t->dtype, &t->quant, f[i]);
	return 0;
}

/* Row-major strides for a rank/dims pair. */
static void make_strides(uint32_t rank, const uint32_t *dims, uint64_t *st)
{
	int i;

	if (rank == 0)
		return;
	st[rank - 1] = 1;
	for (i = (int)rank - 2; i >= 0; i--)
		st[i] = st[i + 1] * dims[i + 1];
}

static uint32_t dim_aligned(qnn_tensor_obj_t *t, uint32_t i, uint32_t orank)
{
	uint32_t off = orank - t->rank;

	if (i < off)
		return 1u;
	return t->dims[i - off];
}

/* ------------------------------------------------------------------ */
/* elementwise binary (broadcast)                                      */
/* ------------------------------------------------------------------ */
static int op_elementwise(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *b = n->ins[1], *o = n->outs[0];
	uint32_t orank, i;
	uint32_t od[REF_MAX_DIM];
	uint64_t total = 1;
	float *fa, *fb, *fo;
	uint64_t idx;
	int rc = 0;

	if (!a || !b || !o)
		return -1;
	orank = a->rank > b->rank ? a->rank : b->rank;
	if (orank > REF_MAX_DIM)
		return -1;
	for (i = 0; i < orank; i++) {
		uint32_t da = dim_aligned(a, i, orank);
		uint32_t db = dim_aligned(b, i, orank);

		if (da != db && da != 1u && db != 1u)
			return -1;
		od[i] = da > db ? da : db;
		total *= od[i];
	}
	fa = to_float(plan, a);
	fb = to_float(plan, b);
	if (!fa || !fb) {
		free(fa);
		free(fb);
		return -1;
	}
	fo = (float *)malloc((size_t)total * sizeof(float));
	if (!fo) {
		free(fa);
		free(fb);
		return -1;
	}
	{
		uint64_t ast[REF_MAX_DIM], bst[REF_MAX_DIM], ost[REF_MAX_DIM];
		uint32_t coord[REF_MAX_DIM];

		make_strides(a->rank, a->dims, ast);
		make_strides(b->rank, b->dims, bst);
		make_strides(orank, od, ost);
		for (idx = 0; idx < total; idx++) {
			uint64_t rem = idx, ia = 0, ib = 0;
			float x, y, r;

			for (i = 0; i < orank; i++) {
				coord[i] = (uint32_t)(rem / ost[i]);
				rem %= ost[i];
			}
			for (i = 0; i < a->rank; i++) {
				uint32_t c = coord[orank - a->rank + i];

				ia += (uint64_t)(a->dims[i] == 1u ? 0u : c) * ast[i];
			}
			for (i = 0; i < b->rank; i++) {
				uint32_t c = coord[orank - b->rank + i];

				ib += (uint64_t)(b->dims[i] == 1u ? 0u : c) * bst[i];
			}
			x = fa[ia];
			y = fb[ib];
			switch (n->op_kind) {
			case QNN_OP_KIND_ADD:     r = x + y; break;
			case QNN_OP_KIND_SUB:     r = x - y; break;
			case QNN_OP_KIND_MUL:     r = x * y; break;
			case QNN_OP_KIND_DIV:     r = (y != 0.0f) ? x / y : 0.0f; break;
			case QNN_OP_KIND_MAXIMUM: r = x > y ? x : y; break;
			case QNN_OP_KIND_MINIMUM: r = x < y ? x : y; break;
			default:                  r = 0.0f; rc = -1; break;
			}
			fo[idx] = r;
		}
	}
	if (rc == 0)
		rc = from_float(plan, o, fo, total);
	free(fa);
	free(fb);
	free(fo);
	return rc;
}

/* ------------------------------------------------------------------ */
/* elementwise unary                                                   */
/* ------------------------------------------------------------------ */
static int op_unary(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint64_t total, i;
	float *fa, *fo;
	int rc;

	if (!a || !o)
		return -1;
	total = qnn_tensor_elements(a);
	fa = to_float(plan, a);
	if (!fa)
		return -1;
	fo = (float *)malloc((size_t)total * sizeof(float));
	if (!fo) {
		free(fa);
		return -1;
	}
	for (i = 0; i < total; i++) {
		float x = fa[i];

		switch (n->op_kind) {
		case QNN_OP_KIND_RELU:    fo[i] = x > 0.0f ? x : 0.0f; break;
		case QNN_OP_KIND_RELU6:   fo[i] = x < 0.0f ? 0.0f : (x > 6.0f ? 6.0f : x); break;
		case QNN_OP_KIND_SIGMOID: fo[i] = 1.0f / (1.0f + expf(-x)); break;
		case QNN_OP_KIND_TANH:    fo[i] = tanhf(x); break;
		default:                  fo[i] = x; break;
		}
	}
	rc = from_float(plan, o, fo, total);
	free(fa);
	free(fo);
	return rc;
}

/* ------------------------------------------------------------------ */
/* softmax (along axis, default last)                                  */
/* ------------------------------------------------------------------ */
static int op_softmax(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint64_t total, outer, inner, ax, i, j, k;
	float *fa, *fo;
	int32_t axis;
	int rc;

	if (!a || !o || a->rank == 0)
		return -1;
	axis = n->attrs.axis;
	if (axis < 0)
		axis = (int32_t)a->rank + axis;
	if (axis < 0 || (uint32_t)axis >= a->rank)
		axis = (int32_t)a->rank - 1;
	total = qnn_tensor_elements(a);
	fa = to_float(plan, a);
	if (!fa)
		return -1;
	fo = (float *)malloc((size_t)total * sizeof(float));
	if (!fo) {
		free(fa);
		return -1;
	}
	outer = 1;
	for (i = 0; i < (uint32_t)axis; i++)
		outer *= a->dims[i];
	ax = a->dims[axis];
	inner = 1;
	for (i = (uint32_t)axis + 1; i < a->rank; i++)
		inner *= a->dims[i];
	for (i = 0; i < outer; i++) {
		for (k = 0; k < inner; k++) {
			uint64_t base = (i * ax) * inner + k;
			float mx = -INFINITY, sum = 0.0f;

			for (j = 0; j < ax; j++) {
				float v = fa[base + j * inner];

				if (v > mx)
					mx = v;
			}
			for (j = 0; j < ax; j++) {
				float e = expf(fa[base + j * inner] - mx);

				fo[base + j * inner] = e;
				sum += e;
			}
			if (sum == 0.0f)
				sum = 1.0f;
			for (j = 0; j < ax; j++)
				fo[base + j * inner] /= sum;
		}
	}
	rc = from_float(plan, o, fo, total);
	free(fa);
	free(fo);
	return rc;
}


/* ------------------------------------------------------------------ */
/* Conv2d / DepthwiseConv2d (NHWC)                                     */
/* ------------------------------------------------------------------ */
static int op_conv(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *w = n->ins[1], *o = n->outs[0];
	qnn_tensor_obj_t *bias = (n->num_ins >= 3) ? n->ins[2] : NULL;
	uint32_t Nn, H, W, Cin, KH, KW, Cout, Cin_per, OH, OW;
	int group, Cout_per, pt, pb, pl, pr, sh, sw, dh, dw;
	float *fa, *fw, *fb, *fo;
	uint64_t total_out;
	uint32_t nn, oh, ow, oc, kh, kw, cig;
	int32_t ih, iw, ic, g;

	if (!a || !w || !o || a->rank != 4 || w->rank != 4 || o->rank != 4)
		return -1;
	Nn = a->dims[0]; H = a->dims[1]; W = a->dims[2]; Cin = a->dims[3];
	KH = w->dims[0]; KW = w->dims[1]; Cin_per = w->dims[2]; Cout = w->dims[3];
	OH = o->dims[1]; OW = o->dims[2];
	group = (n->op_kind == QNN_OP_KIND_DEPTHWISE_CONV2D) ? (int)Cin :
	        (n->attrs.group > 0 ? (int)n->attrs.group : 1);
	if (group <= 0 || (int)Cout % group != 0 || (int)Cin % group != 0)
		return -1;
	Cout_per = (int)Cout / group;
	pt = n->attrs.pad[0]; pb = n->attrs.pad[1];
	pl = n->attrs.pad[2]; pr = n->attrs.pad[3];
	sh = n->attrs.stride[0] > 0 ? n->attrs.stride[0] : 1;
	sw = n->attrs.stride[1] > 0 ? n->attrs.stride[1] : 1;
	dh = n->attrs.dilate[0] > 0 ? n->attrs.dilate[0] : 1;
	dw = n->attrs.dilate[1] > 0 ? n->attrs.dilate[1] : 1;
	(void)pb; (void)pr;

	fa = to_float(plan, a);
	fw = to_float(plan, w);
	fb = bias ? to_float(plan, bias) : NULL;
	if (!fa || !fw || (bias && !fb)) {
		free(fa); free(fw); free(fb);
		return -1;
	}
	total_out = qnn_tensor_elements(o);
	fo = (float *)calloc((size_t)total_out, sizeof(float));
	if (!fo) {
		free(fa); free(fw); free(fb);
		return -1;
	}
	for (nn = 0; nn < Nn; nn++)
		for (oh = 0; oh < OH; oh++)
			for (ow = 0; ow < OW; ow++)
				for (oc = 0; oc < Cout; oc++) {
					float acc = fb ? fb[oc] : 0.0f;

					g = (int)oc / Cout_per;
					for (kh = 0; kh < KH; kh++)
						for (kw = 0; kw < KW; kw++)
							for (cig = 0; cig < Cin_per; cig++) {
								float wv;

								ih = (int32_t)(oh * (uint32_t)sh) - pt +
								     (int32_t)(kh * (uint32_t)dh);
								iw = (int32_t)(ow * (uint32_t)sw) - pl +
								     (int32_t)(kw * (uint32_t)dw);
								if (ih < 0 || iw < 0 ||
								    (uint32_t)ih >= H ||
								    (uint32_t)iw >= W)
									continue;
								ic = g * (int)Cin_per + (int)cig;
								wv = fw[(((uint64_t)(kh * KW + kw) *
								      Cin_per) + cig) * Cout + oc];
								acc += fa[(((uint64_t)(nn * H +
								       (uint32_t)ih) * W +
								       (uint32_t)iw) * Cin) +
								       (uint32_t)ic] * wv;
							}
					fo[((uint64_t)(nn * OH + oh) * OW + ow) *
					   Cout + oc] = acc;
				}
	{
		int rc = from_float(plan, o, fo, total_out);

		free(fa); free(fw); free(fb); free(fo);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/* FullyConnected                                                      */
/* ------------------------------------------------------------------ */
static int op_fc(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *w = n->ins[1], *o = n->outs[0];
	qnn_tensor_obj_t *bias = (n->num_ins >= 3) ? n->ins[2] : NULL;
	uint32_t Cin, Cout, batch;
	float *fa, *fw, *fb, *fo;
	uint64_t total_out;
	uint32_t bb, oc, ic;

	if (!a || !w || !o || w->rank < 2)
		return -1;
	Cin = w->dims[w->rank - 2];
	Cout = w->dims[w->rank - 1];
	if (Cin == 0 || Cout == 0)
		return -1;
	batch = (uint32_t)(qnn_tensor_elements(a) / Cin);
	fa = to_float(plan, a);
	fw = to_float(plan, w);
	fb = bias ? to_float(plan, bias) : NULL;
	if (!fa || !fw || (bias && !fb)) {
		free(fa); free(fw); free(fb);
		return -1;
	}
	total_out = (uint64_t)batch * Cout;
	fo = (float *)calloc((size_t)total_out, sizeof(float));
	if (!fo) {
		free(fa); free(fw); free(fb);
		return -1;
	}
	for (bb = 0; bb < batch; bb++)
		for (oc = 0; oc < Cout; oc++) {
			float acc = fb ? fb[oc] : 0.0f;

			for (ic = 0; ic < Cin; ic++)
				acc += fa[(uint64_t)bb * Cin + ic] *
				       fw[(uint64_t)ic * Cout + oc];
			if (n->attrs.activation == 1 && acc < 0.0f)
				acc = 0.0f;
			fo[(uint64_t)bb * Cout + oc] = acc;
		}
	{
		int rc = from_float(plan, o, fo, total_out);

		free(fa); free(fw); free(fb); free(fo);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/* PoolAvg2d / PoolMax2d (NHWC)                                        */
/* ------------------------------------------------------------------ */
static int op_pool(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint32_t Nn, H, W, C, KH, KW, OH, OW;
	int pt, pl, sh, sw, is_max;
	float *fa, *fo;
	uint64_t total_out;
	uint32_t nn, oh, ow, c, kh, kw;

	if (!a || !o || a->rank != 4 || o->rank != 4)
		return -1;
	Nn = a->dims[0]; H = a->dims[1]; W = a->dims[2]; C = a->dims[3];
	KH = (uint32_t)(n->attrs.kernel[0] > 0 ? n->attrs.kernel[0] : 1);
	KW = (uint32_t)(n->attrs.kernel[1] > 0 ? n->attrs.kernel[1] : 1);
	OH = o->dims[1]; OW = o->dims[2];
	pt = n->attrs.pad[0]; pl = n->attrs.pad[2];
	sh = n->attrs.stride[0] > 0 ? n->attrs.stride[0] : 1;
	sw = n->attrs.stride[1] > 0 ? n->attrs.stride[1] : 1;
	is_max = (n->op_kind == QNN_OP_KIND_MAX_POOL);

	fa = to_float(plan, a);
	if (!fa)
		return -1;
	total_out = qnn_tensor_elements(o);
	fo = (float *)calloc((size_t)total_out, sizeof(float));
	if (!fo) {
		free(fa);
		return -1;
	}
	for (nn = 0; nn < Nn; nn++)
		for (oh = 0; oh < OH; oh++)
			for (ow = 0; ow < OW; ow++)
				for (c = 0; c < C; c++) {
					float acc = is_max ? -INFINITY : 0.0f;
					uint32_t cnt = 0;

					for (kh = 0; kh < KH; kh++)
						for (kw = 0; kw < KW; kw++) {
							int32_t ih = (int32_t)(oh *
								(uint32_t)sh) - pt +
								(int32_t)kh;
							int32_t iw = (int32_t)(ow *
								(uint32_t)sw) - pl +
								(int32_t)kw;
							float v;

							if (ih < 0 || iw < 0 ||
							    (uint32_t)ih >= H ||
							    (uint32_t)iw >= W)
								continue;
							v = fa[(((uint64_t)(nn * H +
							    (uint32_t)ih) * W +
							    (uint32_t)iw) * C) + c];
							if (is_max) {
								if (v > acc)
									acc = v;
							} else {
								acc += v;
								cnt++;
							}
						}
					if (!is_max && cnt)
						acc /= (float)cnt;
					if (is_max && acc == -INFINITY)
						acc = 0.0f;
					fo[((uint64_t)(nn * OH + oh) * OW + ow) *
					   C + c] = acc;
				}
	{
		int rc = from_float(plan, o, fo, total_out);

		free(fa); free(fo);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/* Reduce (sum / mean / max / min)                                     */
/* ------------------------------------------------------------------ */
static int op_reduce(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint32_t rank, i, orank = 0;
	uint8_t reduced[REF_MAX_DIM];
	uint32_t od[REF_MAX_DIM];
	int32_t amap[REF_MAX_DIM];
	uint64_t ast[REF_MAX_DIM], ost[REF_MAX_DIM];
	uint64_t total_in, total_out = 1, idx, count = 1;
	float *fa, *fo;
	int is_max = (n->op_kind == QNN_OP_KIND_REDUCE_MAX);
	int is_min = (n->op_kind == QNN_OP_KIND_REDUCE_MIN);
	int is_mean = (n->op_kind == QNN_OP_KIND_REDUCE_MEAN);
	int keep = n->attrs.keep_dims;

	if (!a || !o || a->rank == 0 || a->rank > REF_MAX_DIM)
		return -1;
	rank = a->rank;
	memset(reduced, 0, sizeof(reduced));
	if (n->attrs.num_axes == 0) {
		for (i = 0; i < rank; i++)
			reduced[i] = 1;
	} else {
		for (i = 0; i < n->attrs.num_axes && i < rank; i++) {
			int32_t ax = n->attrs.axes[i];

			if (ax < 0)
				ax += (int32_t)rank;
			if (ax >= 0 && (uint32_t)ax < rank)
				reduced[ax] = 1;
		}
	}
	for (i = 0; i < rank; i++) {
		if (reduced[i]) {
			count *= a->dims[i];
			amap[i] = -1;
			if (keep) {
				amap[i] = (int32_t)orank;
				od[orank++] = 1u;
			}
		} else {
			amap[i] = (int32_t)orank;
			od[orank++] = a->dims[i];
		}
	}
	for (i = 0; i < orank; i++)
		total_out *= od[i];
	fa = to_float(plan, a);
	if (!fa)
		return -1;
	fo = (float *)malloc((size_t)total_out * sizeof(float));
	if (!fo) {
		free(fa);
		return -1;
	}
	for (idx = 0; idx < total_out; idx++)
		fo[idx] = is_max ? -INFINITY : (is_min ? INFINITY : 0.0f);
	total_in = qnn_tensor_elements(a);
	make_strides(rank, a->dims, ast);
	make_strides(orank, od, ost);
	for (idx = 0; idx < total_in; idx++) {
		uint32_t coord[REF_MAX_DIM];
		uint64_t rem = idx, oi = 0;
		float v = fa[idx];

		for (i = 0; i < rank; i++) {
			coord[i] = (uint32_t)(rem / ast[i]);
			rem %= ast[i];
		}
		for (i = 0; i < rank; i++) {
			if (amap[i] < 0)
				continue;
			oi += (uint64_t)coord[i] * ost[amap[i]];
		}
		if (is_max) {
			if (v > fo[oi]) fo[oi] = v;
		} else if (is_min) {
			if (v < fo[oi]) fo[oi] = v;
		} else {
			fo[oi] += v;
		}
	}
	if (is_mean && count)
		for (idx = 0; idx < total_out; idx++)
			fo[idx] /= (float)count;
	{
		int rc = from_float(plan, o, fo, total_out);

		free(fa); free(fo);
		return rc;
	}
}

/* ------------------------------------------------------------------ */
/* Concat (along axis)                                                 */
/* ------------------------------------------------------------------ */
static int op_concat(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *o = n->outs[0];
	qnn_tensor_obj_t *first = n->ins[0];
	uint32_t rank, axis, i, k;
	uint64_t ost[REF_MAX_DIM], total_out;
	float *fo;
	uint32_t axis_off = 0;
	int rc = 0;

	if (!o || !first || first->rank == 0 || n->num_ins < 2)
		return -1;
	rank = first->rank;
	axis = (uint32_t)n->attrs.axis;
	if (n->attrs.axis < 0)
		axis = (uint32_t)((int32_t)rank + n->attrs.axis);
	if (axis >= rank)
		return -1;
	total_out = qnn_tensor_elements(o);
	fo = (float *)calloc((size_t)total_out, sizeof(float));
	if (!fo)
		return -1;
	make_strides(o->rank, o->dims, ost);
	for (k = 0; k < n->num_ins; k++) {
		qnn_tensor_obj_t *t = n->ins[k];
		float *ft;
		uint64_t ist[REF_MAX_DIM], tn, idx;

		if (!t || t->rank != rank) {
			rc = -1;
			break;
		}
		ft = to_float(plan, t);
		if (!ft) {
			rc = -1;
			break;
		}
		make_strides(rank, t->dims, ist);
		tn = qnn_tensor_elements(t);
		for (idx = 0; idx < tn; idx++) {
			uint32_t coord[REF_MAX_DIM];
			uint64_t rem = idx, oi = 0;

			for (i = 0; i < rank; i++) {
				coord[i] = (uint32_t)(rem / ist[i]);
				rem %= ist[i];
			}
			coord[axis] += axis_off;
			for (i = 0; i < rank; i++)
				oi += (uint64_t)coord[i] * ost[i];
			if (oi < total_out)
				fo[oi] = ft[idx];
		}
		axis_off += t->dims[axis];
		free(ft);
	}
	if (rc == 0)
		rc = from_float(plan, o, fo, total_out);
	free(fo);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Transpose (perm)                                                    */
/* ------------------------------------------------------------------ */
static int op_transpose(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint32_t rank, i, perm[REF_MAX_DIM];
	uint64_t ast[REF_MAX_DIM], total;
	float *fa, *fo;
	uint64_t idx;
	int rc;

	if (!a || !o || a->rank == 0 || a->rank != o->rank || a->rank > REF_MAX_DIM)
		return -1;
	rank = a->rank;
	if (n->attrs.num_perm == rank) {
		for (i = 0; i < rank; i++)
			perm[i] = (uint32_t)n->attrs.perm[i];
	} else {
		for (i = 0; i < rank; i++)
			perm[i] = rank - 1u - i;   /* default reverse */
	}
	total = qnn_tensor_elements(a);
	fa = to_float(plan, a);
	if (!fa)
		return -1;
	fo = (float *)malloc((size_t)total * sizeof(float));
	if (!fo) {
		free(fa);
		return -1;
	}
	make_strides(rank, a->dims, ast);
	for (idx = 0; idx < total; idx++) {
		uint32_t oc[REF_MAX_DIM], ic[REF_MAX_DIM];
		uint64_t rem = idx, oi = 0, ii = 0;
		uint64_t ost[REF_MAX_DIM];
		uint32_t od[REF_MAX_DIM];

		for (i = 0; i < rank; i++)
			od[i] = o->dims[i];
		make_strides(rank, od, ost);
		for (i = 0; i < rank; i++) {
			oc[i] = (uint32_t)(rem / ost[i]);
			rem %= ost[i];
		}
		for (i = 0; i < rank; i++)
			ic[perm[i]] = oc[i];
		for (i = 0; i < rank; i++)
			ii += (uint64_t)ic[i] * ast[i];
		fo[idx] = fa[ii];
		(void)oi;
	}
	rc = from_float(plan, o, fo, total);
	free(fa);
	free(fo);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Reshape / Convert (element copy through float)                      */
/* ------------------------------------------------------------------ */
static int op_copy(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	qnn_tensor_obj_t *a = n->ins[0], *o = n->outs[0];
	uint64_t total;
	float *fa;
	int rc;

	if (!a || !o)
		return -1;
	total = qnn_tensor_elements(o);
	if (total != qnn_tensor_elements(a))
		return -1;
	fa = to_float(plan, a);
	if (!fa)
		return -1;
	rc = from_float(plan, o, fa, total);
	free(fa);
	return rc;
}

/* ------------------------------------------------------------------ */
/* dispatch + plan lifecycle                                           */
/* ------------------------------------------------------------------ */
static int ref_run_node(qnn_ref_exec_t *plan, qnn_node_obj_t *n)
{
	switch (n->op_kind) {
	case QNN_OP_KIND_ADD:
	case QNN_OP_KIND_SUB:
	case QNN_OP_KIND_MUL:
	case QNN_OP_KIND_DIV:
	case QNN_OP_KIND_MAXIMUM:
	case QNN_OP_KIND_MINIMUM:
		return op_elementwise(plan, n);
	case QNN_OP_KIND_RELU:
	case QNN_OP_KIND_RELU6:
	case QNN_OP_KIND_SIGMOID:
	case QNN_OP_KIND_TANH:
		return op_unary(plan, n);
	case QNN_OP_KIND_SOFTMAX:
		return op_softmax(plan, n);
	case QNN_OP_KIND_CONV2D:
	case QNN_OP_KIND_DEPTHWISE_CONV2D:
		return op_conv(plan, n);
	case QNN_OP_KIND_FULLY_CONNECTED:
		return op_fc(plan, n);
	case QNN_OP_KIND_AVG_POOL:
	case QNN_OP_KIND_MAX_POOL:
		return op_pool(plan, n);
	case QNN_OP_KIND_REDUCE_SUM:
	case QNN_OP_KIND_REDUCE_MEAN:
	case QNN_OP_KIND_REDUCE_MAX:
	case QNN_OP_KIND_REDUCE_MIN:
		return op_reduce(plan, n);
	case QNN_OP_KIND_CONCAT:
		return op_concat(plan, n);
	case QNN_OP_KIND_TRANSPOSE:
		return op_transpose(plan, n);
	case QNN_OP_KIND_RESHAPE:
	case QNN_OP_KIND_CONVERT:
		return op_copy(plan, n);
	default:
		return -1;
	}
}

static int kind_supported(uint32_t kind)
{
	switch (kind) {
	case QNN_OP_KIND_ADD: case QNN_OP_KIND_SUB: case QNN_OP_KIND_MUL:
	case QNN_OP_KIND_DIV: case QNN_OP_KIND_MAXIMUM: case QNN_OP_KIND_MINIMUM:
	case QNN_OP_KIND_RELU: case QNN_OP_KIND_RELU6: case QNN_OP_KIND_SIGMOID:
	case QNN_OP_KIND_TANH: case QNN_OP_KIND_SOFTMAX: case QNN_OP_KIND_CONV2D:
	case QNN_OP_KIND_DEPTHWISE_CONV2D: case QNN_OP_KIND_FULLY_CONNECTED:
	case QNN_OP_KIND_AVG_POOL: case QNN_OP_KIND_MAX_POOL:
	case QNN_OP_KIND_REDUCE_SUM: case QNN_OP_KIND_REDUCE_MEAN:
	case QNN_OP_KIND_REDUCE_MAX: case QNN_OP_KIND_REDUCE_MIN:
	case QNN_OP_KIND_CONCAT: case QNN_OP_KIND_TRANSPOSE:
	case QNN_OP_KIND_RESHAPE: case QNN_OP_KIND_CONVERT:
		return 1;
	default:
		return 0;
	}
}

qnn_ref_exec_t *qnn_ref_build(qnn_graph_obj_t *g, char *err, size_t errsz)
{
	qnn_ref_exec_t *plan;
	uint32_t i, j;

	if (!g)
		return NULL;
	plan = (qnn_ref_exec_t *)calloc(1, sizeof(*plan));
	if (!plan)
		return NULL;
	for (i = 0; i < g->num_nodes; i++) {
		qnn_node_obj_t *n = &g->nodes[i];

		if (!kind_supported(n->op_kind)) {
			if (err)
				snprintf(err, errsz, "node %u op '%s' unsupported",
				         i, n->op_name ? n->op_name : "?");
			qnn_ref_free(plan);
			return NULL;
		}
		if (n->num_ins == 0 || n->num_outs == 0) {
			if (err)
				snprintf(err, errsz, "node %u missing io", i);
			qnn_ref_free(plan);
			return NULL;
		}
		for (j = 0; j < n->num_ins; j++)
			if (!n->ins[j]) {
				if (err)
					snprintf(err, errsz, "node %u null input", i);
				qnn_ref_free(plan);
				return NULL;
			}
		/* Outputs need a destination: graph outputs get an owned buffer
		   the client can read; pure intermediates get plan scratch. */
		for (j = 0; j < n->num_outs; j++) {
			qnn_tensor_obj_t *t = n->outs[j];

			if (!t) {
				if (err)
					snprintf(err, errsz, "node %u null output", i);
				qnn_ref_free(plan);
				return NULL;
			}
			if (t->data)
				continue;
			if (t->is_graph_output) {
				uint64_t bytes = qnn_tensor_elements(t) *
				                 (uint64_t)qnn_dtype_size(t->dtype);

				if (bytes == 0 ||
				    qnn_tensor_reserve_data(t, bytes) != QNN_SUCCESS) {
					if (err)
						snprintf(err, errsz,
						         "node %u output alloc", i);
					qnn_ref_free(plan);
					return NULL;
				}
			} else if (ref_slot_alloc(plan, t) != 0) {
				if (err)
					snprintf(err, errsz,
					         "node %u scratch alloc", i);
				qnn_ref_free(plan);
				return NULL;
			}
		}
	}
	return plan;
}

int qnn_ref_execute(qnn_graph_obj_t *g, qnn_ref_exec_t *plan,
                    qnn_tensor_obj_t *const *ins, uint32_t num_ins,
                    qnn_tensor_obj_t *const *outs, uint32_t num_outs,
                    Qnn_ProfileHandle_t prof, char *err, size_t errsz)
{
	uint32_t i;

	(void)ins; (void)num_ins; (void)outs; (void)num_outs;
	if (!g || !plan)
		return -1;
	/* Nodes run in insertion order, which the graph builder keeps
	   topological (the QNN addNode contract). */
	for (i = 0; i < g->num_nodes; i++) {
		qnn_node_obj_t *n = &g->nodes[i];
		uint64_t t0 = qnn_time_usec();
		int rc = ref_run_node(plan, n);

		if (rc != 0) {
			if (err)
				snprintf(err, errsz, "node %u '%s' failed",
				         i, n->op_name ? n->op_name : "?");
			return rc;
		}
		if (prof)
			qnn_profile_record(prof, i, n->op_name ? n->op_name : "op",
			                   qnn_time_usec() - t0);
	}
	return 0;
}

void qnn_ref_free(qnn_ref_exec_t *plan)
{
	uint32_t i;

	if (!plan)
		return;
	for (i = 0; i < plan->num_slots; i++)
		free(plan->slots[i].buf);
	free(plan->slots);
	free(plan);
}
