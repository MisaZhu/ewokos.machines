/*
 * qnn_tensor.c - QnnTensor_* implementation and the context tensor pool.
 *
 * Every tensor is context-scoped (the QNN v2 model): createContextTensor and
 * createGraphTensor both allocate into the owning context's pool, and a graph
 * tensor is additionally routed onto its graph's input/output boundary by
 * Qnn_TensorType_t.  Data buffers are aliased, not copied - QnnTensor_setData
 * points the tensor at the caller's memory exactly like the vendor API, where
 * the client keeps ownership until execute() returns.  Only the deserializer
 * (qnn_context.c) allocates owned buffers, for STATIC weights baked into a
 * context binary.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <stdlib.h>
#include <string.h>

/* Append a tensor to the context pool, growing it geometrically. */
int qnn_ctx_track_tensor(qnn_context_obj_t *c, qnn_tensor_obj_t *t)
{
	if (!c || !t)
		return QNN_ERROR;
	if (c->num_tensors >= c->tensor_cap) {
		uint32_t nc = c->tensor_cap ? c->tensor_cap * 2u : 16u;
		qnn_tensor_obj_t **np = (qnn_tensor_obj_t **)
			realloc(c->tensors, (size_t)nc * sizeof(*np));

		if (!np)
			return QNN_ERROR;
		c->tensors = np;
		c->tensor_cap = nc;
	}
	c->tensors[c->num_tensors++] = t;
	return QNN_SUCCESS;
}

qnn_tensor_obj_t *qnn_tensor_create_common(qnn_context_obj_t *c,
        qnn_graph_obj_t *g, const char *name, Qnn_TensorType_t ttype,
        Qnn_DataType_t dtype, const Qnn_Shape_t *shape,
        const Qnn_QuantParams_t *qp)
{
	qnn_tensor_obj_t *t;
	uint32_t i;

	if (!c)
		return NULL;
	t = (qnn_tensor_obj_t *)qnn_obj_alloc(QNN_OBJ_TENSOR, sizeof(*t));
	if (!t)
		return NULL;
	t->ctx = c;
	t->name = qnn_strdup_safe(name ? name : "");
	if (name && !t->name) {
		qnn_obj_free(t);
		return NULL;
	}
	t->ttype = ttype;
	t->dtype = dtype;
	if (shape) {
		t->rank = shape->rank > 8u ? 8u : shape->rank;
		for (i = 0; i < t->rank && shape->dims; i++)
			t->dims[i] = shape->dims[i];
	}
	if (qp)
		t->quant = *qp;
	else {
		t->quant.scale = 1.0f;
		t->quant.zeroPoint = 0;
	}
	t->is_graph_input = (ttype == QNN_TENSOR_TYPE_APP_WRITE ||
	                     ttype == QNN_TENSOR_TYPE_READWRITE);
	t->is_graph_output = (ttype == QNN_TENSOR_TYPE_APP_READ ||
	                      ttype == QNN_TENSOR_TYPE_READWRITE);

	if (qnn_ctx_track_tensor(c, t) != QNN_SUCCESS) {
		free(t->name);
		qnn_obj_free(t);
		return NULL;
	}
	/* A graph tensor joins the graph boundary; a context tensor (weights,
	   or one not yet bound to a graph) does not. */
	if (g && qnn_graph_add_boundary(g, t) != QNN_SUCCESS) {
		/* Untrack from the pool on failure to keep ownership clean. */
		uint32_t k;

		for (k = 0; k < c->num_tensors; k++) {
			if (c->tensors[k] == t) {
				memmove(&c->tensors[k], &c->tensors[k + 1],
				        (c->num_tensors - k - 1) *
				        sizeof(*c->tensors));
				c->num_tensors--;
				break;
			}
		}
		free(t->name);
		qnn_obj_free(t);
		return NULL;
	}
	return t;
}

int qnn_tensor_reserve_data(qnn_tensor_obj_t *t, uint64_t bytes)
{
	void *p;

	if (!t || bytes == 0)
		return QNN_ERROR;
	if (t->data_owned && t->data) {
		p = realloc(t->data, (size_t)bytes);
		if (!p)
			return QNN_ERROR;
	} else {
		p = malloc((size_t)bytes);
		if (!p)
			return QNN_ERROR;
		/* Any prior alias belonged to the client - drop, do not free. */
	}
	t->data = p;
	t->data_bytes = bytes;
	t->data_owned = true;
	return QNN_SUCCESS;
}

