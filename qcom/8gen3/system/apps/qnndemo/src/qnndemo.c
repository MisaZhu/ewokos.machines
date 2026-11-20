/*
 * qnndemo - MNIST handwritten-digit classification on the Q8B HTP NPU.
 *
 * This is a small, end-to-end application around libadrenonpu's public QNN C
 * API, not a synthetic operator test.  It loads the trained MNIST context
 * binary produced by qairt_mnist_build.py, classifies a corpus of 28x28 U8
 * images on the real Hexagon V68 DSP (execution_backend=htp_native, CPU
 * fallback explicitly disabled), compares every output against an independent
 * integer reference, and then reports AP/DSP latency, throughput and effective
 * convolution throughput.
 *
 * Timing follows the rules used by machines/qcom/8gen3/system/bin/npudemo:
 *   - AP intervals use the raw free-running CNTVCT_EL0 counter.
 *   - DSP intervals are the adapter's native.last_dsp_us (graphExecute only).
 *   - The checked FPS window also includes binding, output poisoning and
 *     reference comparison; load, warmup and reporting are excluded.
 *
 * The corpus is 100 MNIST test images [128,228) by default.  Point --input,
 * --labels and --reference at a larger corpus to benchmark more samples.
 */
#include <adrenonpu/qnn_core.h>
#include <ewoksys/fine_cnt.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/vfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_TENSORS       4u
#define MAX_BYTES         (4u * 1024u * 1024u)
#define MAX_SAMPLES       1000u
#define MAX_ITERATIONS    10000u
#define MAX_SHOW          64u
#define MNIST_PIXELS      784u
#define MNIST_CLASSES     10u
#define MNIST_MACS        786560ull
#define TOLERANCE         8u
#define DEFAULT_WARMUP    10u

typedef struct {
	const char *context;
	const char *graph;
	const char *input;
	const char *labels;
	const char *reference;
	const char *json;
	uint32_t samples;      /* 0 = infer from the input file */
	uint32_t show;
	uint32_t warmup;
	uint32_t iterations;   /* 0 = regression/demo only */
	int quiet;
	int confusion;
} options_t;

typedef struct {
	Qnn_TensorHandle_t tensor[MAX_TENSORS];
	uint32_t count;
	uint32_t bytes[MAX_TENSORS];
	uint32_t total;
} boundary_t;

typedef struct {
	double mean;
	double p50;
	double p95;
	double min;
	double max;
} stats_t;

static uint64_t now_ticks(void)
{
#if defined(__aarch64__)
	__asm__ volatile("isb" ::: "memory");
#endif
	return fine_cnt_read();
}

static uint32_t counter_frequency(void)
{
#if defined(__aarch64__)
	uint64_t hz;

	/* Q8B firmware programs CNTFRQ to match CNTVCT (19.2 MHz).  Read the
	 * architectural register directly, as npudemo does. */
	__asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz) :: "memory");
	return hz <= UINT32_MAX ? (uint32_t)hz : 0;
#else
	return kernel_tic_fine_hz();
#endif
}

static void usage(const char *argv0)
{
	printf("usage: %s [options]\n"
	       "  --context PATH     QNN context binary (default /apps/qnndemo/res/q8b_mnist.bin)\n"
	       "  --graph NAME       graph name inside the context (default q8b_mnist_u8)\n"
	       "  --input PATH       contiguous U8 NHWC images, 784 B each (default /apps/qnndemo/res/input.bin)\n"
	       "  --labels PATH      one U8 class label per sample (default /apps/qnndemo/res/labels.bin)\n"
	       "  --reference PATH   integer reference output, 10 B per sample (default /apps/qnndemo/res/reference.bin)\n"
	       "  --samples N        corpus size (default: input_size / 784)\n"
	       "  --show N           visualize the first N digits (default 10)\n"
	       "  --warmup N         checked warmup executions (default 10)\n"
	       "  --bench N          timed executions, 0 disables the benchmark (default 1000)\n"
	       "  --json PATH        write the metrics summary as JSON\n"
	       "  --confusion        print the 10x10 confusion matrix\n"
	       "  --quiet            suppress per-image output and ASCII art\n"
	       "  --help             this text\n", argv0);
}

