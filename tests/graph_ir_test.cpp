// The graph lowering (src/levelzero/graph_ir.h) against hand-encoded ONNX
// graphs: each fusion folds to the one operation the runtime launches, with
// the numbers a folded batch norm leaves in the weights; shape arithmetic
// stays on the host and what nothing reads is pruned; an op the lowering
// does not know fails naming the node; and the feature-pyramid rewrites of
// fuse_merges.  No model file and no device: this runs in every flavor.
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include "levelzero/graph_ir.h"
#include "levelzero/onnx_initializers.h"
#include "support/check.h"
#include "support/onnx_bytes.h"

namespace {

using grparse::levelzero::parse_onnx_graph;
using grparse::levelzero::graph::Activation;
using grparse::levelzero::graph::IrGraph;
using grparse::levelzero::graph::IrNode;
using grparse::levelzero::graph::IrOp;
using grparse::levelzero::graph::lower_onnx_graph;
using grparse_test::require;
using grparse_test::require_equal;
using grparse_test::onnx::float_attribute;
using grparse_test::onnx::float_tensor;
using grparse_test::onnx::int64_tensor;
using grparse_test::onnx::int_attribute;
using grparse_test::onnx::ints_attribute;
using grparse_test::onnx::model;
using grparse_test::onnx::node;
using grparse_test::onnx::string_attribute;

using Names = std::vector<std::string>;

IrGraph lower(const Names& nodes, const Names& tensors, const std::string& output = "y") {
  return lower_onnx_graph(parse_onnx_graph(model(nodes, tensors, {"x"}, {output}), "test graph"), "test graph");
}

std::vector<float> floats(const IrGraph& graph, size_t at, size_t count) {
  return {graph.weights.begin() + std::ptrdiff_t(at), graph.weights.begin() + std::ptrdiff_t(at + count)};
}

void require_near(const std::vector<float>& actual, const std::vector<float>& expected, const std::string& what) {
  require_equal(actual.size(), expected.size(), what + ": count");
  for (size_t i = 0; i < actual.size(); ++i) {
    require(std::fabs(actual[i] - expected[i]) <= 1e-6F,
            std::format("{}: value {} is {}, expected {}", what, i, actual[i], expected[i]));
  }
}

const std::string& name_of(const IrGraph& graph, int value) { return graph.values[size_t(value)].name; }

// The graph's one node, which must produce the graph's output from its input.
const IrNode& only_node(const IrGraph& graph, const char* label) {
  const std::string what = label;
  require_equal(graph.nodes.size(), size_t(1), what + ": one operation is left");
  const IrNode& node = graph.nodes[0];
  require_equal(name_of(graph, node.inputs[0]), "x", what + ": it reads the graph input");
  require_equal(name_of(graph, node.output), "y", what + ": it writes the graph output");
  require_equal(graph.outputs.size(), size_t(1), what + ": one graph output");
  require_equal(graph.outputs[0], node.output, what + ": the graph output is its value");
  return node;
}

// A 1 x 1 convolution of one input channel to two outputs, weights 2 and 3.
std::string conv_weights() { return float_tensor("w", {2, 1, 1, 1}, {2, 3}); }

// Conv, BatchNormalization, then x * clip(x + 3, 0, 6) / 6: one convolution
// with the statistics in its weights and bias.
void folds_batch_norm_and_hard_swish_pattern() {
  const IrGraph graph = lower(
      {node("Conv", {"x", "w"}, "c"),
       node("BatchNormalization", {"c", "gamma", "beta", "mean", "var"}, "n", {float_attribute("epsilon", 1.0F)}),
       node("Add", {"n", "three"}, "a"), node("Clip", {"a", "zero", "six"}, "k"), node("Mul", {"n", "k"}, "m"),
       node("Div", {"m", "six"}, "y")},
      {conv_weights(), float_tensor("gamma", {2}, {1, 2}), float_tensor("beta", {2}, {0.5F, -1}),
       float_tensor("mean", {2}, {1, 0}), float_tensor("var", {2}, {3, 8}), float_tensor("three", {}, {3}),
       float_tensor("zero", {}, {0}), float_tensor("six", {}, {6})});
  const IrNode& conv = only_node(graph, "conv + batch norm + hard swish");
  require(conv.op == IrOp::kConv, "it is a convolution");
  require(conv.activation == Activation::kHardSwish, "the Add/Clip/Mul/Div pattern is its activation");
  require_equal(conv.out_channels, 2, "two output channels");
  require_equal(conv.k, 1, "one term an output");
  require_equal(conv.groups, 1, "groups default to 1");
  // scale = gamma / sqrt(var + eps): 1 / 2 and 2 / 3.  bias = (0 - mean) * scale + beta.
  require_near(floats(graph, conv.weights, 2), {1.0F, 2.0F}, "weights carry the batch norm's scale");
  require(conv.has_bias, "a folded batch norm leaves a bias");
  require_near(floats(graph, conv.bias, 2), {0.0F, -1.0F}, "bias carries the batch norm's shift");
}

// A bias that arrives as Add of a Reshape'd constant, Paddle's export.
void folds_reshaped_bias() {
  const IrGraph graph = lower(
      {node("Reshape", {"b", "shape"}, "b4"), node("Conv", {"x", "w"}, "c", {ints_attribute("strides", {2, 1})}),
       node("Add", {"c", "b4"}, "y")},
      {conv_weights(), float_tensor("b", {2}, {0.25F, -4}), int64_tensor("shape", {4}, {1, 2, 1, 1})});
  const IrNode& conv = only_node(graph, "conv + reshaped bias");
  require(conv.op == IrOp::kConv && conv.activation == Activation::kNone, "a convolution with no activation");
  require_equal(conv.stride_h, 2, "stride over rows");
  require_equal(conv.stride_w, 1, "stride over columns");
  require_near(floats(graph, conv.weights, 2), {2.0F, 3.0F}, "weights are untouched");
  require_near(floats(graph, conv.bias, 2), {0.25F, -4.0F}, "the constant behind the Reshape is the bias");
}

// x * sigmoid(x * 1), Paddle's Swish.
void folds_swish_pattern() {
  const IrGraph graph = lower({node("Conv", {"x", "w"}, "c"), node("Mul", {"c", "one"}, "s"),
                               node("Sigmoid", {"s"}, "g"), node("Mul", {"c", "g"}, "y")},
                              {conv_weights(), float_tensor("one", {}, {1})});
  require(only_node(graph, "conv + swish").activation == Activation::kSwish, "Mul/Sigmoid/Mul is Swish");
}

// The HardSwish operator rounds differently from the pattern and keeps its own activation.
void folds_hard_swish_operator() {
  const IrGraph graph = lower({node("Conv", {"x", "w"}, "c"), node("HardSwish", {"c"}, "y")}, {conv_weights()});
  require(only_node(graph, "conv + HardSwish").activation == Activation::kHardSwishOp,
          "the operator is not the pattern");
}

void folds_matmul_bias() {
  const IrGraph graph = lower({node("MatMul", {"x", "m"}, "p"), node("Add", {"p", "b"}, "y")},
                              {float_tensor("m", {2, 3}, {1, 2, 3, 4, 5, 6}), float_tensor("b", {3}, {7, 8, 9})});
  const IrNode& linear = only_node(graph, "MatMul + Add");
  require(linear.op == IrOp::kMatMulConst, "a MatMul by a constant");
  require_equal(linear.k, 2, "terms");
  require_equal(linear.n, 3, "outputs");
  require_near(floats(graph, linear.weights, 6), {1, 2, 3, 4, 5, 6}, "weights stay [K][N]");
  require(linear.has_bias, "the Add is its bias");
  require_near(floats(graph, linear.bias, 3), {7, 8, 9}, "bias");
}

// ReduceMean, Sub, Pow 2, ReduceMean, Add eps, Sqrt, Div, Mul gamma, Add beta.
void folds_layer_norm_pattern() {
  const IrGraph graph = lower(
      {node("ReduceMean", {"x"}, "mu", {ints_attribute("axes", {-1})}), node("Sub", {"x", "mu"}, "d"),
       node("Pow", {"d", "two"}, "sq"), node("ReduceMean", {"sq"}, "v", {ints_attribute("axes", {-1})}),
       node("Add", {"v", "eps"}, "ve"), node("Sqrt", {"ve"}, "sd"), node("Div", {"d", "sd"}, "q"),
       node("Mul", {"q", "gamma"}, "g"), node("Add", {"g", "beta"}, "y")},
      {float_tensor("two", {}, {2}), float_tensor("eps", {}, {0.5F}), float_tensor("gamma", {3}, {1, 2, 3}),
       float_tensor("beta", {3}, {4, 5, 6})});
  const IrNode& norm = only_node(graph, "layer norm");
  require(norm.op == IrOp::kLayerNorm, "nine nodes are one LayerNorm");
  require_equal(norm.alpha, 0.5F, "its epsilon");
  require_equal(norm.n, 3, "its width");
  require_near(floats(graph, norm.weights, 3), {1, 2, 3}, "gamma");
  require_near(floats(graph, norm.bias, 3), {4, 5, 6}, "beta");
}

// A lone ReduceMean that is not a LayerNorm's first node stays itself.
void keeps_a_plain_reduce_mean() {
  const IrGraph graph = lower({node("ReduceMean", {"x"}, "y", {ints_attribute("axes", {-1})})}, {});
  require(only_node(graph, "ReduceMean").op == IrOp::kReduceMean, "a ReduceMean");
}

// Reshape(x, Concat(Slice(Shape(x)), constant)): integer arithmetic for the
// host, in order.  A second Shape branch nothing reads is gone.
void keeps_shape_arithmetic_on_the_host_and_prunes_the_unread() {
  const IrGraph graph = lower(
      {node("Shape", {"x"}, "s"), node("Shape", {"x"}, "unread"), node("Gather", {"unread", "zero"}, "unread0"),
       node("Slice", {"s", "starts", "ends"}, "batch"),
       node("Concat", {"batch", "tail"}, "shape", {int_attribute("axis", 0)}), node("Reshape", {"x", "shape"}, "y")},
      {int64_tensor("zero", {}, {0}), int64_tensor("starts", {1}, {0}), int64_tensor("ends", {1}, {1}),
       int64_tensor("tail", {1}, {-1})});
  const std::vector<std::string> expected{"Shape", "Slice", "Concat", "Reshape"};
  require_equal(graph.nodes.size(), expected.size(), "the unread Shape and Gather are pruned");
  for (size_t i = 0; i < expected.size(); ++i) {
    require(graph.nodes[i].op == IrOp::kHost, expected[i] + " runs on the host");
    require_equal(graph.nodes[i].onnx_op, expected[i], "host nodes keep their order and op");
  }
  require(graph.values[size_t(graph.nodes[0].output)].host, "a Shape's output is host integers");
  const auto& tail = graph.values[size_t(graph.nodes[2].inputs[1])];
  require(tail.constant && tail.host && tail.ints == std::vector<int64_t>{-1}, "an int64 constant is a host value");
}

void refuses_an_unknown_op_by_name() {
  std::string message;
  try {
    (void)lower({node("Erf", {"x"}, "y")}, {});
  } catch (const std::runtime_error& error) {
    message = error.what();
  }
  require_equal(message, "test graph: node y (Erf) is not an op the graph lowering knows",
                "an unknown op fails naming the graph, the node and the op");
}

void refuses_a_dilated_convolution() {
  bool threw = false;
  try {
    (void)lower({node("Conv", {"x", "w"}, "y", {ints_attribute("dilations", {2, 2})})}, {conv_weights()});
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()).find("y is not a symmetrically padded, undilated convolution") !=
            std::string::npos;
  }
  require(threw, "a dilated convolution is refused by name");
}

