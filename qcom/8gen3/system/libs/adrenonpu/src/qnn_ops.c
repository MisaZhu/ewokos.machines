/*
 * qnn_ops.c - built-in op package ("qti.aisw" OpDef subset) registry and
 * addNode attribute parsing.
 *
 * The op list is the subset the CPU reference executor implements and the
 * HTP marshaller accepts; names and parameter spellings follow the QNN
 * OpDef package so a graph built against the vendor SDK headers maps 1:1.
 * Attribute keys use the op-package parameter names flattened into the
 * string config form of qnn_core.h:
 *   Conv2d            pad="t,b,l,r" stride="h,w" dilate="h,w" group="N"
 *   DepthwiseConv2d   pad / stride / dilate
 *   PoolAvg2d/PoolMax2d  kernel="h,w" stride="h,w" pad="t,b,l,r" ceil="0|1"
 *   Softmax           axis="N"
 *   Reduce*           axes="a,b,..." keep_dims="0|1"
 *   Concat            axis="N"
 *   Transpose         perm="a,b,c,d"
 *   FullyConnected    activation="none|relu"
 */
#include <adrenonpu/qnn_core_internal.h>
#include <stdlib.h>
#include <string.h>

static const qnn_op_info_t g_ops[] = {
	{ "Conv2d",            QNN_OP_KIND_CONV2D,           2, 3, 1, 1, "stride,pad" },
	{ "DepthwiseConv2d",   QNN_OP_KIND_DEPTHWISE_CONV2D, 2, 3, 1, 1, "stride,pad" },
	{ "FullyConnected",    QNN_OP_KIND_FULLY_CONNECTED,  2, 3, 1, 1, NULL },
	{ "ElementWiseAdd",    QNN_OP_KIND_ADD,              2, 2, 1, 1, NULL },
	{ "ElementWiseSubtract", QNN_OP_KIND_SUB,            2, 2, 1, 1, NULL },
	{ "ElementWiseMultiply", QNN_OP_KIND_MUL,            2, 2, 1, 1, NULL },
	{ "ElementWiseDivide", QNN_OP_KIND_DIV,              2, 2, 1, 1, NULL },
	{ "ElementWiseMaximum", QNN_OP_KIND_MAXIMUM,        2, 2, 1, 1, NULL },
	{ "ElementWiseMinimum", QNN_OP_KIND_MINIMUM,        2, 2, 1, 1, NULL },
	{ "Relu",              QNN_OP_KIND_RELU,             1, 1, 1, 1, NULL },
	{ "Relu6",             QNN_OP_KIND_RELU6,            1, 1, 1, 1, NULL },
	{ "Sigmoid",           QNN_OP_KIND_SIGMOID,          1, 1, 1, 1, NULL },
	{ "Tanh",              QNN_OP_KIND_TANH,             1, 1, 1, 1, NULL },
	{ "Softmax",           QNN_OP_KIND_SOFTMAX,          1, 1, 1, 1, "axis" },
	{ "PoolAvg2d",         QNN_OP_KIND_AVG_POOL,         1, 1, 1, 1, "kernel,stride" },
	{ "PoolMax2d",         QNN_OP_KIND_MAX_POOL,         1, 1, 1, 1, "kernel,stride" },
	{ "ReduceSum",         QNN_OP_KIND_REDUCE_SUM,       1, 1, 1, 1, "axes" },
	{ "ReduceMean",        QNN_OP_KIND_REDUCE_MEAN,      1, 1, 1, 1, "axes" },
	{ "ReduceMax",         QNN_OP_KIND_REDUCE_MAX,       1, 1, 1, 1, "axes" },
	{ "ReduceMin",         QNN_OP_KIND_REDUCE_MIN,       1, 1, 1, 1, "axes" },
	{ "Concat",            QNN_OP_KIND_CONCAT,           2, QNN_NODE_MAX_INPUTS, 1, 1, "axis" },
	{ "Transpose",         QNN_OP_KIND_TRANSPOSE,        1, 1, 1, 1, "perm" },
	{ "Reshape",           QNN_OP_KIND_RESHAPE,          1, 1, 1, 1, NULL },
	{ "Convert",           QNN_OP_KIND_CONVERT,          1, 1, 1, 1, NULL },
};