/* True when a finalized/under-construction graph node still references `t`. */
static bool tensor_in_use(qnn_context_obj_t *c, qnn_tensor_obj_t *t)
{
	uint32_t gi, ni, k;

	for (gi = 0; gi < c->num_graphs; gi++) {
		qnn_graph_obj_t *g = c->graphs[gi];

		if (!g)
			continue;
		for (k = 0; k < g->num_graph_ins; k++)
			if (g->graph_ins[k] == t)
				return true;
		for (k = 0; k < g->num_graph_outs; k++)
			if (g->graph_outs[k] == t)
				return true;
		for (ni = 0; ni < g->num_nodes; ni++) {
			qnn_node_obj_t *n = &g->nodes[ni];
			uint32_t j;

			for (j = 0; j < n->num_ins; j++)
				if (n->ins[j] == t)
					return true;
			for (j = 0; j < n->num_outs; j++)
				if (n->outs[j] == t)
					return true;
		}
	}
	return false;
}

Qnn_ErrorHandle_t QnnTensor_createContextTensor(Qnn_ContextHandle_t ctx,
        const char *name, Qnn_TensorType_t tType, Qnn_DataType_t dType,
        const Qnn_Shape_t *shape, const Qnn_QuantParams_t *qp,
        Qnn_TensorHandle_t *tensor)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	qnn_tensor_obj_t *t;

	if (!c || !tensor)
		return QNN_ERROR;
	t = qnn_tensor_create_common(c, NULL, name, tType, dType, shape, qp);
	if (!t)
		return QNN_ERROR;
	*tensor = (Qnn_TensorHandle_t)(uintptr_t)t;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_createGraphTensor(Qnn_GraphHandle_t graph,
        const char *name, Qnn_TensorType_t tType, Qnn_DataType_t dType,
        const Qnn_Shape_t *shape, const Qnn_QuantParams_t *qp,
        Qnn_TensorHandle_t *tensor)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);
	qnn_tensor_obj_t *t;

	if (!g || !tensor || g->native_graph)
		return QNN_ERROR;
	t = qnn_tensor_create_common(g->ctx, g, name, tType, dType, shape, qp);
	if (!t)
		return QNN_ERROR;
	*tensor = (Qnn_TensorHandle_t)(uintptr_t)t;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_free(Qnn_TensorHandle_t tensor)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);
	qnn_context_obj_t *c;
	uint32_t k;

	if (!t)
		return QNN_ERROR;
	c = t->ctx;
	if (!c)
		return QNN_ERROR;
	/* Tensors are context-scoped; freeing one a graph still references
	   would leave dangling node pointers, so refuse it exactly like the
	   vendor API rejects an in-use tensor.  Release the whole context to
	   reclaim these. */
	if (tensor_in_use(c, t))
		return QNN_ERROR;
	for (k = 0; k < c->num_tensors; k++) {
		if (c->tensors[k] == t) {
			memmove(&c->tensors[k], &c->tensors[k + 1],
			        (c->num_tensors - k - 1) * sizeof(*c->tensors));
			c->num_tensors--;
			break;
		}
	}
	if (t->data_owned)
		free(t->data);
	free(t->name);
	qnn_obj_free(t);
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_setData(Qnn_TensorHandle_t tensor, void *data,
                                    uint64_t bytes)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);
	uint64_t expect;

	if (!t)
		return QNN_ERROR;
	expect = qnn_tensor_elements(t) * (uint64_t)qnn_dtype_size(t->dtype);
	if (data && bytes && expect && bytes != expect)
		return QNN_ERROR;
	/* If the tensor owns a buffer (deserialized weight), copy into it so
	   the caller's memory is not retained; otherwise alias the caller. */
	if (t->data_owned && t->data && data && bytes <= t->data_bytes) {
		memcpy(t->data, data, (size_t)bytes);
		return QNN_SUCCESS;
	}
	if (t->data_owned && t->data) {
		free(t->data);
		t->data_owned = false;
	}
	t->data = data;
	t->data_bytes = data ? bytes : 0;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_getData(Qnn_TensorHandle_t tensor, void **data,
                                    uint64_t *bytes)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);

	if (!t || !data || !bytes)
		return QNN_ERROR;
	*data = t->data;
	*bytes = t->data_bytes;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_getShape(Qnn_TensorHandle_t tensor,
                                     Qnn_Shape_t *shape)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);

	if (!t || !shape)
		return QNN_ERROR;
	shape->rank = t->rank;
	/* dims points at the tensor's own storage; valid while it lives. */
	shape->dims = t->dims;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_getDataType(Qnn_TensorHandle_t tensor,
                                        Qnn_DataType_t *dtype)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);

	if (!t || !dtype)
		return QNN_ERROR;
	*dtype = t->dtype;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_getTensorType(Qnn_TensorHandle_t tensor,
                                          Qnn_TensorType_t *ttype)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);

	if (!t || !ttype)
		return QNN_ERROR;
	*ttype = t->ttype;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnTensor_getQuantParams(Qnn_TensorHandle_t tensor,
                                           Qnn_QuantParams_t *qp)
{
	qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
		qnn_obj_from_handle(tensor, QNN_OBJ_TENSOR);

	if (!t || !qp)
		return QNN_ERROR;
	*qp = t->quant;
	return QNN_SUCCESS;
}
