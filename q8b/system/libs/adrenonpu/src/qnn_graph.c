/*
 * qnn_graph.c - QnnGraph_* implementation: node construction, finalize,
 * and the dual-path execute dispatch.
 *
 * A graph is an ordered list of nodes over the context tensor pool.  finalize
 * prepares it for execution: when the cDSP session is up it is marshalled to
 * the HTP (npu_htp.c); the CPU reference plan (npu_ref_exec.c) is always built
 * so the graph stays runnable off silicon, mirroring how the vendor SDK ships
 * libQnnCpu.so alongside libQnnHtp.so.  execute binds the caller's I/O tensor
 * data onto the graph boundary and runs whichever path is available.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <stdlib.h>
#include <string.h>

int qnn_ctx_track_graph(qnn_context_obj_t *c, qnn_graph_obj_t *g)
{
	if (!c || !g)
		return QNN_ERROR;
	if (c->num_graphs >= c->graph_cap) {
		uint32_t nc = c->graph_cap ? c->graph_cap * 2u : 8u;
		qnn_graph_obj_t **np = (qnn_graph_obj_t **)
			realloc(c->graphs, (size_t)nc * sizeof(*np));

		if (!np)
			return QNN_ERROR;
		c->graphs = np;
		c->graph_cap = nc;
	}
	c->graphs[c->num_graphs++] = g;
	return QNN_SUCCESS;
}

qnn_graph_obj_t *qnn_graph_obj_create(qnn_context_obj_t *c, const char *name)
{
	qnn_graph_obj_t *g;

	if (!c)
		return NULL;
	g = (qnn_graph_obj_t *)qnn_obj_alloc(QNN_OBJ_GRAPH, sizeof(*g));
	if (!g)
		return NULL;
	g->ctx = c;
	g->name = qnn_strdup_safe(name ? name : "");
	if (name && !g->name) {
		qnn_obj_free(g);
		return NULL;
	}
	g->htp = c->htp;
	if (qnn_ctx_track_graph(c, g) != QNN_SUCCESS) {
		free(g->name);
		qnn_obj_free(g);
		return NULL;
	}
	return g;
}

int qnn_graph_track_tensor(qnn_graph_obj_t *g, qnn_tensor_obj_t *t)
{
	uint32_t i;

	if (!g || !t)
		return QNN_ERROR;
	for (i = 0; i < g->num_tensors; i++)
		if (g->tensors[i] == t)
			return QNN_SUCCESS;   /* already tracked */
	if (g->num_tensors >= g->tensor_cap) {
		uint32_t nc = g->tensor_cap ? g->tensor_cap * 2u : 16u;
		qnn_tensor_obj_t **np = (qnn_tensor_obj_t **)
			realloc(g->tensors, (size_t)nc * sizeof(*np));

		if (!np)
			return QNN_ERROR;
		g->tensors = np;
		g->tensor_cap = nc;
	}
	g->tensors[g->num_tensors++] = t;
	return QNN_SUCCESS;
}

/* Append `t` to a boundary list, growing it. */
static int boundary_push(qnn_tensor_obj_t ***list, uint32_t *n,
                         qnn_tensor_obj_t *t)
{
	uint32_t i;
	qnn_tensor_obj_t **np;

	for (i = 0; i < *n; i++)
		if ((*list)[i] == t)
			return QNN_SUCCESS;
	np = (qnn_tensor_obj_t **)realloc(*list, (size_t)(*n + 1) * sizeof(*np));
	if (!np)
		return QNN_ERROR;
	np[*n] = t;
	*list = np;
	(*n)++;
	return QNN_SUCCESS;
}

int qnn_graph_add_boundary(qnn_graph_obj_t *g, qnn_tensor_obj_t *t)
{
	if (qnn_graph_track_tensor(g, t) != QNN_SUCCESS)
		return QNN_ERROR;
	if (t->is_graph_input &&
	    boundary_push(&g->graph_ins, &g->num_graph_ins, t) != QNN_SUCCESS)
		return QNN_ERROR;
	if (t->is_graph_output &&
	    boundary_push(&g->graph_outs, &g->num_graph_outs, t) != QNN_SUCCESS)
		return QNN_ERROR;
	return QNN_SUCCESS;
}