#define OPS_COUNT ((uint32_t)(sizeof(g_ops) / sizeof(g_ops[0])))

const qnn_op_info_t *qnn_op_lookup(const char *name)
{
	uint32_t i;

	if (!name)
		return NULL;
	for (i = 0; i < OPS_COUNT; i++) {
		if (strcmp(g_ops[i].name, name) == 0)
			return &g_ops[i];
	}
	return NULL;
}

uint32_t qnn_op_count(void)
{
	return OPS_COUNT;
}

const qnn_op_info_t *qnn_op_table(void)
{
	return g_ops;
}

/* Parse a comma separated int list ("1,2,3") into out[], up to max items.
   Returns the number parsed. */
static uint32_t parse_int_list(const char *v, int32_t *out, uint32_t max)
{
	uint32_t n = 0;
	const char *p = v;

	while (p && *p && n < max) {
		out[n++] = (int32_t)strtol(p, (char **)&p, 10);
		if (*p == ',')
			p++;
		else
			break;
	}
	return n;
}

void qnn_op_parse_attrs(const Qnn_ConfigParam_t *cfg, uint32_t ncfg,
                        qnn_op_attrs_t *a)
{
	const char *v;

	memset(a, 0, sizeof(*a));
	a->stride[0] = a->stride[1] = 1;
	a->dilate[0] = a->dilate[1] = 1;
	a->group = 1;
	a->axis = -1;

	if ((v = qnn_cfg_get(cfg, ncfg, "pad")))
		parse_int_list(v, a->pad, 4);
	if ((v = qnn_cfg_get(cfg, ncfg, "stride")))
		parse_int_list(v, a->stride, 2);
	if ((v = qnn_cfg_get(cfg, ncfg, "dilate")))
		parse_int_list(v, a->dilate, 2);
	if ((v = qnn_cfg_get(cfg, ncfg, "kernel")))
		parse_int_list(v, a->kernel, 2);
	if ((v = qnn_cfg_get(cfg, ncfg, "perm")))
		a->num_perm = parse_int_list(v, a->perm, 4);
	if ((v = qnn_cfg_get(cfg, ncfg, "axes")))
		a->num_axes = parse_int_list(v, a->axes, QNN_MAX_AXES);
	a->group = qnn_cfg_get_int(cfg, ncfg, "group", 1);
	a->axis = qnn_cfg_get_int(cfg, ncfg, "axis", -1);
	a->keep_dims = qnn_cfg_get_int(cfg, ncfg, "keep_dims", 0);
	a->ceil_mode = qnn_cfg_get_int(cfg, ncfg, "ceil", 0);
	v = qnn_cfg_get(cfg, ncfg, "activation");
	a->activation = (v && strcmp(v, "relu") == 0) ? 1 : 0;
}

int qnn_op_validate_config(const char *op_name, const char *const *attr_keys,
                           uint32_t num_attrs)
{
	const qnn_op_info_t *info = qnn_op_lookup(op_name);
	const char *p;
	char keybuf[32];

	if (!info)
		return QNN_ERROR;
	if (!info->required_attrs)
		return QNN_SUCCESS;

	/* Each required key must be present in the addNode config. */
	p = info->required_attrs;
	while (p && *p) {
		const char *comma = strchr(p, ',');
		size_t seglen = comma ? (size_t)(comma - p) : strlen(p);
		uint32_t i;
		int found = 0;

		if (seglen == 0 || seglen >= sizeof(keybuf))
			return QNN_ERROR;
		memcpy(keybuf, p, seglen);
		keybuf[seglen] = '\0';
		for (i = 0; i < num_attrs; i++) {
			if (attr_keys[i] && strcmp(attr_keys[i], keybuf) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			return QNN_ERROR;
		p = comma ? comma + 1 : NULL;
	}
	return QNN_SUCCESS;
}
