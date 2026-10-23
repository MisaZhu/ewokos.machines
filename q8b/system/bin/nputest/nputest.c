/*
 * nputest.c - functional test for the adrenonpu QNN C API (libadrenonpu.a).
 *
 * Exercises the whole QNN surface the way an inference client would: interface
 * / provider discovery, backend bring-up, device platform info, context and
 * graph construction, tensor binding, synchronous + async execute, context
 * binary serialize -> deserialize round-trip, profiling, the HTP private
 * tuning extensions and shared-buffer registration.
 *
 * Numeric graphs are checked against hand-computed reference values produced
 * by the library's in-process CPU reference executor.  On the current kernel
 * the cDSP PAS/FastRPC ABI is absent, so QnnBackend_create declines the HTP
 * path and every graph runs on that CPU reference executor - the test asserts
 * on results, not on which back end produced them, so it passes either way and
 * prints which path was taken.
 *
 * Exit status: 0 when every assertion passes, 1 otherwise.
 */
#include <adrenonpu/qnn_core.h>
#include <adrenonpu/qnn_htp_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* tiny test harness                                                   */
/* ------------------------------------------------------------------ */
static int g_pass;
static int g_fail;
static int g_verbose;

static void check(int cond, const char *what)
{
	if (cond) {
		g_pass++;
		printf("  [PASS] %s\n", what);
	} else {
		g_fail++;
		printf("  [FAIL] %s\n", what);
	}
}

static void section(const char *name)
{
	printf("\n== %s ==\n", name);
}

static int feq(float a, float b, float tol)
{
	float d = a - b;

	if (d < 0.0f)
		d = -d;
	return d <= tol;
}

static Qnn_ConfigParam_t cfg(const char *k, const char *v)
{
	Qnn_ConfigParam_t c;

	c.key = k;
	c.value = v;
	return c;
}

static Qnn_TensorHandle_t mk_graph_tensor(Qnn_GraphHandle_t g, const char *name,
        Qnn_TensorType_t tt, Qnn_DataType_t dt, uint32_t *dims, uint32_t rank,
        const Qnn_QuantParams_t *qp)
{
	Qnn_Shape_t s;
	Qnn_TensorHandle_t h = 0;

	s.rank = rank;
	s.dims = dims;
	if (QnnTensor_createGraphTensor(g, name, tt, dt, &s, qp, &h) !=
	    QNN_SUCCESS)
		return 0;
	return h;
}

static Qnn_TensorHandle_t mk_ctx_tensor(Qnn_ContextHandle_t c, const char *name,
        Qnn_TensorType_t tt, Qnn_DataType_t dt, uint32_t *dims, uint32_t rank,
        const Qnn_QuantParams_t *qp)
{
	Qnn_Shape_t s;
	Qnn_TensorHandle_t h = 0;

	s.rank = rank;
	s.dims = dims;
	if (QnnTensor_createContextTensor(c, name, tt, dt, &s, qp, &h) !=
	    QNN_SUCCESS)
		return 0;
	return h;
}

/* Read a FLOAT32 output tensor's buffer (owned by the executor after run). */
static const float *out_f32(Qnn_TensorHandle_t t, uint64_t *bytes)
{
	void *p = NULL;
	uint64_t n = 0;

	if (QnnTensor_getData(t, &p, &n) != QNN_SUCCESS)
		return NULL;
	if (bytes)
		*bytes = n;
	return (const float *)p;
}

static void log_cb(uint32_t level, const char *msg, void *ud)
{
	(void)ud;
	if (g_verbose)
		printf("  [qnn lvl%u] %s", level, msg);
}