qnn_node_obj_t *qnn_graph_node_append(qnn_graph_obj_t *g, const char *op_name,
        uint32_t op_kind, const qnn_op_attrs_t *attrs,
        qnn_tensor_obj_t *const *ins, uint32_t num_ins,
        qnn_tensor_obj_t *const *outs, uint32_t num_outs)
{
	qnn_node_obj_t *n;
	uint32_t i;

	if (!g || num_ins > QNN_NODE_MAX_INPUTS || num_outs > QNN_NODE_MAX_OUTPUTS)
		return NULL;
	if (g->num_nodes >= g->node_cap) {
		uint32_t nc = g->node_cap ? g->node_cap * 2u : 16u;
		qnn_node_obj_t *np = (qnn_node_obj_t *)
			realloc(g->nodes, (size_t)nc * sizeof(*np));

		if (!np)
			return NULL;
		g->nodes = np;
		g->node_cap = nc;
	}
	n = &g->nodes[g->num_nodes];
	memset(n, 0, sizeof(*n));
	n->op_name = qnn_strdup_safe(op_name ? op_name : "");
	n->op_kind = op_kind;
	if (attrs)
		n->attrs = *attrs;
	if (num_ins) {
		n->ins = (qnn_tensor_obj_t **)
			calloc(num_ins, sizeof(*n->ins));
		if (!n->ins) {
			free(n->op_name);
			return NULL;
		}
		for (i = 0; i < num_ins; i++) {
			n->ins[i] = ins ? ins[i] : NULL;
			if (ins[i])
				qnn_graph_track_tensor(g, ins[i]);
		}
		n->num_ins = num_ins;
	}
	if (num_outs) {
		n->outs = (qnn_tensor_obj_t **)
			calloc(num_outs, sizeof(*n->outs));
		if (!n->outs) {
			free(n->ins);
			free(n->op_name);
			n->ins = NULL;
			return NULL;
		}
		for (i = 0; i < num_outs; i++) {
			n->outs[i] = outs ? outs[i] : NULL;
			if (outs[i])
				qnn_graph_track_tensor(g, outs[i]);
		}
		n->num_outs = num_outs;
	}
	g->num_nodes++;
	/* A structural change invalidates any prior preparation. */
	g->finalized = false;
	return n;
}

void qnn_graph_release_internals(qnn_graph_obj_t *g)
{
	uint32_t i;

	if (!g)
		return;
	for (i = 0; i < g->num_nodes; i++) {
		free(g->nodes[i].op_name);
		free(g->nodes[i].ins);
		free(g->nodes[i].outs);
	}
	free(g->nodes);
	free(g->tensors);
	free(g->graph_ins);
	free(g->graph_outs);
	free(g->name);
	g->nodes = NULL;
	g->tensors = NULL;
	g->graph_ins = NULL;
	g->graph_outs = NULL;
	g->name = NULL;
	g->num_nodes = g->node_cap = 0;
	g->num_tensors = g->tensor_cap = 0;
	g->num_graph_ins = g->num_graph_outs = 0;
}

void qnn_graph_apply_tuning(qnn_graph_obj_t *g)
{
	if (!g || !g->ctx || !g->ctx->backend)
		return;
	/* Push the current perf tier; a no-op vote when the cDSP is down. */
	if (g->ctx->backend->npu_available)
		qnn_htp_power_vote(g->ctx->backend, g->htp.perf_mode);
}

/* ------------------------------------------------------------------ */
/* QnnGraph_* API                                                      */
/* ------------------------------------------------------------------ */
Qnn_ErrorHandle_t QnnGraph_create(Qnn_ContextHandle_t ctx, const char *graphName,
                                  Qnn_GraphHandle_t *graph)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	qnn_graph_obj_t *g;

	if (!c || !graph)
		return QNN_ERROR;
	g = qnn_graph_obj_create(c, graphName);
	if (!g)
		return QNN_ERROR;
	*graph = (Qnn_GraphHandle_t)(uintptr_t)g;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnGraph_retrieve(Qnn_ContextHandle_t ctx,
                                    const char *graphName,
                                    Qnn_GraphHandle_t *graph)
{
	qnn_context_obj_t *c = (qnn_context_obj_t *)
		qnn_obj_from_handle(ctx, QNN_OBJ_CONTEXT);
	uint32_t i;

	if (!c || !graphName || !graph)
		return QNN_ERROR;
	for (i = 0; i < c->num_graphs; i++) {
		qnn_graph_obj_t *g = c->graphs[i];

		if (g && g->name && strcmp(g->name, graphName) == 0) {
			*graph = (Qnn_GraphHandle_t)(uintptr_t)g;
			return QNN_SUCCESS;
		}
	}
	return QNN_ERROR;
}