// The squeeze-excite block of the detector's neck: f + f * gate, the gate
// two convolutions over f's global average pool.  One ScaleMerge replaces
// the Mul and the Add.
void fuses_an_excite_into_one_merge() {
  const IrGraph graph = lower(
      {node("Conv", {"x", "w"}, "f"), node("GlobalAveragePool", {"f"}, "p"), node("Conv", {"p", "w1"}, "r0"),
       node("Relu", {"r0"}, "r"), node("Conv", {"r", "w2"}, "g0"),
       node("HardSigmoid", {"g0"}, "g", {float_attribute("alpha", 0.2F), float_attribute("beta", 0.5F)}),
       node("Mul", {"f", "g"}, "m"), node("Add", {"f", "m"}, "y")},
      {conv_weights(), float_tensor("w1", {1, 2, 1, 1}, {1, 1}), float_tensor("w2", {2, 1, 1, 1}, {1, 1})});
  require_equal(graph.nodes.size(), size_t(5), "conv, pool, two gate convs and the merge");
  require(graph.nodes[2].activation == Activation::kRelu && graph.nodes[3].activation == Activation::kHardSigmoid,
          "the gate's activations are fused into its convolutions");
  require_equal(graph.nodes[3].alpha, 0.2F, "HardSigmoid alpha");
  const IrNode& merge = graph.nodes[4];
  require(merge.op == IrOp::kScaleMerge, "the Add became a ScaleMerge");
  require_equal(merge.inputs.size(), size_t(3), "x, gate and no upsampled term");
  require_equal(name_of(graph, merge.inputs[0]), "f", "x is the feature map");
  require_equal(name_of(graph, merge.inputs[1]), "g", "the gate");
  require_equal(merge.inputs[2], -1, "nothing is added");
  require(merge.ints == std::vector<int64_t>{1, 1}, "no upsampling");
}