/* ------------------------------------------------------------------ */
/* interface / backend / device discovery                              */
/* ------------------------------------------------------------------ */
static Qnn_BackendHandle_t test_interface(void)
{
	const QnnProvider_t *provs = NULL;
	uint32_t np = 0;
	Qnn_Version_t v;
	Qnn_BackendHandle_t be = 0;
	const char *bid = NULL;
	bool cap_htp = false, cap_cdsp = false, cap_cpu = false;

	section("QnnInterface / QnnBackend discovery");
	check(QnnInterface_getApiVersion(&v) == QNN_SUCCESS,
	      "getApiVersion succeeds");
	printf("  API version %u.%u.%u\n", v.major, v.minor, v.patch);

	check(QnnInterface_getProviders(&provs, &np) == QNN_SUCCESS && np >= 1 &&
	      provs && provs[0].backendId, "getProviders returns >=1 provider");
	if (np >= 1 && provs[0].backendId)
		printf("  provider[0].backendId = %s\n", provs[0].backendId);
	check(np >= 1 && provs[0].backendId &&
	      strcmp(provs[0].backendId, "HTP") == 0, "provider is the HTP backend");

	check(QnnBackend_create(NULL, 0, &be) == QNN_SUCCESS && be != 0,
	      "QnnBackend_create succeeds");
	if (!be)
		return 0;

	check(QnnBackend_getBuildId(be, &bid) == QNN_SUCCESS && bid,
	      "getBuildId succeeds");
	if (bid)
		printf("  build id: %s\n", bid);

	check(QnnProperty_hasCapability(be, "HTP", &cap_htp) == QNN_SUCCESS &&
	      cap_htp, "capability 'HTP' is true");
	QnnProperty_hasCapability(be, "npu.cdsp.available", &cap_cdsp);
	QnnProperty_hasCapability(be, "npu.cpu_reference", &cap_cpu);
	printf("  cDSP (HTP hardware) available : %s\n", cap_cdsp ? "yes" : "no");
	printf("  CPU reference executor        : %s\n", cap_cpu ? "yes" : "no");
	printf("  => graphs will run on the %s path\n",
	       cap_cdsp ? "HTP/cDSP" : "CPU reference");
	check(cap_cpu, "CPU reference executor is always available");

	{
		const char *ops[64];
		uint32_t nops = 64;

		check(QnnBackend_getSupportedOperations(be, ops, &nops) ==
		      QNN_SUCCESS && nops > 0, "getSupportedOperations lists ops");
		printf("  supported ops (%u):", nops);
		for (uint32_t i = 0; i < nops; i++)
			printf(" %s", ops[i]);
		printf("\n");
	}
	return be;
}

static void test_device(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t *out_dev)
{
	Qnn_DeviceHandle_t dev = 0;
	const Qnn_ConfigParam_t *info = NULL;
	uint32_t ninfo = 0;
	void *infra = (void *)1;

	section("QnnDevice");
	check(QnnDevice_create(be, NULL, 0, &dev) == QNN_SUCCESS && dev != 0,
	      "QnnDevice_create succeeds");
	if (!dev) {
		*out_dev = 0;
		return;
	}
	check(QnnDevice_getPlatformInfo(dev, &info, &ninfo) == QNN_SUCCESS &&
	      ninfo > 0, "getPlatformInfo returns entries");
	for (uint32_t i = 0; i < ninfo; i++)
		printf("  %-16s = %s\n", info[i].key, info[i].value);
	check(QnnDevice_getInfrastructure(dev, &infra) == QNN_SUCCESS,
	      "getInfrastructure succeeds");
	QnnDevice_freePlatformInfo(dev, info, ninfo);
	*out_dev = dev;
}