static int parse_u32(const char *s, uint32_t max, uint32_t *out)
{
	uint64_t value = 0;

	if (!s || !*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		value = value * 10 + (uint64_t)(*s - '0');
		if (value > max)
			return -1;
	}
	*out = (uint32_t)value;
	return 0;
}

static int parse_args(int argc, char **argv, options_t *o)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage(argv[0]);
			exit(0);
		} else if (!strcmp(a, "--quiet")) {
			o->quiet = 1;
			continue;
		} else if (!strcmp(a, "--confusion")) {
			o->confusion = 1;
			continue;
		} else if (!strcmp(a, "--context")) {
			if (!v) return -1;
			o->context = v;
		} else if (!strcmp(a, "--graph")) {
			if (!v) return -1;
			o->graph = v;
		} else if (!strcmp(a, "--input")) {
			if (!v) return -1;
			o->input = v;
		} else if (!strcmp(a, "--labels")) {
			if (!v) return -1;
			o->labels = v;
		} else if (!strcmp(a, "--reference")) {
			if (!v) return -1;
			o->reference = v;
		} else if (!strcmp(a, "--json")) {
			if (!v) return -1;
			o->json = v;
		} else if (!strcmp(a, "--samples")) {
			uint32_t x;
			if (!v || parse_u32(v, MAX_SAMPLES, &x) || x == 0) return -1;
			o->samples = x;
		} else if (!strcmp(a, "--show")) {
			uint32_t x;
			if (!v || parse_u32(v, MAX_SHOW, &x)) return -1;
			o->show = x;
		} else if (!strcmp(a, "--warmup")) {
			uint32_t x;
			if (!v || parse_u32(v, MAX_SAMPLES, &x)) return -1;
			o->warmup = x;
		} else if (!strcmp(a, "--bench")) {
			uint32_t x;
			if (!v || parse_u32(v, MAX_ITERATIONS, &x)) return -1;
			o->iterations = x;
		} else {
			return -1;
		}
		i++; /* every option above consumes one value */
	}
	return 0;
}

static int property_u64(Qnn_GraphHandle_t graph, const char *key, uint64_t *out)
{
	const char *s = NULL;
	uint64_t value = 0;

	if (QnnGraph_getProperty(graph, key, &s) != QNN_SUCCESS || !s || !*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		value = value * 10 + (uint64_t)(*s - '0');
	}
	*out = value;
	return 0;
}

static int get_boundaries(Qnn_GraphHandle_t graph, int output, boundary_t *b)
{
	uint32_t count = 0;
	int (*get)(Qnn_GraphHandle_t, Qnn_TensorHandle_t *, uint32_t *) =
		output ? QnnGraph_getOutputTensors : QnnGraph_getInputTensors;

	memset(b, 0, sizeof(*b));
	if (get(graph, NULL, &count) != QNN_SUCCESS || count == 0 ||
	    count > MAX_TENSORS)
		return -1;
	b->count = count;
	if (get(graph, b->tensor, &b->count) != QNN_SUCCESS ||
	    b->count != count)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		Qnn_Shape_t shape;
		Qnn_DataType_t dtype;
		Qnn_QuantParams_t qp;
		uint32_t bytes = 1;

		if (QnnTensor_getShape(b->tensor[i], &shape) != QNN_SUCCESS ||
		    QnnTensor_getDataType(b->tensor[i], &dtype) != QNN_SUCCESS ||
		    QnnTensor_getQuantParams(b->tensor[i], &qp) != QNN_SUCCESS)
			return -1;
		if (dtype != QNN_DATATYPE_UINT8 || shape.rank == 0 ||
		    shape.rank > 8)
			return -1;
		for (uint32_t j = 0; j < shape.rank; j++) {
			if (shape.dims[j] == 0 ||
			    bytes > MAX_BYTES / shape.dims[j])
				return -1;
			bytes *= shape.dims[j];
		}
		if (bytes > MAX_BYTES - b->total)
			return -1;
		b->bytes[i] = bytes;
		b->total += bytes;
	}
	return 0;
}

