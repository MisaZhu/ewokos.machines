/* Local QNN C API -> official QAIRT opaque contexts. No private graph ABI
 * and no CPU fallback. A backend owns one transport and one live context. */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_model.h>
#include <adrenonpu/npu_cdsp.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

struct qnn_native_backend {
    uint32_t session, handle;
    unsigned epoch;
    bool probe_owned, dirty, closing, ready;
    qnn_context_obj_t *context;
};
struct qnn_native_context {
    npu_model_info_t model;
    uint8_t *binary, *input, *output;
    uint32_t binary_size;
    bool closing, execution_failed;
    uint64_t executions, last_dsp_us;
};
static qnn_backend_obj_t *owner;
int qnn_native_transport_owned(void) { return owner != NULL; }

static int live(qnn_backend_obj_t *b)
{
    struct qnn_native_backend *s = b ? b->native : NULL;
    return s && owner == b && s->ready && !s->closing && s->handle &&
        s->epoch == frpc_mapping_epoch() && s->session == frpc_session_get();
}

static int backend_open(qnn_backend_obj_t *b)
{
    if (!b || !b->native_mode) return QNN_ERROR;
    if (b->native) return live(b) && !b->native->dirty ? QNN_SUCCESS : QNN_ERROR;
    /* Do not adopt an existing diagnostic/default backend's session. */
    if (owner || npu_cdsp_booted() || frpc_session_get()) return QNN_ERROR;
    struct qnn_native_backend *s = calloc(1, sizeof(*s));
    if (!s) return QNN_ERROR;
    b->native = s;
    owner = b;
    s->probe_owned = true;
    b->cdsp_owned = 1; /* includes failed-probe cache/partial CREATE cleanup */
    b->npu_probe_rc = npu_cdsp_probe_unsigned();
    if (b->npu_probe_rc != NPU_PROBE_OK) return QNN_ERROR;
    s->session = b->frpc_session = frpc_session_get();
    s->epoch = frpc_mapping_epoch();
    if (!s->session || frpc_open(s->session, NPU_SDK_NATIVE_URI, &s->handle) ||
        !s->handle || s->handle == UINT32_MAX) return QNN_ERROR;
    npu_sdk_dsp_info_t info = {0};
    if (npu_sdk_dsp_query(s->session, s->handle, &info) || info.status ||
        info.stage != 2 || info.backend_id != 6 || info.major != 2 || info.minor < 40)
        return QNN_ERROR;
    s->ready = true;
    b->npu_available = 1;
    return QNN_SUCCESS;
}

int qnn_native_backend_in_use(qnn_backend_obj_t *b)
{
    return b && b->native && b->native->context != NULL;
}

int qnn_native_backend_close(qnn_backend_obj_t *b)
{
    struct qnn_native_backend *s = b ? b->native : NULL;
    if (!s) return QNN_SUCCESS;
    if (s->context) return QNN_ERROR;
    s->closing = true;
    b->npu_available = 0;
    if (s->handle && s->epoch == frpc_mapping_epoch()) {
        int rc = frpc_close(s->session, s->handle);
        if (rc && s->epoch == frpc_mapping_epoch()) return QNN_ERROR;
    }
    s->handle = 0;
    if (s->probe_owned && npu_cdsp_shutdown()) return QNN_ERROR;
    s->probe_owned = false;
    b->cdsp_owned = 0;
    b->frpc_session = 0;
    if (owner == b) owner = NULL;
    free(s);
    b->native = NULL;
    return QNN_SUCCESS;
}

/* Raw dtype and quantization stay in DSP metadata. The local tensor view
 * translates scale*(q+offset) to scale*(q-zeroPoint), without re-quantizing. */