Names nearest_resize_attributes() {
  return {string_attribute("mode", "nearest"), string_attribute("coordinate_transformation_mode", "asymmetric"),
          string_attribute("nearest_mode", "floor")};
}

// The pyramid's top-down path: a + nearest(b, x2), and a channel Concat of
// an upsampled part with a plain one.
void fuses_upsampled_adds_and_concats() {
  const Names tensors{float_tensor("wa", {2, 1, 1, 1}, {1, 1}), float_tensor("wb", {2, 1, 1, 1}, {1, 1}),
                      float_tensor("scales", {4}, {1, 1, 2, 2})};
  const IrGraph added = lower({node("Conv", {"x", "wa"}, "a"), node("Conv", {"x", "wb"}, "b"),
                               node("Resize", {"b", "", "scales"}, "up", nearest_resize_attributes()),
                               node("Add", {"a", "up"}, "y")},
                              tensors);
  require_equal(added.nodes.size(), size_t(3), "the Resize is absorbed");
  const IrNode& merge = added.nodes[2];
  require(merge.op == IrOp::kScaleMerge, "Add of a Resize is a ScaleMerge");
  require_equal(name_of(added, merge.inputs[0]), "a", "the full-size term");
  require_equal(merge.inputs[1], -1, "no gate");
  require_equal(name_of(added, merge.inputs[2]), "b", "the term to upsample");
  require(merge.ints == std::vector<int64_t>{1, 2}, "upsampled by 2");

  const IrGraph joined = lower({node("Conv", {"x", "wa"}, "a"), node("Conv", {"x", "wb"}, "b"),
                                node("Resize", {"b", "", "scales"}, "up", nearest_resize_attributes()),
                                node("Concat", {"up", "a"}, "y", {int_attribute("axis", 1)})},
                               tensors);
  require_equal(joined.nodes.size(), size_t(3), "the Resize is absorbed into the Concat");
  const IrNode& concat = joined.nodes[2];
  require(concat.op == IrOp::kConcatScaled, "a channel Concat of a Resize is a ConcatScaled");
  require_equal(concat.inputs.size(), size_t(4), "a part and a gate slot for each input");
  require_equal(name_of(joined, concat.inputs[0]), "b", "first part, before its upsampling");
  require_equal(name_of(joined, concat.inputs[2]), "a", "second part");
  require(concat.inputs[1] == -1 && concat.inputs[3] == -1, "neither part is gated");
  require(concat.ints == std::vector<int64_t>{2, 1}, "the first part is upsampled by 2");
}