static int bind_data(boundary_t *b, uint8_t *data)
{
	for (uint32_t i = 0, offset = 0; i < b->count;
	     offset += b->bytes[i++]) {
		if (QnnTensor_setData(b->tensor[i], data + offset,
				      b->bytes[i]) != QNN_SUCCESS)
			return -1;
	}
	return 0;
}

static unsigned argmax10(const uint8_t *values)
{
	unsigned best = 0;

	for (unsigned i = 1; i < MNIST_CLASSES; i++) {
		if (values[i] > values[best])
			best = i;
	}
	return best;
}

static void class_probs(const uint8_t *values, const Qnn_QuantParams_t *qp,
			double probs[MNIST_CLASSES])
{
	double logit[MNIST_CLASSES];
	double maxv = -1.0e300;
	double sum = 0.0;
	double scale = (qp && qp->scale != 0.0f) ? (double)qp->scale : 1.0;

	for (unsigned i = 0; i < MNIST_CLASSES; i++) {
		logit[i] = ((double)values[i] - (double)qp->zeroPoint) * scale;
		if (logit[i] > maxv)
			maxv = logit[i];
	}
	for (unsigned i = 0; i < MNIST_CLASSES; i++) {
		probs[i] = exp(logit[i] - maxv);
		sum += probs[i];
	}
	if (sum > 0.0) {
		for (unsigned i = 0; i < MNIST_CLASSES; i++)
			probs[i] /= sum;
	}
}

static void print_digit(const uint8_t *pixels)
{
	static const char ramp[] = " .:-=+*#%@";

	for (unsigned y = 0; y < 28; y += 2) {
		for (unsigned x = 0; x < 28; x += 2) {
			unsigned sum = (unsigned)pixels[y * 28 + x] +
				       (unsigned)pixels[y * 28 + x + 1] +
				       (unsigned)pixels[(y + 1) * 28 + x] +
				       (unsigned)pixels[(y + 1) * 28 + x + 1];
			unsigned level = (sum / 4u) * 9u / 255u;

			putchar(ramp[level]);
		}
		putchar('\n');
	}
}