static int tensor_type(const npu_model_tensor_t *m, Qnn_DataType_t *dt,
                       Qnn_QuantParams_t *qp)
{
    switch (m->data_type) {
    case 0x0108: case 0x0408: *dt = QNN_DATATYPE_UINT8; break;
    case 0x0008: case 0x0308: *dt = QNN_DATATYPE_INT8; break;
    case 0x0116: case 0x0416: *dt = QNN_DATATYPE_UINT16; break;
    case 0x0016: case 0x0316: *dt = QNN_DATATYPE_INT16; break;
    case 0x0132: case 0x0432: *dt = QNN_DATATYPE_UINT32; break;
    case 0x0032: case 0x0332: *dt = QNN_DATATYPE_INT32; break;
    case 0x0216: *dt = QNN_DATATYPE_FLOAT16; break;
    case 0x0232: *dt = QNN_DATATYPE_FLOAT32; break;
    default: return QNN_ERROR;
    }
    *qp = (Qnn_QuantParams_t){1.0f, 0};
    if ((m->data_type & 0xff00u) == 0x0300u || (m->data_type & 0xff00u) == 0x0400u) {
        if (m->quant_encoding != 0 || !isfinite(m->scale) || m->scale <= 0 ||
            m->quant_offset == INT32_MIN) return QNN_ERROR;
        qp->scale = m->scale;
        qp->zeroPoint = -m->quant_offset;
    }
    uint64_t bytes = qnn_dtype_size(*dt);
    if (!m->rank || m->rank > 8 || !m->name[0] ||
        !memchr(m->name, 0, sizeof(m->name))) return QNN_ERROR;
    for (uint32_t i = 0; i < m->rank; i++) {
        if (!m->dimensions[i] || bytes > NPU_MODEL_MAX_BYTES / m->dimensions[i])
            return QNN_ERROR;
        bytes *= m->dimensions[i];
    }
    return bytes == m->bytes ? QNN_SUCCESS : QNN_ERROR;
}

/* Validate even when the transport already checked its wire reply. */
static int metadata_valid(const npu_model_info_t *m)
{
    if (m->abi != NPU_MODEL_ABI || !m->graph_name[0] ||
        !memchr(m->graph_name, 0, sizeof(m->graph_name)) ||
        !m->num_inputs || m->num_inputs > NPU_MODEL_MAX_TENSORS ||
        !m->num_outputs || m->num_outputs > NPU_MODEL_MAX_TENSORS ||
        !m->input_bytes || m->input_bytes > NPU_MODEL_MAX_BYTES ||
        !m->output_bytes || m->output_bytes > NPU_MODEL_MAX_BYTES) return 0;
    for (unsigned dir = 0; dir < 2; dir++) {
        const npu_model_tensor_t *t = dir ? m->outputs : m->inputs;
        uint32_t count = dir ? m->num_outputs : m->num_inputs;
        uint32_t total = dir ? m->output_bytes : m->input_bytes, off = 0;
        for (uint32_t i = 0; i < count; i++) {
            Qnn_DataType_t dt; Qnn_QuantParams_t qp;
            if (tensor_type(&t[i], &dt, &qp) || t[i].offset != off ||
                t[i].bytes > total - off) return 0;
            off += t[i].bytes;
        }
        if (off != total) return 0;
    }
    return 1;
}

static int boundaries(qnn_context_obj_t *c, qnn_graph_obj_t *g,
                       const npu_model_tensor_t *m, uint32_t count, bool output)
{
    for (uint32_t i = 0; i < count; i++) {
        Qnn_DataType_t dt;
        Qnn_QuantParams_t qp;
        if (tensor_type(&m[i], &dt, &qp)) return QNN_ERROR;
        for (uint32_t j = 0; j < i; j++)
            if (!strcmp(m[i].name, m[j].name)) return QNN_ERROR;
        Qnn_Shape_t shape = {m[i].rank, (uint32_t *)m[i].dimensions};
        if (!qnn_tensor_create_common(c, g, m[i].name,
                    output ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_APP_WRITE,
                    dt, &shape, &qp)) return QNN_ERROR;
    }
    return QNN_SUCCESS;
}