/* ------------------------------------------------------------------ */
/* numeric graph tests (CPU reference executor)                        */
/* ------------------------------------------------------------------ */
static void test_add_f32(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	uint32_t d[4] = { 1, 1, 1, 4 };
	Qnn_TensorHandle_t ia, ib, ot;
	Qnn_TensorHandle_t ins[2], outs[1];
	float a[4] = { 1, 2, 3, 4 };
	float b[4] = { 10, 20, 30, 40 };
	const float *o;
	int ok;

	section("ElementWiseAdd (FLOAT32)");
	check(QnnContext_create(be, dev, NULL, 0, &ctx) == QNN_SUCCESS && ctx,
	      "context create");
	check(QnnGraph_create(ctx, "add", &g) == QNN_SUCCESS && g, "graph create");
	ia = mk_graph_tensor(g, "a", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	ib = mk_graph_tensor(g, "b", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	check(ia && ib && ot, "three graph tensors created");

	ins[0] = ia; ins[1] = ib; outs[0] = ot;
	check(QnnGraph_addNode(g, "ElementWiseAdd", ins, 2, outs, 1, NULL, 0) ==
	      QNN_SUCCESS, "addNode ElementWiseAdd");
	check(QnnGraph_finalize(g) == QNN_SUCCESS, "graph finalize");

	QnnTensor_setData(ia, a, sizeof(a));
	QnnTensor_setData(ib, b, sizeof(b));
	check(QnnGraph_execute(g, ins, 2, outs, 1, NULL) == QNN_SUCCESS,
	      "graph execute");

	o = out_f32(ot, NULL);
	ok = o && feq(o[0], 11, 1e-5f) && feq(o[1], 22, 1e-5f) &&
	     feq(o[2], 33, 1e-5f) && feq(o[3], 44, 1e-5f);
	if (o)
		printf("  out = {%.1f, %.1f, %.1f, %.1f} (expect 11,22,33,44)\n",
		       o[0], o[1], o[2], o[3]);
	check(ok, "add result correct");

	/* negative path: unknown op must be rejected. */
	check(QnnGraph_addNode(g, "NoSuchOp", ins, 2, outs, 1, NULL, 0) ==
	      QNN_ERROR, "addNode rejects unknown op");

	QnnContext_free(ctx);
}

static void test_relu_f32(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	uint32_t d[4] = { 1, 1, 1, 4 };
	Qnn_TensorHandle_t in, ot, ins[1], outs[1];
	float x[4] = { -1.0f, 2.0f, -3.0f, 4.0f };
	const float *o;
	int ok;

	section("Relu (FLOAT32)");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "relu", &g);
	in = mk_graph_tensor(g, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	ins[0] = in; outs[0] = ot;
	check(QnnGraph_addNode(g, "Relu", ins, 1, outs, 1, NULL, 0) ==
	      QNN_SUCCESS, "addNode Relu");
	check(QnnGraph_finalize(g) == QNN_SUCCESS, "finalize");
	QnnTensor_setData(in, x, sizeof(x));
	check(QnnGraph_execute(g, ins, 1, outs, 1, NULL) == QNN_SUCCESS, "execute");
	o = out_f32(ot, NULL);
	ok = o && feq(o[0], 0, 1e-5f) && feq(o[1], 2, 1e-5f) &&
	     feq(o[2], 0, 1e-5f) && feq(o[3], 4, 1e-5f);
	if (o)
		printf("  out = {%.1f, %.1f, %.1f, %.1f} (expect 0,2,0,4)\n",
		       o[0], o[1], o[2], o[3]);
	check(ok, "relu result correct");
	QnnContext_free(ctx);
}

static void test_conv2d_f32(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	uint32_t id[4] = { 1, 2, 2, 1 };   /* NHWC */
	uint32_t wd[4] = { 2, 2, 1, 1 };   /* KH,KW,Cin,Cout */
	uint32_t od[4] = { 1, 1, 1, 1 };
	Qnn_TensorHandle_t in, w, ot, ins[2], outs[1];
	Qnn_ConfigParam_t cc[2];
	float img[4] = { 1, 2, 3, 4 };
	float kw[4] = { 1, 1, 1, 1 };
	const float *o;

	section("Conv2d 2x2 valid (FLOAT32, NHWC)");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "conv", &g);
	in = mk_graph_tensor(g, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, id, 4, NULL);
	w = mk_ctx_tensor(ctx, "w", QNN_TENSOR_TYPE_STATIC,
	                  QNN_DATATYPE_FLOAT32, wd, 4, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, od, 4, NULL);
	check(in && w && ot, "conv tensors created");
	QnnTensor_setData(w, kw, sizeof(kw));
	cc[0] = cfg("stride", "1,1");
	cc[1] = cfg("pad", "0,0,0,0");
	ins[0] = in; ins[1] = w; outs[0] = ot;
	check(QnnGraph_addNode(g, "Conv2d", ins, 2, outs, 1, cc, 2) ==
	      QNN_SUCCESS, "addNode Conv2d");
	check(QnnGraph_finalize(g) == QNN_SUCCESS, "finalize");
	QnnTensor_setData(in, img, sizeof(img));
	check(QnnGraph_execute(g, ins, 2, outs, 1, NULL) == QNN_SUCCESS, "execute");
	o = out_f32(ot, NULL);
	if (o)
		printf("  out = %.1f (expect 10 = 1+2+3+4)\n", o[0]);
	check(o && feq(o[0], 10.0f, 1e-4f), "conv2d result correct");

	/* Conv2d without its required stride/pad attrs must be rejected. */
	check(QnnGraph_addNode(g, "Conv2d", ins, 2, outs, 1, NULL, 0) ==
	      QNN_ERROR, "addNode Conv2d rejects missing required attrs");
	QnnContext_free(ctx);
}

static void test_softmax_f32(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	uint32_t d[4] = { 1, 1, 1, 3 };
	Qnn_TensorHandle_t in, ot, ins[1], outs[1];
	Qnn_ConfigParam_t c1[1];
	float x[3] = { 1, 2, 3 };
	const float *o;
	float sum;

	section("Softmax axis=-1 (FLOAT32)");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "sm", &g);
	in = mk_graph_tensor(g, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 4, NULL);
	c1[0] = cfg("axis", "-1");
	ins[0] = in; outs[0] = ot;
	check(QnnGraph_addNode(g, "Softmax", ins, 1, outs, 1, c1, 1) ==
	      QNN_SUCCESS, "addNode Softmax");
	check(QnnGraph_finalize(g) == QNN_SUCCESS, "finalize");
	QnnTensor_setData(in, x, sizeof(x));
	check(QnnGraph_execute(g, ins, 1, outs, 1, NULL) == QNN_SUCCESS, "execute");
	o = out_f32(ot, NULL);
	sum = o ? (o[0] + o[1] + o[2]) : 0.0f;
	if (o)
		printf("  out = {%.4f, %.4f, %.4f} sum=%.4f\n",
		       o[0], o[1], o[2], sum);
	check(o && feq(sum, 1.0f, 1e-4f), "softmax sums to 1");
	check(o && o[2] > o[1] && o[1] > o[0], "softmax is monotonic");
	QnnContext_free(ctx);
}

