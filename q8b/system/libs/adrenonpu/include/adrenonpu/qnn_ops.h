/*
 * qnn_ops.h - op package registry, per-node attribute model and shape /
 * config helpers shared by the graph builder, the HTP dispatch marshaller
 * and the CPU reference executor.
 *
 * Op names follow the Qualcomm QNN OpDef package naming ("qti.aisw", e.g.
 * "Conv2d", "ElementWiseAdd").  The attribute keys below mirror the QNN
 * op-package parameter names exposed to QnnGraph_addNode configs
 * (pad[], stride[], dilate[], axis, perm, ...), flattened to the
 * string key/value Qnn_ConfigParam_t form used by qnn_core.h.
 */
#ifndef QNN_OPS_H
#define QNN_OPS_H

#include <adrenonpu/qnn_core.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	QNN_OP_KIND_UNKNOWN = 0,
	QNN_OP_KIND_CONV2D,
	QNN_OP_KIND_DEPTHWISE_CONV2D,
	QNN_OP_KIND_FULLY_CONNECTED,
	QNN_OP_KIND_ADD,
	QNN_OP_KIND_SUB,
	QNN_OP_KIND_MUL,
	QNN_OP_KIND_DIV,
	QNN_OP_KIND_MAXIMUM,
	QNN_OP_KIND_MINIMUM,
	QNN_OP_KIND_RELU,
	QNN_OP_KIND_RELU6,
	QNN_OP_KIND_SIGMOID,
	QNN_OP_KIND_TANH,
	QNN_OP_KIND_SOFTMAX,
	QNN_OP_KIND_AVG_POOL,
	QNN_OP_KIND_MAX_POOL,
	QNN_OP_KIND_REDUCE_SUM,
	QNN_OP_KIND_REDUCE_MEAN,
	QNN_OP_KIND_REDUCE_MAX,
	QNN_OP_KIND_REDUCE_MIN,
	QNN_OP_KIND_CONCAT,
	QNN_OP_KIND_TRANSPOSE,
	QNN_OP_KIND_RESHAPE,
	QNN_OP_KIND_CONVERT
};

#define QNN_NODE_MAX_INPUTS  8
#define QNN_NODE_MAX_OUTPUTS 4
#define QNN_MAX_AXES         4

/* Parsed addNode config.  Array parameters keep the QNN op-package fixed
   2x/4x widths (pad[4] = {top,bottom,left,right}, stride[2], dilate[2]). */
typedef struct {
	int32_t  pad[4];        /* Conv2d / pooling                       */
	int32_t  stride[2];
	int32_t  dilate[2];
	int32_t  kernel[2];     /* pooling filter size                    */
	int32_t  group;         /* Conv2d group count                     */
	int32_t  axis;          /* Softmax / Reduce / Concat              */
	int32_t  axes[QNN_MAX_AXES];
	uint32_t num_axes;
	int32_t  perm[4];       /* Transpose permutation                  */
	uint32_t num_perm;
	int32_t  keep_dims;     /* Reduce*                                */
	int32_t  activation;    /* FullyConnected fused act (0 none, 1 relu) */
	int32_t  ceil_mode;     /* pooling                                */
} qnn_op_attrs_t;

typedef struct {
	const char *name;
	uint32_t    kind;        /* QNN_OP_KIND_*                          */
	uint8_t     min_ins;
	uint8_t     max_ins;
	uint8_t     min_outs;
	uint8_t     max_outs;
	const char *required_attrs; /* comma separated attr key list      */
} qnn_op_info_t;

const qnn_op_info_t *qnn_op_lookup(const char *name);
uint32_t qnn_op_count(void);
const qnn_op_info_t *qnn_op_table(void);

/* Validate an addNode parameter set: op registered, I/O counts in range,
   required attributes present.  Returns QNN_SUCCESS / QNN_ERROR. */
int qnn_op_validate_config(const char *op_name, const char *const *attr_keys,
                           uint32_t num_attrs);

void qnn_op_parse_attrs(const Qnn_ConfigParam_t *cfg, uint32_t ncfg,
                        qnn_op_attrs_t *a);

#ifdef __cplusplus
}
#endif

#endif /* QNN_OPS_H */