Qnn_ErrorHandle_t QnnGraph_addNode(Qnn_GraphHandle_t graph, const char *opName,
        Qnn_TensorHandle_t *inTensors, uint32_t numIn,
        Qnn_TensorHandle_t *outTensors, uint32_t numOut,
        const Qnn_ConfigParam_t *cfg, uint32_t nCfg)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);
	const qnn_op_info_t *info;
	const char *keys[QNN_NODE_MAX_INPUTS + 8];
	qnn_tensor_obj_t *ins[QNN_NODE_MAX_INPUTS];
	qnn_tensor_obj_t *outs[QNN_NODE_MAX_OUTPUTS];
	qnn_op_attrs_t attrs;
	uint32_t i;

	if (!g || !opName)
		return QNN_ERROR;
	if (numIn > QNN_NODE_MAX_INPUTS || numOut > QNN_NODE_MAX_OUTPUTS)
		return QNN_ERROR;
	if ((numIn > 0 && !inTensors) || (numOut > 0 && !outTensors))
		return QNN_ERROR;
	if (nCfg > 0 && !cfg)
		return QNN_ERROR;

	info = qnn_op_lookup(opName);
	if (!info) {
		qnn_log(QNN_LOG_LVL_ERROR, "addNode: unknown op '%s'\n", opName);
		return QNN_ERROR;
	}
	if (numIn < info->min_ins || numIn > info->max_ins ||
	    numOut < info->min_outs || numOut > info->max_outs) {
		qnn_log(QNN_LOG_LVL_ERROR,
		        "addNode: '%s' io count in=%u out=%u out of range\n",
		        opName, numIn, numOut);
		return QNN_ERROR;
	}
	for (i = 0; i < nCfg && i < QNN_NODE_MAX_INPUTS + 8; i++)
		keys[i] = cfg[i].key;
	if (qnn_op_validate_config(opName, keys,
	        nCfg < QNN_NODE_MAX_INPUTS + 8 ? nCfg :
	        QNN_NODE_MAX_INPUTS + 8) != QNN_SUCCESS) {
		qnn_log(QNN_LOG_LVL_ERROR,
		        "addNode: '%s' missing required attribute\n", opName);
		return QNN_ERROR;
	}
	qnn_op_parse_attrs(cfg, nCfg, &attrs);

	/* Resolve handles to context tensors; every tensor must belong to this
	   graph's context (the QNN v2 scoping rule). */
	memset(ins, 0, sizeof(ins));
	memset(outs, 0, sizeof(outs));
	for (i = 0; i < numIn; i++) {
		qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
			qnn_obj_from_handle(inTensors[i], QNN_OBJ_TENSOR);

		if (!t || t->ctx != g->ctx)
			return QNN_ERROR;
		ins[i] = t;
	}
	for (i = 0; i < numOut; i++) {
		qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
			qnn_obj_from_handle(outTensors[i], QNN_OBJ_TENSOR);

		if (!t || t->ctx != g->ctx)
			return QNN_ERROR;
		outs[i] = t;
	}
	if (!qnn_graph_node_append(g, opName, info->kind, &attrs,
	                          ins, numIn, outs, numOut))
		return QNN_ERROR;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnGraph_finalize(Qnn_GraphHandle_t graph)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);
	qnn_backend_obj_t *b;
	char err[160];
	int htp_ok = 0;

	if (!g)
		return QNN_ERROR;
	if (g->finalized)
		return QNN_SUCCESS;
	if (g->num_nodes == 0) {
		qnn_log(QNN_LOG_LVL_ERROR, "finalize: graph '%s' is empty\n",
		        g->name ? g->name : "?");
		return QNN_ERROR;
	}
	b = g->ctx ? g->ctx->backend : NULL;
	qnn_graph_apply_tuning(g);

	if (b && b->npu_available) {
		htp_ok = (qnn_htp_prepare(g) == 0);
		if (!htp_ok)
			qnn_log(QNN_LOG_LVL_WARN,
			        "finalize: HTP prepare failed for '%s'; "
			        "using CPU reference\n", g->name);
	}
	err[0] = '\0';
	if (!g->ref)
		g->ref = qnn_ref_build(g, err, sizeof(err));
	if (!htp_ok && !g->ref) {
		qnn_log(QNN_LOG_LVL_ERROR,
		        "finalize: '%s' has no runnable path (%s)\n",
		        g->name ? g->name : "?", err);
		return QNN_ERROR;
	}
	g->finalized = true;
	qnn_log(QNN_LOG_LVL_INFO, "graph '%s' finalized (%u nodes, %s)\n",
	        g->name ? g->name : "?", g->num_nodes,
	        htp_ok ? "HTP" : "CPU-ref");
	return QNN_SUCCESS;
}

