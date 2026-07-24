/**
 * onnx.h - minimal self-contained ONNX inference engine (float32).
 *
 * Written for the EwokOS sherpa-onnx port. No protobuf, no onnxruntime:
 * parses the .onnx protobuf wire format directly and executes the graph
 * with plain C++ (C++14, no exceptions, no RTTI).
 *
 * Supported op set targets speech models exported for sherpa-onnx
 * (CTC encoders such as zipformer/conformer, small CNN KWS models):
 *   Conv, MatMul, Gemm, Add/Sub/Mul/Div/Pow, Relu, Sigmoid, Tanh, Erf,
 *   Exp, Log, Sqrt, Abs, Neg, Floor, Clip, Softmax, LogSoftmax,
 *   LayerNormalization, BatchNormalization, MaxPool, AveragePool,
 *   GlobalAveragePool, GlobalMaxPool, Reshape, Flatten, Squeeze,
 *   Unsqueeze, Transpose, Concat, Split, Expand, Tile, Slice, Pad,
 *   Gather, Shape, Size, Cast, Constant, ConstantOfShape, ReduceMean,
 *   Identity, Dropout, DequantizeLinear.
 *
 * Only float32 (and int64/int32 index tensors) are supported end to end;
 * quantized models must be converted to float32 first.
 */