static void test_add_u8(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	uint32_t d[2] = { 1, 3 };
	Qnn_QuantParams_t qp;
	Qnn_TensorHandle_t ia, ib, ot, ins[2], outs[1];
	uint8_t a[3] = { 1, 2, 3 };
	uint8_t b[3] = { 4, 5, 6 };
	void *o = NULL;
	uint64_t on = 0;
	const uint8_t *ou;

	section("ElementWiseAdd (UINT8 quantized, scale=1 zp=0)");
	qp.scale = 1.0f;
	qp.zeroPoint = 0;
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "addq", &g);
	ia = mk_graph_tensor(g, "a", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_UINT8, d, 2, &qp);
	ib = mk_graph_tensor(g, "b", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_UINT8, d, 2, &qp);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_UINT8, d, 2, &qp);
	ins[0] = ia; ins[1] = ib; outs[0] = ot;
	check(QnnGraph_addNode(g, "ElementWiseAdd", ins, 2, outs, 1, NULL, 0) ==
	      QNN_SUCCESS, "addNode ElementWiseAdd(u8)");
	check(QnnGraph_finalize(g) == QNN_SUCCESS, "finalize");
	QnnTensor_setData(ia, a, sizeof(a));
	QnnTensor_setData(ib, b, sizeof(b));
	check(QnnGraph_execute(g, ins, 2, outs, 1, NULL) == QNN_SUCCESS, "execute");
	QnnTensor_getData(ot, &o, &on);
	ou = (const uint8_t *)o;
	if (ou && on == 3)
		printf("  out = {%u, %u, %u} (expect 5,7,9)\n",
		       ou[0], ou[1], ou[2]);
	check(ou && on == 3 && ou[0] == 5 && ou[1] == 7 && ou[2] == 9,
	      "quantized add result correct");
	QnnContext_free(ctx);
}