/* Find the graph boundary tensor matching `t` (same object, else same name). */
static qnn_tensor_obj_t *match_boundary(qnn_tensor_obj_t *const *list,
                                        uint32_t n, qnn_tensor_obj_t *t)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (list[i] == t)
			return list[i];
	if (t->name)
		for (i = 0; i < n; i++)
			if (list[i] && list[i]->name &&
			    strcmp(list[i]->name, t->name) == 0)
				return list[i];
	return NULL;
}

/* Refresh the graph boundary tensors' data pointers from the caller-supplied
   execute arrays (matched by identity or name). */
static void bind_io(qnn_graph_obj_t *g, Qnn_TensorHandle_t *in, uint32_t nin,
                    Qnn_TensorHandle_t *out, uint32_t nout)
{
	uint32_t i;

	for (i = 0; in && i < nin; i++) {
		qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
			qnn_obj_from_handle(in[i], QNN_OBJ_TENSOR);
		qnn_tensor_obj_t *m;

		if (!t)
			continue;
		m = match_boundary((qnn_tensor_obj_t *const *)g->graph_ins,
		                  g->num_graph_ins, t);
		if (m && m != t) {
			m->data = t->data;
			m->data_bytes = t->data_bytes;
		}
	}
	for (i = 0; out && i < nout; i++) {
		qnn_tensor_obj_t *t = (qnn_tensor_obj_t *)
			qnn_obj_from_handle(out[i], QNN_OBJ_TENSOR);
		qnn_tensor_obj_t *m;

		if (!t)
			continue;
		m = match_boundary((qnn_tensor_obj_t *const *)g->graph_outs,
		                  g->num_graph_outs, t);
		if (m && m != t) {
			m->data = t->data;
			m->data_bytes = t->data_bytes;
		}
	}
}

static Qnn_ErrorHandle_t graph_run(qnn_graph_obj_t *g,
                                   Qnn_TensorHandle_t *in, uint32_t nin,
                                   Qnn_TensorHandle_t *out, uint32_t nout,
                                   Qnn_ProfileHandle_t prof)
{
	qnn_backend_obj_t *b = g->ctx ? g->ctx->backend : NULL;
	char err[160];
	uint64_t t0, t1;
	int rc;

	if (!g->finalized)
		return QNN_ERROR;
	bind_io(g, in, nin, out, nout);

	/* Primary path: HTP dispatch to the cDSP over FastRPC. */
	if (b && b->npu_available && g->htp_prepared) {
		t0 = qnn_time_usec();
		rc = qnn_htp_execute(g,
		           (qnn_tensor_obj_t *const *)g->graph_ins,
		           g->num_graph_ins,
		           (qnn_tensor_obj_t *const *)g->graph_outs,
		           g->num_graph_outs);
		t1 = qnn_time_usec();
		if (rc == 0) {
			if (prof)
				qnn_profile_record(prof, 0,
				         g->name ? g->name : "graph", t1 - t0);
			return QNN_SUCCESS;
		}
		qnn_log(QNN_LOG_LVL_WARN,
		        "execute: HTP path returned %d; falling back to CPU\n",
		        rc);
	}

	/* Fallback / portable path: in-process CPU reference executor. */
	err[0] = '\0';
	if (!g->ref)
		g->ref = qnn_ref_build(g, err, sizeof(err));
	if (!g->ref) {
		qnn_log(QNN_LOG_LVL_ERROR, "execute: no plan for '%s' (%s)\n",
		        g->name ? g->name : "?", err);
		return QNN_ERROR;
	}
	t0 = qnn_time_usec();
	rc = qnn_ref_execute(g, g->ref,
	           (qnn_tensor_obj_t *const *)g->graph_ins, g->num_graph_ins,
	           (qnn_tensor_obj_t *const *)g->graph_outs, g->num_graph_outs,
	           prof, err, sizeof(err));
	t1 = qnn_time_usec();
	if (prof)
		qnn_profile_record(prof, 0, g->name ? g->name : "graph",
		                   t1 - t0);
	if (rc != 0) {
		qnn_log(QNN_LOG_LVL_ERROR, "execute: '%s' failed (%s)\n",
		        g->name ? g->name : "?", err);
		return QNN_ERROR;
	}
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnGraph_execute(Qnn_GraphHandle_t graph,
        Qnn_TensorHandle_t *inTensors, uint32_t numIn,
        Qnn_TensorHandle_t *outTensors, uint32_t numOut,
        Qnn_ProfileHandle_t prof)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);

	if (!g)
		return QNN_ERROR;
	return graph_run(g, inTensors, numIn, outTensors, numOut, prof);
}