#ifndef SHERPA_ONNX_PORT_ONNX_H_
#define SHERPA_ONNX_PORT_ONNX_H_

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace sonnx {

enum DType {
  kFloat = 1,
  kUint8 = 2,
  kInt8 = 3,
  kInt32 = 6,
  kInt64 = 7,
  kBool = 9,
};

struct Tensor {
  int dtype = kFloat;
  std::vector<int64_t> shape;
  std::vector<float> f;     // float payload (dtype == kFloat)
  std::vector<int64_t> i;   // integer payload (kInt32/kInt64)
  std::vector<uint8_t> b;   // byte payload (kUint8/kInt8/kBool)

  int64_t Numel() const;
  bool IsInt() const { return dtype != kFloat; }
  bool IsByte() const { return dtype == kUint8 || dtype == kInt8 || dtype == kBool; }

  // ewokstl vector has no data(); storage is contiguous so &v[0] is safe
  // (guarded for empty vectors).
  float *pf() { return f.empty() ? (float *)0 : &f[0]; }
  const float *pf() const { return f.empty() ? (const float *)0 : &f[0]; }
  int64_t *pi() { return i.empty() ? (int64_t *)0 : &i[0]; }
  const int64_t *pi() const { return i.empty() ? (const int64_t *)0 : &i[0]; }
  uint8_t *pb() { return b.empty() ? (uint8_t *)0 : &b[0]; }
  const uint8_t *pb() const { return b.empty() ? (const uint8_t *)0 : &b[0]; }

  // Quantized element read (byte payload, signed for kInt8).
  int32_t QAt(int64_t idx) const {
    return dtype == kInt8 ? static_cast<int32_t>(static_cast<int8_t>(b[idx]))
                          : static_cast<int32_t>(b[idx]);
  }
  // Get element as float/int regardless of storage.
  float AtFloat(int64_t idx) const {
    if (dtype == kFloat) return f[idx];
    if (IsByte()) return static_cast<float>(QAt(idx));
    return static_cast<float>(i[idx]);
  }
  int64_t AtInt(int64_t idx) const {
    if (dtype == kFloat) return static_cast<int64_t>(f[idx]);
    if (IsByte()) return static_cast<int64_t>(QAt(idx));
    return i[idx];
  }

  static Tensor Float(std::vector<int64_t> shape);
  static Tensor Int(std::vector<int64_t> shape, int dtype = kInt64);
  static Tensor Byte(std::vector<int64_t> shape, int dtype = kUint8);

  // ONNX sequence payload (owned); non-null marks a sequence value
  // (SequenceEmpty/SequenceInsert/ConcatFromSequence, Loop carried deps)
  std::vector<Tensor> *seq = nullptr;
  bool IsSeq() const { return seq != nullptr; }

  Tensor() {}
  Tensor(const Tensor &o);
  Tensor(Tensor &&o);
  Tensor &operator=(const Tensor &o);
  Tensor &operator=(Tensor &&o);
  ~Tensor();
};

inline Tensor::Tensor(const Tensor &o)
    : dtype(o.dtype), shape(o.shape), f(o.f), i(o.i), b(o.b),
      seq(o.seq ? new std::vector<Tensor>(*o.seq) : nullptr) {}
inline Tensor::Tensor(Tensor &&o)
    : dtype(o.dtype), shape(static_cast<std::vector<int64_t> &&>(o.shape)),
      f(static_cast<std::vector<float> &&>(o.f)),
      i(static_cast<std::vector<int64_t> &&>(o.i)),
      b(static_cast<std::vector<uint8_t> &&>(o.b)), seq(o.seq) {
  o.seq = nullptr;
}
inline Tensor &Tensor::operator=(const Tensor &o) {
  if (this != &o) {
    dtype = o.dtype; shape = o.shape; f = o.f; i = o.i; b = o.b;
    delete seq;
    seq = o.seq ? new std::vector<Tensor>(*o.seq) : nullptr;
  }
  return *this;
}
inline Tensor &Tensor::operator=(Tensor &&o) {
  if (this != &o) {
    dtype = o.dtype;
    shape = static_cast<std::vector<int64_t> &&>(o.shape);
    f = static_cast<std::vector<float> &&>(o.f);
    i = static_cast<std::vector<int64_t> &&>(o.i);
    b = static_cast<std::vector<uint8_t> &&>(o.b);
    delete seq;
    seq = o.seq;
    o.seq = nullptr;
  }
  return *this;
}
inline Tensor::~Tensor() { delete seq; }

struct Graph;  // defined below (subgraph bodies for Loop/If/Scan)

// Node/Attr live at namespace scope so subgraphs (Graph) can hold Nodes.
struct Attr {
  int type = 0;  // 0 none, 1 int, 2 float, 3 string, 4 tensor, 5 ints,
                 // 6 floats, 7 graph
  int64_t i = 0;
  float f = 0.0f;
  std::string s;
  Tensor t;
  std::vector<int64_t> ints;
  std::vector<float> floats;
  Graph *g = nullptr;  // owned subgraph (type == 7)

  Attr() {}
  Attr(const Attr &o);
  Attr(Attr &&o);
  Attr &operator=(const Attr &o);
  Attr &operator=(Attr &&o);
  ~Attr();
};

struct Node {
  std::string op;
  std::vector<std::string> input;
  std::vector<std::string> output;
  std::map<std::string, Attr> attr;
};

// GraphProto payload; used for the top-level graph and Loop/If bodies.
struct Graph {
  std::vector<Node> nodes;
  std::map<std::string, Tensor> initializers;
  std::vector<std::string> input_names;
  std::vector<std::string> output_names;
};

inline Attr::Attr(const Attr &o)
    : type(o.type), i(o.i), f(o.f), s(o.s), t(o.t), ints(o.ints),
      floats(o.floats), g(o.g ? new Graph(*o.g) : nullptr) {}
inline Attr::Attr(Attr &&o)
    : type(o.type), i(o.i), f(o.f), s(static_cast<std::string &&>(o.s)),
      t(static_cast<Tensor &&>(o.t)),
      ints(static_cast<std::vector<int64_t> &&>(o.ints)),
      floats(static_cast<std::vector<float> &&>(o.floats)), g(o.g) {
  o.g = nullptr;
}
inline Attr &Attr::operator=(const Attr &o) {
  if (this != &o) {
    type = o.type; i = o.i; f = o.f; s = o.s; t = o.t;
    ints = o.ints; floats = o.floats;
    delete g;
    g = o.g ? new Graph(*o.g) : nullptr;
  }
  return *this;
}
inline Attr &Attr::operator=(Attr &&o) {
  if (this != &o) {
    type = o.type; i = o.i; f = o.f;
    s = static_cast<std::string &&>(o.s);
    t = static_cast<Tensor &&>(o.t);
    ints = static_cast<std::vector<int64_t> &&>(o.ints);
    floats = static_cast<std::vector<float> &&>(o.floats);
    delete g;
    g = o.g;
    o.g = nullptr;
  }
  return *this;
}
inline Attr::~Attr() { delete g; }

class Model {
 public:
  // Load a .onnx file. Returns false on failure (message printed to stderr).
  bool Load(const char *path);

  // Number of external (non-initializer) graph inputs.
  int NumInputs() const { return static_cast<int>(input_names_.size()); }
  const char *InputName(int index) const { return input_names_[index].c_str(); }
  std::vector<int64_t> InputShape(int index) const;  // -1 marks dynamic dims

  int NumOutputs() const { return static_cast<int>(output_names_.size()); }
  const char *OutputName(int index) const { return output_names_[index].c_str(); }

  // inputs[i] corresponds to InputName(i). Returns false on failure.
  bool Run(const std::vector<const Tensor *> &inputs,
           std::vector<Tensor> *outputs) const;

  // aliases kept so existing Model::Node / Model::Attr spellings compile
  typedef sonnx::Attr Attr;
  typedef sonnx::Node Node;

  std::vector<Node> nodes_;
  std::map<std::string, Tensor> initializers_;
  std::vector<std::string> input_names_;
  std::vector<std::vector<int64_t>> input_shapes_;
  std::vector<std::string> output_names_;
  std::map<std::string, std::string> metadata_;  // ModelProto.metadata_props

  // Model metadata (custom_props); returns "" when absent.
  std::string Metadata(const char *key) const {
    std::map<std::string, std::string>::const_iterator it = metadata_.find(key);
    return it == metadata_.end() ? std::string() : it->second;
  }

 private:
  bool Parse(const uint8_t *data, size_t len);
};

// Model::Load progress in percent (0..100); safe to poll from another
// thread while Load runs (file read maps to 0..95, parse to 95..100).
// The split reflects the real time distribution on SD-card targets:
// reading dominates, parsing is memcpy-bound and nearly instant.
extern volatile int g_load_progress;

}  // namespace sonnx

#endif  // SHERPA_ONNX_PORT_ONNX_H_