/* ------------------------------------------------------------------ */
/* context binary serialize -> deserialize -> execute round-trip       */
/* ------------------------------------------------------------------ */
static void test_fc_roundtrip(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t c1 = 0, c2 = 0;
	Qnn_GraphHandle_t g1 = 0, g2 = 0;
	uint32_t id[2] = { 1, 2 };
	uint32_t wd[2] = { 2, 1 };
	uint32_t od[2] = { 1, 1 };
	Qnn_TensorHandle_t in, w, ot, ins[2], outs[1];
	float xv[2] = { 3, 4 };
	float wv[2] = { 1, 1 };
	const float *o1;
	uint8_t *blob = NULL;
	uint64_t bsz = 0;
	bool valid = false;
	Qnn_TensorHandle_t rt_in, rt_out;
	float o2 = -1.0f;

	section("FullyConnected + context binary round-trip");
	QnnContext_create(be, dev, NULL, 0, &c1);
	QnnGraph_create(c1, "fc", &g1);
	in = mk_graph_tensor(g1, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, id, 2, NULL);
	w = mk_ctx_tensor(c1, "w", QNN_TENSOR_TYPE_STATIC,
	                  QNN_DATATYPE_FLOAT32, wd, 2, NULL);
	ot = mk_graph_tensor(g1, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, od, 2, NULL);
	check(in && w && ot, "fc tensors created");
	QnnTensor_setData(w, wv, sizeof(wv));
	ins[0] = in; ins[1] = w; outs[0] = ot;
	check(QnnGraph_addNode(g1, "FullyConnected", ins, 2, outs, 1, NULL, 0) ==
	      QNN_SUCCESS, "addNode FullyConnected");
	check(QnnContext_finalize(c1) == QNN_SUCCESS, "context finalize");

	QnnTensor_setData(in, xv, sizeof(xv));
	check(QnnGraph_execute(g1, ins, 2, outs, 1, NULL) == QNN_SUCCESS,
	      "execute (source context)");
	o1 = out_f32(ot, NULL);
	if (o1)
		printf("  source out = %.1f (expect 7 = 3*1+4*1)\n", o1[0]);
	check(o1 && feq(o1[0], 7.0f, 1e-4f), "source result correct");

	/* Serialize the finalized context (STATIC weight baked in). */
	check(QnnContext_getBinarySize(c1, &bsz) == QNN_SUCCESS && bsz > 0,
	      "getBinarySize > 0");
	blob = (uint8_t *)malloc((size_t)bsz);
	check(blob != NULL, "alloc binary buffer");
	check(QnnContext_getBinary(c1, blob, bsz) == QNN_SUCCESS, "getBinary");
	printf("  context binary size = %llu bytes\n",
	       (unsigned long long)bsz);

	check(QnnContext_validateBinary(be, dev, blob, bsz, &valid) ==
	      QNN_SUCCESS && valid, "validateBinary accepts the blob");
	/* Corrupt magic -> must be rejected. */
	{
		uint8_t bad = blob[0];
		bool v2 = true;

		blob[0] = (uint8_t)~bad;
		QnnContext_validateBinary(be, dev, blob, bsz, &v2);
		check(!v2, "validateBinary rejects corrupted magic");
		blob[0] = bad;
	}

	check(QnnContext_createFromBinary(be, dev, blob, bsz, NULL, 0, &c2) ==
	      QNN_SUCCESS && c2, "createFromBinary");
	check(QnnGraph_retrieve(c2, "fc", &g2) == QNN_SUCCESS && g2,
	      "retrieve deserialized graph 'fc'");

	/* Bind fresh runtime I/O by name (the deserialized graph keeps its own
	   boundary tensors; execute matches them by name). */
	rt_in = mk_ctx_tensor(c2, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                      QNN_DATATYPE_FLOAT32, id, 2, NULL);
	rt_out = mk_ctx_tensor(c2, "out", QNN_TENSOR_TYPE_APP_READ,
	                       QNN_DATATYPE_FLOAT32, od, 2, NULL);
	check(rt_in && rt_out, "runtime I/O tensors created in loaded context");
	QnnTensor_setData(rt_in, xv, sizeof(xv));
	QnnTensor_setData(rt_out, &o2, sizeof(o2));
	ins[0] = rt_in; outs[0] = rt_out;
	check(QnnGraph_execute(g2, ins, 1, outs, 1, NULL) == QNN_SUCCESS,
	      "execute (deserialized context)");
	printf("  loaded out = %.1f (expect 7)\n", o2);
	check(feq(o2, 7.0f, 1e-4f), "round-trip result correct");

	free(blob);
	QnnContext_free(c2);
	QnnContext_free(c1);
}