static int remote_unload(qnn_backend_obj_t *b)
{
    struct qnn_native_backend *s = b->native;
    if (!s->dirty) return QNN_SUCCESS;
    if (s->epoch != frpc_mapping_epoch()) { s->dirty = false; return QNN_SUCCESS; }
    npu_sdk_dsp_info_t info = {0};
    if (!live(b) || npu_model_unload(s->session, s->handle, &info) || info.status)
        return QNN_ERROR;
    s->dirty = false;
    return QNN_SUCCESS;
}

static void context_storage_free(struct qnn_native_context *n)
{
    if (!n) return;
    free(n->binary); free(n->input); free(n->output); free(n);
}

int qnn_native_context_load(qnn_context_obj_t *c, const void *binary,
                            uint64_t size, const char *graph_name)
{
    qnn_backend_obj_t *b = c ? c->backend : NULL;
    if (!b || !b->native_mode || !binary || !size || size > NPU_MODEL_MAX_BYTES ||
        !graph_name || strlen(graph_name) >= NPU_MODEL_NAME_BYTES ||
        qnn_native_backend_in_use(b)) return QNN_ERROR;
    struct qnn_native_context *n = calloc(1, sizeof(*n));
    if (!n) return QNN_ERROR;
    n->binary = malloc((size_t)size);
    if (!n->binary) { context_storage_free(n); return QNN_ERROR; }
    memcpy(n->binary, binary, (size_t)size);
    n->binary_size = (uint32_t)size;
    if (backend_open(b)) { context_storage_free(n); return QNN_ERROR; }
    struct qnn_native_backend *s = b->native;
    s->dirty = true; /* timeout/malformed reply may still have loaded a context */
    int rc = npu_model_load(s->session, s->handle, n->binary, n->binary_size,
                            graph_name, &n->model);
    if (rc || n->model.info.status || !metadata_valid(&n->model)) goto fail;
    n->input = malloc(n->model.input_bytes);
    n->output = malloc(n->model.output_bytes);
    if (!n->input || !n->output) goto fail;
    qnn_graph_obj_t *g = qnn_graph_obj_create(c, n->model.graph_name);
    if (!g) goto fail;
    g->native_graph = true;
    if (boundaries(c, g, n->model.inputs, n->model.num_inputs, false) ||
        boundaries(c, g, n->model.outputs, n->model.num_outputs, true)) goto fail;
    g->from_binary = true;
    g->finalized = c->finalized = true;
    c->native = n;
    s->context = c;
    return QNN_SUCCESS;
fail:
    /* AP staging is never referenced by the DSP after an invoke returns.
     * The backend retains module/dirty ownership if unload itself fails. */
    (void)remote_unload(b);
    context_storage_free(n);
    return QNN_ERROR;
}

int qnn_native_context_release(qnn_context_obj_t *c)
{
    if (!c || !c->native) return QNN_SUCCESS;
    c->native->closing = true;
    qnn_backend_obj_t *b = c->backend;
    if (!b || !b->native || b->native->context != c || remote_unload(b))
        return QNN_ERROR;
    b->native->context = NULL;
    context_storage_free(c->native);
    c->native = NULL;
    return QNN_SUCCESS;
}