// A Resize by a fractional factor is no whole-pixel copy and stays a Resize.
void leaves_a_fractional_resize_alone() {
  const IrGraph graph = lower({node("Conv", {"x", "wa"}, "a"), node("Conv", {"x", "wb"}, "b"),
                               node("Resize", {"b", "", "scales"}, "up", nearest_resize_attributes()),
                               node("Add", {"a", "up"}, "y")},
                              {float_tensor("wa", {2, 1, 1, 1}, {1, 1}), float_tensor("wb", {2, 1, 1, 1}, {1, 1}),
                               float_tensor("scales", {4}, {1, 1, 1.5F, 1.5F})});
  require_equal(graph.nodes.size(), size_t(4), "nothing is absorbed");
  require(graph.nodes[2].op == IrOp::kResize && graph.nodes[3].op == IrOp::kBinary, "Resize and Add stay");
}

}  // namespace

int main() {
  return grparse_test::run_test_main(
      "graph-ir-test", "graph lowering ok",
      {folds_batch_norm_and_hard_swish_pattern, folds_reshaped_bias, folds_swish_pattern, folds_hard_swish_operator,
       folds_matmul_bias, folds_layer_norm_pattern, keeps_a_plain_reduce_mean,
       keeps_shape_arithmetic_on_the_host_and_prunes_the_unread, refuses_an_unknown_op_by_name,
       refuses_a_dilated_convolution, fuses_an_excite_into_one_merge, fuses_upsampled_adds_and_concats,
       leaves_a_fractional_resize_alone});
}
