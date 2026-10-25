/* Minimal public-API regression followed by a correctness-gated HTP benchmark. */
#include <adrenonpu/qnn_core.h>
#include <ewoksys/fine_cnt.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TENSORS 4u
#define MAX_BYTES (4u * 1024u * 1024u)
#define RELOADS 2u
#define WARMUP 10u
#define TOLERANCE 8u
#define REQUIRE(expr) do { if (!(expr)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #expr); goto done; \
} } while (0)

typedef struct {
    Qnn_TensorHandle_t tensor[MAX_TENSORS];
    uint32_t count, bytes[MAX_TENSORS], total;
} boundary_t;

static uint64_t now_ticks(void) {
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#endif
    return fine_cnt_read();
}

static uint32_t counter_frequency(void) {
#if defined(__aarch64__)
    uint64_t hz;
    /* Q8B firmware programs CNTFRQ to match CNTVCT (19.2 MHz).
     * Read it directly, without depending on the shared-clock ABI version. */
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz) :: "memory");
    return hz <= UINT32_MAX ? (uint32_t)hz : 0;
#else
    return kernel_tic_fine_hz();
#endif
}

/* Strict decimal parsing, including overflow checks, independent of libc errno. */
static int number(const char *s, uint64_t max, uint64_t *out) {
    uint64_t value = 0;
    if (!s || !*s) return -1;
    for (; *s; s++) {
        unsigned digit = (unsigned)(*s - '0');
        if (digit > 9 || digit > max || value > (max - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

static int property(Qnn_GraphHandle_t g, const char *key, uint64_t *out) {
    const char *value = NULL;
    return QnnGraph_getProperty(g, key, &value) || number(value, UINT64_MAX, out);
}

static int boundaries(Qnn_GraphHandle_t g, int output, boundary_t *b) {
    uint32_t count = 0;
    int (*get)(Qnn_GraphHandle_t, Qnn_TensorHandle_t *, uint32_t *) =
        output ? QnnGraph_getOutputTensors : QnnGraph_getInputTensors;
    if (get(g, NULL, &count) || !count || count > MAX_TENSORS) return -1;
    /* Too-small arrays must report the required count without partial writes. */
    Qnn_TensorHandle_t guard[MAX_TENSORS] = {0};
    uint32_t small = count - 1;
    if (get(g, guard, &small) != QNN_ERROR || small != count) return -1;
    for (unsigned i = 0; i < MAX_TENSORS; i++) if (guard[i]) return -1;
    b->count = MAX_TENSORS;
    b->total = 0;
    if (get(g, b->tensor, &b->count) || b->count != count) return -1;
    for (uint32_t i = 0; i < count; i++) {
        Qnn_Shape_t shape;
        Qnn_DataType_t dtype;
        Qnn_QuantParams_t qp;
        if (QnnTensor_getShape(b->tensor[i], &shape) ||
            QnnTensor_getDataType(b->tensor[i], &dtype) ||
            QnnTensor_getQuantParams(b->tensor[i], &qp) ||
            dtype != QNN_DATATYPE_UINT8 || !shape.rank || shape.rank > 8) return -1;
        uint32_t size = 1;
        for (uint32_t j = 0; j < shape.rank; j++) {
            if (!shape.dims[j] || size > MAX_BYTES / shape.dims[j]) return -1;
            size *= shape.dims[j];
        }
        if (size > MAX_BYTES - b->total) return -1;
        b->bytes[i] = size;
        b->total += size;
    }
    return 0;
}

static int bind(boundary_t *b, uint8_t *data) {
    for (uint32_t i = 0, offset = 0; i < b->count; offset += b->bytes[i++])
        if (QnnTensor_setData(b->tensor[i], data + offset, b->bytes[i])) return -1;
    return 0;
}

static int execute(Qnn_GraphHandle_t g, boundary_t *in, boundary_t *out,
        uint8_t *input, const uint8_t *expected, uint8_t *output,
        uint64_t *ap_ticks, uint64_t *dsp_us, unsigned *max_error) {
    if (bind(in, input) || bind(out, output)) return -1;
    /* Every byte starts farther than tolerance from its reference. */
    for (uint32_t i = 0; i < out->total; i++) output[i] = expected[i] < 128 ? 255 : 0;
    uint64_t start = now_ticks();
    int rc = QnnGraph_execute(g, in->tensor, in->count, out->tensor, out->count, 0);
    uint64_t end = now_ticks();
    if (rc) {
        uint64_t completed = 0;
        property(g, "native.executions", &completed);
        printf("FAIL graphExecute rc=%d completed_in_context=%llu\n",
            rc, (unsigned long long)completed);
        return -1;
    }
    if (end <= start || property(g, "native.last_dsp_us", dsp_us)) return -1;
    *ap_ticks = end - start;
    for (uint32_t i = 0; i < out->total; i++) {
        int delta = (int)output[i] - (int)expected[i];
        unsigned error = (unsigned)(delta < 0 ? -delta : delta);
        if (error > *max_error) *max_error = error;
        if (error > TOLERANCE) {
            printf("FAIL output byte=%u actual=%u expected=%u error=%u\n",
                i, output[i], expected[i], error);
            return -1;
        }
    }
    return 0;
}

static int compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void report(const char *name, uint64_t *values, unsigned n, uint64_t macs, uint32_t hz) {
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) total += values[i];
    qsort(values, n, sizeof(*values), compare);
    double us_per_tick = 1000000.0 / hz;
    printf("BENCH %s n=%u mean_us=%.3f p50_us=%.3f p95_us=%.3f min_us=%.3f max_us=%.3f",
        name, n, (double)total * us_per_tick / n,
        values[(n * 50u + 99u) / 100u - 1] * us_per_tick,
        values[(n * 95u + 99u) / 100u - 1] * us_per_tick,
        values[0] * us_per_tick, values[n - 1] * us_per_tick);
    if (total) {
        printf(" fps=%.3f", (double)n * hz / total);
        if (macs) printf(" effective_GOPS=%.3f", (double)macs * 2.0 * n * hz / ((double)total * 1000000000.0));
    }
    printf("\n");
}

int main(int argc, char **argv) {
    uint64_t samples, iterations, macs = 0;
    if ((argc != 7 && argc != 8) || number(argv[5], 1000, &samples) || !samples ||
        number(argv[6], 10000, &iterations) ||
        (argc == 8 && number(argv[7], 1000000000000ull, &macs))) {
        printf("usage: npudemo context.bin graph input.bin expected.bin samples iterations [MACs_per_graph]\n"
               "U8 only; tolerance=8 LSB; iterations=0 runs API regression only.\n");
        return 2;
    }
    int rc = 1, binary_size = 0, input_size = 0, expected_size = 0;
    uint8_t *binary = vfs_readfile(argv[1], &binary_size);
    uint8_t *input = vfs_readfile(argv[3], &input_size);
    uint8_t *expected = vfs_readfile(argv[4], &expected_size);
    uint8_t *output = NULL, *roundtrip = NULL;
    uint64_t *ap = NULL, *dsp = NULL;
    Qnn_BackendHandle_t backend = 0;
    Qnn_ContextHandle_t context = 0;
    Qnn_GraphHandle_t graph = 0;
    unsigned max_error = 0;
    REQUIRE(binary && input && expected && binary_size > 0 && input_size > 0 && expected_size > 0);
    REQUIRE(binary_size <= (int)MAX_BYTES && input_size <= (int)MAX_BYTES && expected_size <= (int)MAX_BYTES);
    output = malloc((size_t)expected_size);
    roundtrip = malloc((size_t)binary_size);
    if (iterations) {
        ap = malloc((size_t)iterations * sizeof(*ap));
        dsp = malloc((size_t)iterations * sizeof(*dsp));
        REQUIRE(ap && dsp);
    }
    REQUIRE(output && roundtrip);
    uint32_t counter_hz = counter_frequency();
    REQUIRE(counter_hz != 0);
    Qnn_ConfigParam_t backend_cfg = {"execution_backend", "htp_native"};
    Qnn_ConfigParam_t context_cfg = {"graph_name", argv[2]};
    REQUIRE(QnnBackend_create(&backend_cfg, 1, &backend) == QNN_SUCCESS);
    bool fallback = true;
    REQUIRE(QnnProperty_hasCapability(backend, "npu.cpu_reference", &fallback) == QNN_SUCCESS && !fallback);
    printf("HTP native; CPU fallback disabled; counter_hz=%u samples=%u reloads=%u tolerance=%u\n",
        counter_hz, (unsigned)samples, RELOADS, TOLERANCE);
    boundary_t in = {0}, out = {0};
    for (unsigned cycle = 0; cycle < RELOADS; cycle++) {
        REQUIRE(QnnContext_createFromBinary(backend, 0, binary, binary_size,
            &context_cfg, 1, &context) == QNN_SUCCESS);
        uint64_t size = 0, count = 0;
        REQUIRE(QnnContext_getBinarySize(context, &size) == QNN_SUCCESS && size == (uint64_t)binary_size);
        REQUIRE(QnnContext_getBinary(context, roundtrip, size - 1) == QNN_ERROR);
        REQUIRE(QnnContext_getBinary(context, roundtrip, size) == QNN_SUCCESS && !memcmp(binary, roundtrip, (size_t)size));
        REQUIRE(QnnGraph_retrieve(context, argv[2], &graph) == QNN_SUCCESS);
        const char *name = NULL;
        REQUIRE(QnnGraph_getProperty(graph, "execution_backend", &name) == QNN_SUCCESS &&
            !strcmp(name, "official-htp-native"));
        REQUIRE(boundaries(graph, 0, &in) == 0 && boundaries(graph, 1, &out) == 0);
        REQUIRE((uint64_t)in.total * samples == (uint64_t)input_size &&
            (uint64_t)out.total * samples == (uint64_t)expected_size);
        REQUIRE(QnnGraph_free(graph) == QNN_ERROR);
        REQUIRE(QnnTensor_free(in.tensor[0]) == QNN_ERROR);
        REQUIRE(QnnBackend_free(backend) == QNN_ERROR);
        REQUIRE(bind(&in, input) == 0 && bind(&out, output) == 0);
        REQUIRE(QnnTensor_setData(in.tensor[0], input, in.bytes[0] + 1) == QNN_ERROR);
        REQUIRE(QnnGraph_execute(graph, in.tensor, in.count - 1, out.tensor, out.count, 0) == QNN_ERROR);
        REQUIRE(QnnGraph_execute(graph, in.tensor, in.count, out.tensor, out.count - 1, 0) == QNN_ERROR);
        if (in.count > 1) {
            Qnn_TensorHandle_t duplicate[MAX_TENSORS];
            memcpy(duplicate, in.tensor, sizeof(duplicate));
            duplicate[1] = duplicate[0];
            REQUIRE(QnnGraph_execute(graph, duplicate, in.count, out.tensor, out.count, 0) == QNN_ERROR);
        }
        REQUIRE(property(graph, "native.executions", &count) == 0 && count == 0);
        for (unsigned i = 0; i < samples; i++) {
            uint64_t a, d;
            REQUIRE(execute(graph, &in, &out, input + (size_t)i * in.total,
                expected + (size_t)i * out.total, output, &a, &d, &max_error) == 0);
        }
        REQUIRE(property(graph, "native.executions", &count) == 0 && count == samples);
        printf("API reload=%u PASS executions=%u input_bytes=%u output_bytes=%u max_error=%u\n",
            cycle + 1, (unsigned)samples, in.total, out.total, max_error);
        if (cycle + 1 < RELOADS) {
            REQUIRE(QnnContext_free(context) == QNN_SUCCESS);
            context = 0;
            graph = 0;
        }
    }
    printf("API PASS: binary roundtrip, boundary/capacity, borrowed handles, live-child protection,\n"
           "  bad sizes/counts, execution counter, reference outputs and context reload\n");
    if (iterations) {
        for (unsigned i = 0; i < WARMUP; i++) {
            uint64_t a, d;
            unsigned sample = i % (unsigned)samples;
            REQUIRE(execute(graph, &in, &out, input + (size_t)sample * in.total,
                expected + (size_t)sample * out.total, output, &a, &d, &max_error) == 0);
        }
        uint64_t start = now_ticks();
        for (unsigned i = 0; i < iterations; i++) {
            unsigned sample = i % (unsigned)samples;
            REQUIRE(execute(graph, &in, &out, input + (size_t)sample * in.total,
                expected + (size_t)sample * out.total, output, &ap[i], &dsp[i], &max_error) == 0);
        }
        uint64_t end = now_ticks(), count = 0;
        REQUIRE(end > start);
        REQUIRE(property(graph, "native.executions", &count) == 0 && count == samples + WARMUP + iterations);
        printf("BENCH warmup=%u measured=%u MACs_per_graph=%llu ops_per_MAC=2 max_error=%u\n",
            WARMUP, (unsigned)iterations, (unsigned long long)macs, max_error);
        report("AP_graphExecute", ap, (unsigned)iterations, macs, counter_hz);
        report("DSP_graphExecute", dsp, (unsigned)iterations, macs, 1000000);
        printf("BENCH checked_loop_us=%.3f checked_loop_fps=%.3f\n",
            (double)(end - start) * 1000000.0 / counter_hz,
            (double)iterations * counter_hz / (end - start));
    }
    REQUIRE(QnnContext_free(context) == QNN_SUCCESS);
    context = 0;
    REQUIRE(QnnBackend_free(backend) == QNN_SUCCESS);
    backend = 0;
    printf("PASS: regression%s and resource cleanup\n", iterations ? ", benchmark" : "");
    rc = 0;
done:
    if (context && QnnContext_free(context) != QNN_SUCCESS) {
        printf("FAIL: context cleanup; handle retained for recovery\n"); rc = 1;
    }
    if (backend && QnnBackend_free(backend) != QNN_SUCCESS) {
        printf("FAIL: backend cleanup; backing retained for recovery\n"); rc = 1;
    }
    free(binary); free(input); free(expected); free(output); free(roundtrip);
    free(ap); free(dsp);
    return rc;
}
