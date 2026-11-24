#pragma once

// An ONNX graph lowered to the operations the Level Zero graph runtime
// (graph_runtime.h) launches.  The lowering fuses what the vision models in
// models/ are built from: a convolution with its batch norm, its bias add and
// its activation (ReLU, HardSigmoid, Sigmoid, and the HardSwish and Swish
// patterns Paddle exports as Add/Clip/Mul/Div and Mul/Sigmoid/Mul); a
// MatMul with its bias add; the LayerNorm pattern (ReduceMean, Sub, Pow,
// ReduceMean, Add, Sqrt, Div, Mul, Add).  Everything else lowers one op to
// one operation.  Shapes stay symbolic: the runtime computes them per call
// from the input shape, so a new input size needs nothing compiled.  The
// shape-computing subgraphs (Shape, Slice, Concat, Cast, Gather, Unsqueeze
// on integers) are marked to run on the host.  An op or attribute the
// lowering does not know fails it, naming the node.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grparse::levelzero::graph {

enum class Activation { kNone = 0, kHardSwish = 1, kRelu = 2, kHardSigmoid = 3, kSigmoid = 4, kSwish = 5 };

enum class IrOp {
  kConv,           // NCHW, any groups, BN and bias folded, fused activation
  kConvTranspose,  // NCHW, groups 1, stride = kernel, no padding
  kMatMulConst,    // [..., K] x const [K, N] + bias, fused activation
  kMatMul,         // batched, both operands computed, broadcast over leading dims
  kLayerNorm,      // over the last axis, gamma and beta
  kBinary,         // Add Sub Mul Div Pow with broadcasting
  kUnary,          // activation of x (Relu, Sigmoid, HardSigmoid, Clip, Sqrt)
  kGlobalAveragePool,
  kAveragePool,
  kMaxPool,
  kResize,         // nearest, asymmetric, floor
  kScaleMerge,     // x * (1 + s) + nearest(y): inputs x, s, y (s, y may be -1); ints = up factors of x and y
  kConcatScaled,   // channel concat of nearest(x_i) * (1 + s_i): inputs x0, s0, x1, s1, ...; ints = up factors
  kTranspose,
  kSlice,
  kConcat,
  kSoftmax,        // last axis
  kReduceMean,     // last axis, keepdims
  kView,           // Reshape, Squeeze, Unsqueeze, Flatten, Identity, Cast: same data, new shape
  kHost,           // integer shape arithmetic, evaluated on the host
};

enum class BinaryOp { kAdd = 0, kSub = 1, kMul = 2, kDiv = 3, kPow = 4 };
enum class UnaryOp { kRelu = 0, kSigmoid = 1, kHardSigmoid = 2, kClip = 3, kSqrt = 4 };

struct IrNode {
  IrOp op = IrOp::kView;
  std::string onnx_op = {};               // the ONNX op type it came from (kHost and kView keep it)
  std::vector<int> inputs = {};           // value ids
  int output = -1;
  // Convolutions and pools.
  int kernel_h = 1, kernel_w = 1, stride_h = 1, stride_w = 1, pad_h = 0, pad_w = 0, groups = 1;
  int out_channels = 0;
  bool ceil_mode = false;
  bool count_include_pad = false;
  // Weights in the runtime's weight block, floats: [Co][Ci/g][kh][kw] for a
  // convolution, [Ci][Co][kh][kw] for a transposed one, [K][N] for MatMulConst.
  size_t weights = 0, bias = 0;
  bool has_bias = false;
  int k = 0, n = 0;                  // MatMulConst
  Activation activation = Activation::kNone;
  float alpha = 0.0F, beta = 0.0F;   // HardSigmoid, Clip bounds, LayerNorm epsilon (alpha)
  BinaryOp binary = BinaryOp::kAdd;
  UnaryOp unary = UnaryOp::kRelu;
  std::vector<int64_t> ints = {};         // Transpose perm, Slice/Reduce axes, Resize scales x1000
  int axis = 0;                      // Concat, Softmax
  std::string name = {};                  // the ONNX output, for errors
};

struct IrValue {
  std::string name = {};
  bool constant = false;      // a weight-block tensor (float) or a host integer tensor
  bool host = false;          // integers known on the host (constant or computed)
  size_t weights = 0;         // a float constant's start in the weight block
  std::vector<int64_t> shape = {};  // a constant's shape
  std::vector<int64_t> ints = {};   // a host constant's values
};

struct IrGraph {
  std::vector<IrValue> values = {};
  std::vector<IrNode> nodes = {};  // in execution order
  std::vector<float> weights = {};  // every float constant and folded weight, 16-float aligned
  int input = -1;
  std::vector<int> outputs = {};
};

IrGraph lower_onnx(const std::filesystem::path& model);

// Rewrites, in place, the launches a feature pyramid with squeeze-excite
// blocks spends on whole-tensor arithmetic (graph_fuse.cpp): the excite
// Mul and its residual Add into one kScaleMerge; a constant integer nearest
// Resize and the Add that reads it into the same node; and a channel Concat
// of such results into one kConcatScaled that writes each part in place.
void fuse_merges(IrGraph* graph);

}  // namespace grparse::levelzero::graph