static int execute_one(Qnn_GraphHandle_t graph, boundary_t *in,
		       boundary_t *out, const uint8_t *input,
		       const uint8_t *reference, uint8_t *output,
		       uint64_t *ap_ticks, uint64_t *dsp_us,
		       unsigned *max_error, unsigned *sample_error)
{
	uint64_t start, end;
	unsigned local = 0;

	if (bind_data(in, (uint8_t *)(uintptr_t)input) != 0 ||
	    bind_data(out, output) != 0)
		return -1;

	/* Every poisoned byte starts farther than tolerance from its reference. */
	for (uint32_t i = 0; i < out->total; i++)
		output[i] = reference[i] < 128 ? 255 : 0;

	start = now_ticks();
	int rc = QnnGraph_execute(graph, in->tensor, in->count,
				  out->tensor, out->count, 0);
	end = now_ticks();
	if (rc != QNN_SUCCESS) {
		uint64_t completed = 0;

		property_u64(graph, "native.executions", &completed);
		printf("FAIL: graphExecute rc=%d completed_in_context=%llu\n",
		       rc, (unsigned long long)completed);
		return -1;
	}
	if (end <= start ||
	    property_u64(graph, "native.last_dsp_us", dsp_us) != 0)
		return -1;
	*ap_ticks = end - start;

	for (uint32_t i = 0; i < out->total; i++) {
		int delta = (int)output[i] - (int)reference[i];
		unsigned error = (unsigned)(delta < 0 ? -delta : delta);

		if (error > local)
			local = error;
		if (error > *max_error)
			*max_error = error;
		if (error > TOLERANCE) {
			printf("FAIL: output byte=%u actual=%u expected=%u error=%u\n",
			       i, output[i], reference[i], error);
			return -1;
		}
	}
	*sample_error = local;
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static stats_t compute_stats(uint64_t *values, unsigned n, double us_per_tick)
{
	stats_t s;
	uint64_t total = 0;

	for (unsigned i = 0; i < n; i++)
		total += values[i];
	qsort(values, n, sizeof(*values), cmp_u64);

	s.mean = (double)total * us_per_tick / (double)n;
	s.p50 = (double)values[(n * 50u + 99u) / 100u - 1u] * us_per_tick;
	s.p95 = (double)values[(n * 95u + 99u) / 100u - 1u] * us_per_tick;
	s.min = (double)values[0] * us_per_tick;
	s.max = (double)values[n - 1] * us_per_tick;
	return s;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t length, offset = 0;

	if (fd < 0)
		return -1;
	length = strlen(text);
	while (offset < length) {
		int n = (int)write(fd, text + offset, length - offset);

		if (n <= 0) {
			close(fd);
			return -1;
		}
		offset += (size_t)n;
	}
	close(fd);
	return 0;
}

static void json_summary(char *buf, size_t cap, const options_t *o,
			 unsigned samples, unsigned checked, unsigned correct,
			 unsigned max_error, double accuracy, double backend_ms,
			 double context_ms, double graph_ms, int have_bench,
			 unsigned iterations, unsigned warmup, const stats_t *ap,
			 const stats_t *dsp, double ap_fps, double dsp_fps,
			 double loop_us, double loop_fps, double gops_ap,
			 double gops_dsp)
{
	char bench[1024];

	if (have_bench) {
		snprintf(bench, sizeof(bench),
			 "{\"warmup\":%u,\"iterations\":%u,"
			 "\"ap_mean_us\":%.3f,\"ap_p50_us\":%.3f,"
			 "\"ap_p95_us\":%.3f,\"ap_min_us\":%.3f,"
			 "\"ap_max_us\":%.3f,\"dsp_mean_us\":%.3f,"
			 "\"dsp_p50_us\":%.3f,\"dsp_p95_us\":%.3f,"
			 "\"dsp_min_us\":%.3f,\"dsp_max_us\":%.3f,"
			 "\"ap_fps\":%.3f,\"dsp_fps\":%.3f,"
			 "\"checked_loop_us\":%.3f,\"checked_loop_fps\":%.3f,"
			 "\"macs_per_image\":%llu,\"effective_gops_ap\":%.3f,"
			 "\"effective_gops_dsp\":%.3f}",
			 warmup, iterations, ap->mean, ap->p50, ap->p95,
			 ap->min, ap->max, dsp->mean, dsp->p50, dsp->p95,
			 dsp->min, dsp->max, ap_fps, dsp_fps, loop_us,
			 loop_fps, (unsigned long long)MNIST_MACS, gops_ap,
			 gops_dsp);
	} else {
		snprintf(bench, sizeof(bench), "null");
	}

	snprintf(buf, cap,
		 "{\"schema\":\"qnndemo/1\","
		 "\"application\":\"MNIST handwritten digit classification\","
		 "\"backend\":\"htp_native\",\"cpu_fallback\":false,"
		 "\"graph\":\"%s\",\"tolerance_lsb\":%u,"
		 "\"samples\":%u,\"checked\":%u,\"correct\":%u,"
		 "\"accuracy\":%.4f,\"max_abs_error_lsb\":%u,"
		 "\"load_ms\":{\"backend\":%.3f,\"context\":%.3f,"
		 "\"graph\":%.3f},\"bench\":%s}",
		 o->graph, TOLERANCE, samples, checked, correct, accuracy,
		 max_error, backend_ms, context_ms, graph_ms, bench);
}

#define DIE(...) do { printf(__VA_ARGS__); failed = 1; goto done; } while (0)

int main(int argc, char **argv)
{
	options_t o = {
		.context = "/apps/qnndemo/res/q8b_mnist.bin",
		.graph = "q8b_mnist_u8",
		.input = "/apps/qnndemo/res/input.bin",
		.labels = "/apps/qnndemo/res/labels.bin",
		.reference = "/apps/qnndemo/res/reference.bin",
		.json = NULL,
		.samples = 0,
		.show = 10,
		.warmup = DEFAULT_WARMUP,
		.iterations = 1000,
		.quiet = 0,
		.confusion = 0,
	};
	Qnn_BackendHandle_t backend = 0;
	Qnn_ContextHandle_t context = 0;
	Qnn_GraphHandle_t graph = 0;
	boundary_t in_b, out_b;
	uint8_t *context_bin = NULL, *input = NULL, *labels = NULL;
	uint8_t *reference = NULL, *output = NULL;
	uint64_t *ap = NULL, *dsp = NULL;
	Qnn_QuantParams_t out_qp;
	int context_size = 0, input_size = 0, labels_size = 0, ref_size = 0;
	unsigned samples, available, checked = 0, correct = 0;
	unsigned max_error = 0, show;
	unsigned preds[MAX_SHOW], truths[MAX_SHOW], errs[MAX_SHOW];
	double confs[MAX_SHOW];
	unsigned confusion[MNIST_CLASSES][MNIST_CLASSES];
	uint64_t t0, t1, t2, t3, backend_ticks = 0, context_ticks = 0;
	uint64_t graph_ticks = 0;
	uint32_t hz;
	int failed = 0, rc = 1, have_bench = 0;
	stats_t ap_stats = {0}, dsp_stats = {0};
	double ap_fps = 0.0, dsp_fps = 0.0, loop_us = 0.0, loop_fps = 0.0;
	double gops_ap = 0.0, gops_dsp = 0.0;
	unsigned iterations = 0;

	if (parse_args(argc, argv, &o) != 0) {
		usage(argv[0]);
		return 2;
	}

	hz = counter_frequency();
	if (hz == 0) {
		printf("FAIL: no architectural counter frequency\n");
		return 1;
	}

	context_bin = vfs_readfile(o.context, &context_size);
	input = vfs_readfile(o.input, &input_size);
	labels = vfs_readfile(o.labels, &labels_size);
	reference = vfs_readfile(o.reference, &ref_size);
	if (!context_bin || context_size <= 0) {
		DIE("FAIL: cannot read context %s\n", o.context);
	}
	if (!input || input_size <= 0) {
		DIE("FAIL: cannot read input %s\n", o.input);
	}
	if (!labels || labels_size <= 0) {
		DIE("FAIL: cannot read labels %s\n", o.labels);
	}
	if (!reference || ref_size <= 0) {
		DIE("FAIL: cannot read reference %s\n", o.reference);
	}
	if (input_size % (int)MNIST_PIXELS) {
		DIE("FAIL: input size %d is not a multiple of 784\n", input_size);
	}
	available = (unsigned)(input_size / (int)MNIST_PIXELS);
	samples = o.samples ? o.samples : available;
	if (samples == 0 || samples > available || samples > MAX_SAMPLES) {
		DIE("FAIL: invalid corpus size %u (available %u, max %u)\n",
		    samples, available, MAX_SAMPLES);
	}
	if (labels_size < (int)samples ||
	    ref_size < (int)(samples * MNIST_CLASSES)) {
		DIE("FAIL: labels/reference do not cover %u samples\n", samples);
	}
	for (unsigned i = 0; i < samples; i++) {
		if (labels[i] >= MNIST_CLASSES) {
			DIE("FAIL: label[%u]=%u is not a digit\n", i, labels[i]);
		}
	}

	t0 = now_ticks();
	Qnn_ConfigParam_t backend_cfg = { "execution_backend", "htp_native" };
	if (QnnBackend_create(&backend_cfg, 1, &backend) != QNN_SUCCESS) {
		DIE("FAIL: QnnBackend_create\n");
	}
	t1 = now_ticks();
	backend_ticks = t1 - t0;

	bool fallback = true;
	bool htp = false;
	if (QnnProperty_hasCapability(backend, "npu.cpu_reference", &fallback) !=
		    QNN_SUCCESS || fallback) {
		DIE("FAIL: CPU fallback is enabled or unavailable\n");
	}
	if (QnnProperty_hasCapability(backend, "HTP", &htp) != QNN_SUCCESS ||
	    !htp) {
		DIE("FAIL: HTP capability is unavailable\n");
	}

	t1 = now_ticks();
	Qnn_ConfigParam_t context_cfg = { "graph_name", o.graph };
	if (QnnContext_createFromBinary(backend, 0,
					(const uint8_t *)context_bin,
					(uint64_t)context_size, &context_cfg, 1,
					&context) != QNN_SUCCESS) {
		DIE("FAIL: QnnContext_createFromBinary\n");
	}
	t2 = now_ticks();
	context_ticks = t2 - t1;

	if (QnnGraph_retrieve(context, o.graph, &graph) != QNN_SUCCESS) {
		DIE("FAIL: QnnGraph_retrieve(%s)\n", o.graph);
	}
	t3 = now_ticks();
	graph_ticks = t3 - t2;

	const char *execution_backend = NULL;
	if (QnnGraph_getProperty(graph, "execution_backend",
				 &execution_backend) != QNN_SUCCESS ||
	    !execution_backend ||
	    strcmp(execution_backend, "official-htp-native") != 0) {
		DIE("FAIL: graph is not on the official native HTP backend\n");
	}
	if (get_boundaries(graph, 0, &in_b) != 0 ||
	    get_boundaries(graph, 1, &out_b) != 0) {
		DIE("FAIL: cannot query graph boundaries\n");
	}
	if (in_b.total != MNIST_PIXELS || out_b.total != MNIST_CLASSES) {
		DIE("FAIL: unexpected tensor sizes in=%u out=%u\n",
		    in_b.total, out_b.total);
	}
	if (QnnTensor_getQuantParams(out_b.tensor[0], &out_qp) != QNN_SUCCESS) {
		DIE("FAIL: cannot query output quantization\n");
	}
	output = malloc(out_b.total);
	if (!output) {
		DIE("FAIL: output allocation\n");
	}

	printf("QNN MNIST demo on Q8B HTP\n");
	printf("context: %s (%d B), graph %s\n", o.context, context_size,
	       o.graph);
	printf("backend: htp_native (%s); CPU fallback disabled; counter_hz=%u\n",
	       execution_backend, hz);
	printf("corpus: %u samples (%d B input, %d B reference); tolerance=%u LSB\n",
	       samples, input_size, ref_size, TOLERANCE);
	printf("load: backend=%.3f ms, context=%.3f ms, graph=%.3f ms\n",
	       (double)backend_ticks * 1000.0 / (double)hz,
	       (double)context_ticks * 1000.0 / (double)hz,
	       (double)graph_ticks * 1000.0 / (double)hz);

	memset(confusion, 0, sizeof(confusion));
	show = o.show < samples ? o.show : samples;

	for (unsigned i = 0; i < samples; i++) {
		uint64_t a, d;
		unsigned sample_error = 0;

		if (execute_one(graph, &in_b, &out_b,
				input + (size_t)i * MNIST_PIXELS,
				reference + (size_t)i * MNIST_CLASSES, output,
				&a, &d, &max_error, &sample_error) != 0) {
			printf("FAIL: sample %u\n", i);
			failed = 1;
			break;
		}
		unsigned pred = argmax10(output);
		unsigned truth = labels[i];

		confusion[truth][pred]++;
		if (pred == truth)
			correct++;
		checked++;
		if (i < show) {
			double probs[MNIST_CLASSES];

			class_probs(output, &out_qp, probs);
			preds[i] = pred;
			truths[i] = truth;
			errs[i] = sample_error;
			confs[i] = probs[pred];
		}
	}

	if (!failed) {
		double accuracy = checked ? (double)correct / (double)checked : 0.0;

		if (!o.quiet) {
			printf("\n  #  label  pred  confidence  |err| LSB\n");
			for (unsigned i = 0; i < show; i++) {
				printf("  %3u      %u      %u     %6.2f%%       %u\n",
				       i, truths[i], preds[i], confs[i] * 100.0,
				       errs[i]);
			}
			for (unsigned i = 0; i < show; i++) {
				printf("\ndigit sample %u: label %u, predicted %u (confidence %.2f%%)\n",
				       i, truths[i], preds[i], confs[i] * 100.0);
				print_digit(input + (size_t)i * MNIST_PIXELS);
			}
		}
		printf("\naccuracy: %u/%u (%.2f%%)\n", correct, checked,
		       accuracy * 100.0);
		printf("max |output-reference|: %u LSB (tolerance %u)\n",
		       max_error, TOLERANCE);
		if (o.confusion) {
			printf("\nconfusion (rows=true, cols=pred):\n     ");
			for (unsigned c = 0; c < MNIST_CLASSES; c++)
				printf("%4u", c);
			printf("\n");
			for (unsigned t = 0; t < MNIST_CLASSES; t++) {
				printf("%4u ", t);
				for (unsigned c = 0; c < MNIST_CLASSES; c++)
					printf("%4u", confusion[t][c]);
				printf("\n");
			}
		}
	}

	iterations = o.iterations;
	if (!failed && iterations > 0) {
		ap = malloc((size_t)iterations * sizeof(*ap));
		dsp = malloc((size_t)iterations * sizeof(*dsp));
		if (!ap || !dsp) {
			printf("FAIL: benchmark allocation\n");
			failed = 1;
		}
	}

	if (!failed && iterations > 0) {
		for (unsigned i = 0; i < o.warmup; i++) {
			uint64_t a, d;
			unsigned sample_error = 0;
			unsigned sample = i % samples;

			if (execute_one(graph, &in_b, &out_b,
					input + (size_t)sample * MNIST_PIXELS,
					reference + (size_t)sample * MNIST_CLASSES,
					output, &a, &d, &max_error,
					&sample_error) != 0) {
				printf("FAIL: warmup %u\n", i);
				failed = 1;
				break;
			}
		}

		if (!failed) {
			uint64_t loop_start = now_ticks();
			unsigned done = 0;

			for (unsigned i = 0; i < iterations; i++) {
				unsigned sample = i % samples;
				unsigned sample_error = 0;

				if (execute_one(graph, &in_b, &out_b,
						input + (size_t)sample * MNIST_PIXELS,
						reference + (size_t)sample * MNIST_CLASSES,
						output, &ap[i], &dsp[i],
						&max_error, &sample_error) != 0) {
					printf("FAIL: measured iteration %u\n", i);
					failed = 1;
					break;
				}
				done++;
			}
			uint64_t loop_end = now_ticks();

			if (!failed && done == iterations && loop_end > loop_start) {
				uint64_t sum_ap = 0, sum_dsp = 0;

				for (unsigned i = 0; i < iterations; i++) {
					sum_ap += ap[i];
					sum_dsp += dsp[i];
				}
				ap_stats = compute_stats(ap, iterations,
							 1000000.0 / (double)hz);
				dsp_stats = compute_stats(dsp, iterations, 1.0);
				ap_fps = (double)iterations * (double)hz /
					 (double)sum_ap;
				dsp_fps = (double)iterations * 1000000.0 /
					  (double)sum_dsp;
				loop_us = (double)(loop_end - loop_start) *
					  1000000.0 / (double)hz;
				loop_fps = (double)iterations * (double)hz /
					   (double)(loop_end - loop_start);
				gops_ap = 2.0 * (double)MNIST_MACS /
					  (ap_stats.mean * 1000.0);
				gops_dsp = 2.0 * (double)MNIST_MACS /
					   (dsp_stats.mean * 1000.0);
				have_bench = 1;

				printf("BENCH AP_graphExecute n=%u mean_us=%.3f p50_us=%.3f p95_us=%.3f min_us=%.3f max_us=%.3f fps=%.3f\n",
				       iterations, ap_stats.mean, ap_stats.p50,
				       ap_stats.p95, ap_stats.min, ap_stats.max,
				       ap_fps);
				printf("BENCH DSP_graphExecute n=%u mean_us=%.3f p50_us=%.3f p95_us=%.3f min_us=%.3f max_us=%.3f fps=%.3f\n",
				       iterations, dsp_stats.mean, dsp_stats.p50,
				       dsp_stats.p95, dsp_stats.min,
				       dsp_stats.max, dsp_fps);
				printf("BENCH checked_loop_us=%.3f checked_loop_fps=%.3f\n",
				       loop_us, loop_fps);
				printf("BENCH macs_per_image=%llu effective_GOPS_ap=%.3f effective_GOPS_dsp=%.3f\n",
				       (unsigned long long)MNIST_MACS, gops_ap,
				       gops_dsp);
			}
		}
	}

	if (!failed) {
		uint64_t count = 0;
		uint64_t expected = (uint64_t)samples +
				    (have_bench ? (uint64_t)o.warmup + iterations : 0);

		if (property_u64(graph, "native.executions", &count) != 0 ||
		    count != expected) {
			printf("FAIL: native.executions=%llu expected=%llu\n",
			       (unsigned long long)count,
			       (unsigned long long)expected);
			failed = 1;
		} else {
			printf("native.executions=%llu (demo %u + warmup %u + measured %u)\n",
			       (unsigned long long)count, samples,
			       have_bench ? o.warmup : 0,
			       have_bench ? iterations : 0);
		}
	}

	if (!failed) {
		char json[4096];
		double accuracy = checked ? (double)correct / (double)checked : 0.0;

		json_summary(json, sizeof(json), &o, samples, checked, correct,
			     max_error, accuracy,
			     (double)backend_ticks * 1000.0 / (double)hz,
			     (double)context_ticks * 1000.0 / (double)hz,
			     (double)graph_ticks * 1000.0 / (double)hz,
			     have_bench, iterations, o.warmup, &ap_stats,
			     &dsp_stats, ap_fps, dsp_fps, loop_us, loop_fps,
			     gops_ap, gops_dsp);
		printf("RESULT %s\n", json);
		if (o.json && write_text(o.json, json) != 0) {
			printf("FAIL: cannot write %s\n", o.json);
			failed = 1;
		}
	}

done:
	if (context && QnnContext_free(context) != QNN_SUCCESS) {
		printf("FAIL: context cleanup\n");
		failed = 1;
	}
	if (backend && QnnBackend_free(backend) != QNN_SUCCESS) {
		printf("FAIL: backend cleanup\n");
		failed = 1;
	}
	if (!failed) {
		printf("PASS: %u checked classifications, benchmark=%s, resource cleanup\n",
		       checked, have_bench ? "yes" : "no");
		rc = 0;
	}
	free(context_bin);
	free(input);
	free(labels);
	free(reference);
	free(output);
	free(ap);
	free(dsp);
	return rc;
}