Qnn_ErrorHandle_t QnnGraph_executeAsync(Qnn_GraphHandle_t graph,
        Qnn_TensorHandle_t *inTensors, uint32_t numIn,
        Qnn_TensorHandle_t *outTensors, uint32_t numOut,
        Qnn_ProfileHandle_t prof, Qnn_SignalHandle_t sig)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);
	Qnn_ErrorHandle_t rc;

	if (!g) {
		qnn_signal_mark(sig, 2);
		return QNN_ERROR;
	}
	/* With no cDSP command queue to hand the work to, execution completes
	   inline; the signal is marked before return, which satisfies the
	   async contract (the caller may still wait on it). */
	rc = graph_run(g, inTensors, numIn, outTensors, numOut, prof);
	qnn_signal_mark(sig, rc == QNN_SUCCESS ? 1 : 2);
	return rc;
}

Qnn_ErrorHandle_t QnnGraph_getProperty(Qnn_GraphHandle_t graph,
                                       const char *key, const char **val)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);

	if (!g || !key || !val)
		return QNN_ERROR;
	if (strcmp(key, "name") == 0)
		*val = g->name ? g->name : "";
	else if (strcmp(key, "num_nodes") == 0)
		*val = qnn_fmt_static("%u", g->num_nodes);
	else if (strcmp(key, "num_inputs") == 0)
		*val = qnn_fmt_static("%u", g->num_graph_ins);
	else if (strcmp(key, "num_outputs") == 0)
		*val = qnn_fmt_static("%u", g->num_graph_outs);
	else if (strcmp(key, "finalized") == 0)
		*val = g->finalized ? "true" : "false";
	else if (strcmp(key, "from_binary") == 0)
		*val = g->from_binary ? "true" : "false";
	else if (strcmp(key, "perf_mode") == 0)
		*val = qnn_fmt_static("%u", (unsigned)g->htp.perf_mode);
	else if (strcmp(key, "precision") == 0)
		*val = qnn_fmt_static("%u", (unsigned)g->htp.precision);
	else
		return QNN_ERROR;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnGraph_free(Qnn_GraphHandle_t graph)
{
	qnn_graph_obj_t *g = (qnn_graph_obj_t *)
		qnn_obj_from_handle(graph, QNN_OBJ_GRAPH);
	qnn_context_obj_t *c;
	uint32_t i;

	if (!g)
		return QNN_ERROR;
	c = g->ctx;
	/* Detach from the owning context so QnnContext_free won't revisit it. */
	if (c) {
		for (i = 0; i < c->num_graphs; i++) {
			if (c->graphs[i] == g) {
				memmove(&c->graphs[i], &c->graphs[i + 1],
				        (c->num_graphs - i - 1) *
				        sizeof(*c->graphs));
				c->num_graphs--;
				break;
			}
		}
	}
	if (c && c->backend && c->backend->npu_available)
		qnn_htp_graph_release(g);
	if (g->ref)
		qnn_ref_free(g->ref);
	qnn_graph_release_internals(g);
	qnn_obj_free(g);
	return QNN_SUCCESS;
}