static int collect(qnn_context_obj_t *c, qnn_tensor_obj_t *const *expected,
                    uint32_t count, Qnn_TensorHandle_t *given, uint32_t n,
                    qnn_tensor_obj_t **resolved, const npu_model_tensor_t *metadata)
{
    if (n != count || !given) return QNN_ERROR;
    bool seen[NPU_MODEL_MAX_TENSORS] = {0};
    for (uint32_t i = 0; i < n; i++) {
        qnn_tensor_obj_t *t = qnn_obj_from_handle(given[i], QNN_OBJ_TENSOR);
        if (!t || t->ctx != c || !t->data) return QNN_ERROR;
        uint32_t k;
        for (k = 0; k < count; k++)
            if (t == expected[k] || (t->name && !strcmp(t->name, expected[k]->name))) break;
        if (k == count || seen[k]) return QNN_ERROR;
        qnn_tensor_obj_t *e = expected[k];
        Qnn_DataType_t dt; Qnn_QuantParams_t qp;
        if (tensor_type(&metadata[k], &dt, &qp) || t->dtype != dt ||
            t->ttype != e->ttype || t->rank != metadata[k].rank ||
            memcmp(t->dims, metadata[k].dimensions, t->rank * sizeof(uint32_t)) ||
            t->quant.scale != qp.scale || t->quant.zeroPoint != qp.zeroPoint ||
            t->data_bytes != metadata[k].bytes) return QNN_ERROR;
        resolved[k] = t;
        seen[k] = true;
    }
    return QNN_SUCCESS;
}

int qnn_native_execute(qnn_graph_obj_t *g, Qnn_TensorHandle_t *in, uint32_t nin,
                        Qnn_TensorHandle_t *out, uint32_t nout, Qnn_ProfileHandle_t prof)
{
    qnn_context_obj_t *c = g ? g->ctx : NULL;
    struct qnn_native_context *n = c ? c->native : NULL;
    qnn_backend_obj_t *b = c ? c->backend : NULL;
    if (!n || !g->native_graph || n->closing || n->execution_failed || !live(b) ||
        b->native->context != c) return QNN_ERROR;
    qnn_tensor_obj_t *inputs[NPU_MODEL_MAX_TENSORS], *outputs[NPU_MODEL_MAX_TENSORS];
    if (collect(c, g->graph_ins, g->num_graph_ins, in, nin, inputs, n->model.inputs) ||
        collect(c, g->graph_outs, g->num_graph_outs, out, nout, outputs, n->model.outputs)) return QNN_ERROR;
    for (uint32_t i = 0; i < nin; i++)
        memcpy(n->input + n->model.inputs[i].offset, inputs[i]->data, n->model.inputs[i].bytes);
    npu_model_execution_t result = {0};
    uint64_t start = qnn_time_usec();
    int rc = npu_model_execute(b->native->session, b->native->handle,
            n->input, n->model.input_bytes, n->output, n->model.output_bytes, &result);
    if (rc || result.info.status || result.executions != 1) { n->execution_failed = true; return QNN_ERROR; }
    for (uint32_t i = 0; i < nout; i++)
        memcpy(outputs[i]->data, n->output + n->model.outputs[i].offset, n->model.outputs[i].bytes);
    n->last_dsp_us = result.execute_us;
    n->executions++;
    if (prof) qnn_profile_record(prof, 0, "native HTP execute RPC", qnn_time_usec() - start);
    return QNN_SUCCESS;
}

int qnn_native_binary(qnn_context_obj_t *c, uint8_t *buf, uint64_t capacity, uint64_t *size)
{
    if (!c || !c->native || c->native->closing || !size) return QNN_ERROR;
    *size = c->native->binary_size;
    if (!buf) return QNN_SUCCESS;
    if (capacity < *size) return QNN_ERROR;
    memcpy(buf, c->native->binary, (size_t)*size);
    return QNN_SUCCESS;
}

int qnn_native_property(qnn_graph_obj_t *g, const char *key, const char **value)
{
    struct qnn_native_context *n = g && g->ctx ? g->ctx->native : NULL;
    if (!n || !key || !value) return QNN_ERROR;
    if (!strcmp(key, "native.executions")) *value = qnn_fmt_static("%llu", (unsigned long long)n->executions);
    else if (!strcmp(key, "native.last_dsp_us")) *value = qnn_fmt_static("%llu", (unsigned long long)n->last_dsp_us);
    else return QNN_ERROR;
    return QNN_SUCCESS;
}