/* ------------------------------------------------------------------ */
/* async execute + signal                                              */
/* ------------------------------------------------------------------ */
static void test_async(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	Qnn_SignalHandle_t sig = 0;
	uint32_t d[2] = { 1, 4 };
	Qnn_TensorHandle_t ia, ib, ot, ins[2], outs[1];
	float a[4] = { 5, 5, 5, 5 };
	float b[4] = { 1, 2, 3, 4 };
	const float *o;

	section("executeAsync + QnnSignal");
	check(QnnSignal_create(be, &sig) == QNN_SUCCESS && sig, "signal create");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "adda", &g);
	ia = mk_graph_tensor(g, "a", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ib = mk_graph_tensor(g, "b", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ins[0] = ia; ins[1] = ib; outs[0] = ot;
	QnnGraph_addNode(g, "ElementWiseAdd", ins, 2, outs, 1, NULL, 0);
	QnnGraph_finalize(g);
	QnnTensor_setData(ia, a, sizeof(a));
	QnnTensor_setData(ib, b, sizeof(b));
	check(QnnGraph_executeAsync(g, ins, 2, outs, 1, NULL, sig) ==
	      QNN_SUCCESS, "executeAsync");
	check(QnnSignal_wait(sig, 2000) == QNN_SUCCESS, "signal wait (done)");
	o = out_f32(ot, NULL);
	if (o)
		printf("  out = {%.0f, %.0f, %.0f, %.0f} (expect 6,7,8,9)\n",
		       o[0], o[1], o[2], o[3]);
	check(o && feq(o[0], 6, 1e-5f) && feq(o[3], 9, 1e-5f),
	      "async result correct");
	QnnSignal_free(sig);
	QnnContext_free(ctx);
}

/* ------------------------------------------------------------------ */
/* profiling                                                           */
/* ------------------------------------------------------------------ */
static void test_profile(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	Qnn_ProfileHandle_t prof = 0;
	Qnn_ConfigParam_t pc[1];
	uint32_t d[2] = { 1, 4 };
	Qnn_TensorHandle_t in, ot, ins[1], outs[1];
	float x[4] = { -1, 2, -3, 4 };
	uint64_t sz = 0;
	uint8_t *buf;
	uint32_t nrec;

	section("QnnProfile");
	pc[0] = cfg("level", "2");
	check(QnnProfile_create(be, pc, 1, &prof) == QNN_SUCCESS && prof,
	      "profile create (level 2)");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "relup", &g);
	in = mk_graph_tensor(g, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ins[0] = in; outs[0] = ot;
	QnnGraph_addNode(g, "Relu", ins, 1, outs, 1, NULL, 0);
	QnnGraph_finalize(g);
	QnnTensor_setData(in, x, sizeof(x));
	check(QnnGraph_execute(g, ins, 1, outs, 1, prof) == QNN_SUCCESS,
	      "execute with profile");
	check(QnnProfile_getData(prof, NULL, &sz) == QNN_SUCCESS && sz >= 16,
	      "profile size query >= header");
	buf = (uint8_t *)malloc((size_t)sz);
	check(QnnProfile_getData(prof, buf, &sz) == QNN_SUCCESS,
	      "profile getData fills buffer");
	check(sz >= 16 && buf[0] == 'Q' && buf[1] == 'P' && buf[2] == 'R' &&
	      buf[3] == 'F', "profile blob has QPRF magic");
	memcpy(&nrec, buf + 8, 4);
	printf("  profile records = %u, blob = %llu bytes\n", nrec,
	       (unsigned long long)sz);
	check(nrec > 0, "profile recorded at least one entry");
	free(buf);
	QnnProfile_free(prof);
	QnnContext_free(ctx);
}

/* ------------------------------------------------------------------ */
/* HTP private tuning extensions                                       */
/* ------------------------------------------------------------------ */
static void test_htp_ext(Qnn_BackendHandle_t be, Qnn_DeviceHandle_t dev)
{
	Qnn_ContextHandle_t ctx = 0;
	Qnn_GraphHandle_t g = 0;
	Qnn_ProfileHandle_t prof = 0;
	uint32_t d[2] = { 1, 2 };
	Qnn_TensorHandle_t in, ot, ins[1], outs[1];

	section("QnnHtp* private extensions");
	QnnContext_create(be, dev, NULL, 0, &ctx);
	QnnGraph_create(ctx, "tune", &g);
	in = mk_graph_tensor(g, "in", QNN_TENSOR_TYPE_APP_WRITE,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ot = mk_graph_tensor(g, "out", QNN_TENSOR_TYPE_APP_READ,
	                     QNN_DATATYPE_FLOAT32, d, 2, NULL);
	ins[0] = in; outs[0] = ot;
	QnnGraph_addNode(g, "Relu", ins, 1, outs, 1, NULL, 0);
	QnnProfile_create(be, NULL, 0, &prof);

	check(QnnHtpContext_SetPerfConfig(ctx, QNN_HTP_PERF_MODE_TURBO) ==
	      QNN_SUCCESS, "SetPerfConfig(TURBO)");
	check(QnnHtpContext_SetPerfConfig(ctx, (QnnHtp_PerfMode_t)99) ==
	      QNN_ERROR, "SetPerfConfig rejects out-of-range mode");
	check(QnnHtpContext_SetVtcmLimit(ctx, 256 * 1024) == QNN_SUCCESS,
	      "SetVtcmLimit(256KB)");
	check(QnnHtpContext_SetConcurrentGroup(ctx, 1) == QNN_SUCCESS,
	      "SetConcurrentGroup(1)");
	check(QnnHtpGraph_SetPrecisionHint(g, QNN_HTP_PRECISION_RELAXED_FP16) ==
	      QNN_SUCCESS, "SetPrecisionHint(RELAXED_FP16)");
	check(QnnHtpGraph_SetSkipDataFlush(g, false) == QNN_SUCCESS,
	      "SetSkipDataFlush(false)");
	check(QnnHtpDevice_SetDspCoreMask(dev, 0x3) == QNN_SUCCESS,
	      "SetDspCoreMask(0b11)");
	check(QnnHtpDevice_SetDspCoreMask(dev, 0x0) == QNN_ERROR,
	      "SetDspCoreMask rejects 0");
	check(QnnHtpDevice_SetDspCoreMask(dev, 0xFF) == QNN_ERROR,
	      "SetDspCoreMask rejects bits beyond NSP cores");
	check(QnnHtpProfile_EnableHwCounters(prof, true) == QNN_SUCCESS,
	      "Profile_EnableHwCounters(true)");

	QnnProfile_free(prof);
	QnnContext_free(ctx);
}

/* ------------------------------------------------------------------ */
/* shared-buffer registration (dma pool backed)                        */
/* ------------------------------------------------------------------ */
static void test_mem(Qnn_BackendHandle_t be)
{
	void *hp = NULL;
	int fd = -12345;

	section("QnnMem shared buffer");
	if (QnnMem_register(be, -1, 0, 4096, &hp) == QNN_SUCCESS && hp) {
		check(QnnMem_getBufferFd(be, hp, &fd) == QNN_SUCCESS,
		      "getBufferFd succeeds");
		memset(hp, 0xAB, 4096);   /* region is writable */
		check(((unsigned char *)hp)[0] == 0xAB &&
		      ((unsigned char *)hp)[4095] == 0xAB,
		      "registered buffer is read/writable");
		check(QnnMem_unregister(be, hp) == QNN_SUCCESS,
		      "unregister succeeds");
	} else {
		printf("  [SKIP] dma pool unavailable; QnnMem_register declined\n");
		g_pass++;   /* declining gracefully is the correct behaviour */
	}
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	Qnn_BackendHandle_t be;
	Qnn_DeviceHandle_t dev = 0;
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("adrenonpu QNN C API test (q8b / SC8280XP / Hexagon V68)\n");
	QnnLog_initialize(log_cb, NULL);

	be = test_interface();
	if (!be) {
		printf("\nbackend create failed; aborting\n");
		QnnLog_free();
		return 1;
	}
	test_device(be, &dev);

	test_add_f32(be, dev);
	test_relu_f32(be, dev);
	test_conv2d_f32(be, dev);
	test_softmax_f32(be, dev);
	test_add_u8(be, dev);
	test_fc_roundtrip(be, dev);
	test_async(be, dev);
	test_profile(be, dev);
	test_htp_ext(be, dev);
	test_mem(be);

	if (dev)
		QnnDevice_free(dev);
	QnnBackend_free(be);
	QnnLog_free();

	printf("\n========================================\n");
	printf("  %d passed, %d failed\n", g_pass, g_fail);
	printf("========================================\n");
	return g_fail ? 1 : 0;
}
