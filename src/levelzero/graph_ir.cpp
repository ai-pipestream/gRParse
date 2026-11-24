#include "levelzero/graph_ir.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "levelzero/onnx_initializers.h"

namespace grparse::levelzero::graph {
namespace {

constexpr int kFloat = 1;

class Lowering {
 public:
  Lowering(OnnxGraph graph, std::string what) : g_(std::move(graph)), what_(std::move(what)) {
    for (size_t index = 0; index < g_.nodes.size(); ++index) {
      for (const std::string& input : g_.nodes[index].inputs) {
        if (!input.empty()) consumers_[input].push_back(index);
      }
    }
    for (const std::string& output : g_.outputs) consumers_[output].push_back(SIZE_MAX);
  }

  IrGraph run() {
    if (g_.inputs.size() != 1) fail("the graph", "one input");
    ir_.input = value_of(g_.inputs[0]);
    for (size_t index = 0; index < g_.nodes.size(); ++index) {
      if (!done_.contains(index)) lower(index);
    }
    for (const std::string& output : g_.outputs) ir_.outputs.push_back(value_of(output));
    prune();
    return std::move(ir_);
  }

 private:
  [[noreturn]] void fail(const std::string& where, const std::string& expected) const {
    throw std::runtime_error(std::format("{}: {} is not {}", what_, where, expected));
  }

  int value_of(const std::string& name) {
    const auto found = ids_.find(name);
    if (found != ids_.end()) return found->second;
    IrValue value{.name = name};
    if (const auto c = g_.constants.find(name); c != g_.constants.end()) {
      value.constant = true;
      value.shape = c->second.dims;
      if (c->second.data_type == kFloat) {
        value.weights = add_weights(c->second.values);
      } else {
        value.host = true;
        value.ints = c->second.ints;
      }
    }
    ir_.values.push_back(std::move(value));
    return ids_[name] = int(ir_.values.size() - 1);
  }

  size_t add_weights(const std::vector<float>& values) {
    ir_.weights.resize((ir_.weights.size() + 15) / 16 * 16, 0.0F);
    const size_t at = ir_.weights.size();
    ir_.weights.insert(ir_.weights.end(), values.begin(), values.end());
    return at;
  }

  const OnnxTensor* constant(const std::string& name) const {
    const auto found = g_.constants.find(name);
    return found == g_.constants.end() ? nullptr : &found->second;
  }

  // A float constant's values, folding a Reshape of a constant (Paddle's
  // bias and LayerNorm parameters arrive that way).
  std::optional<std::vector<float>> float_constant(const std::string& name) const {
    if (const OnnxTensor* c = constant(name); c != nullptr && c->data_type == kFloat) return c->values;
    if (const auto producer = producer_of(name)) {
      const OnnxNode& node = g_.nodes[*producer];
      if (node.op_type == "Reshape" || node.op_type == "Identity" || node.op_type == "Cast") {
        return float_constant(node.inputs[0]);
      }
    }
    return std::nullopt;
  }

  std::optional<float> scalar(const std::string& name) const {
    const auto values = float_constant(name);
    if (!values || values->size() != 1) return std::nullopt;
    return (*values)[0];
  }

  std::optional<size_t> producer_of(const std::string& name) const {
    for (size_t index = 0; index < g_.nodes.size(); ++index) {
      if (std::ranges::find(g_.nodes[index].outputs, name) != g_.nodes[index].outputs.end()) return index;
    }
    return std::nullopt;
  }

  const std::vector<size_t>& consumers(const std::string& name) const {
    static const std::vector<size_t> none;
    const auto found = consumers_.find(name);
    return found == consumers_.end() ? none : found->second;
  }

  // The one node that reads `name`, when exactly one does and it is `op`.
  std::optional<size_t> sole(const std::string& name, const char* op) const {
    const auto& users = consumers(name);
    if (users.size() != 1 || users[0] == SIZE_MAX || g_.nodes[users[0]].op_type != op) return std::nullopt;
    return users[0];
  }

  const OnnxNode& node(size_t index) const { return g_.nodes[index]; }

  int64_t attr_int(const OnnxNode& n, const char* name, int64_t fallback) const {
    const OnnxAttribute* a = n.attribute(name);
    return a == nullptr ? fallback : a->i;
  }
  std::vector<int64_t> attr_ints(const OnnxNode& n, const char* name, std::vector<int64_t> fallback) const {
    const OnnxAttribute* a = n.attribute(name);
    return a == nullptr ? fallback : a->ints;
  }
  float attr_float(const OnnxNode& n, const char* name, float fallback) const {
    const OnnxAttribute* a = n.attribute(name);
    return a == nullptr ? fallback : a->f;
  }

  // After `name`, the activation the graph applies to it alone, consumed.
  // Returns the activation's output name.
  std::string fuse_activation(const std::string& name, IrNode* out) {
    if (auto relu = sole(name, "Relu")) {
      out->activation = Activation::kRelu;
      done_.insert(*relu);
      return node(*relu).outputs[0];
    }
    if (auto sigmoid = sole(name, "Sigmoid")) {
      out->activation = Activation::kSigmoid;
      done_.insert(*sigmoid);
      return node(*sigmoid).outputs[0];
    }
    if (auto hs = sole(name, "HardSigmoid")) {
      out->activation = Activation::kHardSigmoid;
      out->alpha = attr_float(node(*hs), "alpha", 0.2F);
      out->beta = attr_float(node(*hs), "beta", 0.5F);
      done_.insert(*hs);
      return node(*hs).outputs[0];
    }
    const auto& users = consumers(name);
    if (users.size() != 2 || users[0] == SIZE_MAX || users[1] == SIZE_MAX) return name;
    // HardSwish: Div(Mul(x, Clip(Add(x, 3), 0, 6)), 6).  Swish: Mul(x, Sigmoid(Mul(x, 1))).
    for (int order = 0; order < 2; ++order) {
      const size_t first = users[order], second = users[1 - order];
      const OnnxNode& a = node(first);
      const OnnxNode& m = node(second);
      if (m.op_type != "Mul" || std::ranges::find(m.inputs, name) == m.inputs.end()) continue;
      const std::string other = m.inputs[0] == name ? m.inputs[1] : m.inputs[0];
      if (a.op_type == "Add" && a.inputs[0] == name && scalar(a.inputs[1]) == 3.0F) {
        const auto clip = sole(a.outputs[0], "Clip");
        if (!clip || node(*clip).outputs[0] != other || scalar(node(*clip).inputs[1]) != 0.0F ||
            scalar(node(*clip).inputs[2]) != 6.0F) {
          continue;
        }
        const auto div = sole(m.outputs[0], "Div");
        if (!div || scalar(node(*div).inputs[1]) != 6.0F) continue;
        out->activation = Activation::kHardSwish;
        done_.insert({first, *clip, second, *div});
        return node(*div).outputs[0];
      }
      if (a.op_type == "Mul" && a.inputs[0] == name && scalar(a.inputs[1]) == 1.0F) {
        const auto sigmoid = sole(a.outputs[0], "Sigmoid");
        if (!sigmoid || node(*sigmoid).outputs[0] != other) continue;
        out->activation = Activation::kSwish;
        done_.insert({first, *sigmoid, second});
        return node(*sigmoid).outputs.empty() ? name : m.outputs[0];
      }
    }
    return name;
  }

  // A bias Add of a constant after `name`, folded into `bias`.
  std::string fuse_bias(const std::string& name, int channels, std::vector<double>* bias) {
    const auto add = sole(name, "Add");
    if (!add) return name;
    const OnnxNode& n = node(*add);
    const std::string& other = n.inputs[0] == name ? n.inputs[1] : n.inputs[0];
    const auto values = float_constant(other);
    if (!values || int(values->size()) != channels) return name;
    for (int c = 0; c < channels; ++c) (*bias)[size_t(c)] += (*values)[size_t(c)];
    done_.insert(*add);
    return n.outputs[0];
  }

  // BatchNormalization after `name`, folded into the per-output-channel
  // scale of `weights` (blocks of `per_output` floats, output-major unless
  // `input_major`) and into `bias`.
  std::string fuse_batch_norm(const std::string& name, int channels, size_t per_output, bool input_major,
                              int in_channels, std::vector<double>* weights, std::vector<double>* bias) {
    const auto bn = sole(name, "BatchNormalization");
    if (!bn) return name;
    const OnnxNode& n = node(*bn);
    const auto gamma = float_constant(n.inputs[1]), beta = float_constant(n.inputs[2]);
    const auto mean = float_constant(n.inputs[3]), var = float_constant(n.inputs[4]);
    if (!gamma || !beta || !mean || !var) fail(n.outputs[0], "a batch norm with constant statistics");
    const double eps = attr_float(n, "epsilon", 1e-5F);
    for (int c = 0; c < channels; ++c) {
      const double scale = (*gamma)[size_t(c)] / std::sqrt(double((*var)[size_t(c)]) + eps);
      if (input_major) {
        for (int i = 0; i < in_channels; ++i) {
          for (size_t at = 0; at < per_output; ++at) (*weights)[(size_t(i) * channels + c) * per_output + at] *= scale;
        }
      } else {
        for (size_t at = 0; at < per_output; ++at) (*weights)[size_t(c) * per_output + at] *= scale;
      }
      (*bias)[size_t(c)] = ((*bias)[size_t(c)] - (*mean)[size_t(c)]) * scale + (*beta)[size_t(c)];
    }
    done_.insert(*bn);
    return n.outputs[0];
  }

  size_t store(const std::vector<double>& values) {
    std::vector<float> narrow(values.begin(), values.end());
    return add_weights(narrow);
  }

  void emit(IrNode n, const std::string& output) {
    n.output = value_of(output);
    n.name = output;
    ir_.nodes.push_back(std::move(n));
  }

  void lower_conv(const OnnxNode& n, bool transposed) {
    const OnnxTensor* w = constant(n.inputs[1]);
    if (w == nullptr || w->dims.size() != 4) fail(n.outputs[0], "a 2-D convolution with constant weights");
    const auto kernel = attr_ints(n, "kernel_shape", {w->dims[2], w->dims[3]});
    const auto strides = attr_ints(n, "strides", {1, 1});
    const auto pads = attr_ints(n, "pads", {0, 0, 0, 0});
    const auto dilations = attr_ints(n, "dilations", {1, 1});
    IrNode out{.op = transposed ? IrOp::kConvTranspose : IrOp::kConv, .onnx_op = n.op_type};
    out.inputs = {value_of(n.inputs[0])};
    out.kernel_h = int(kernel[0]);
    out.kernel_w = int(kernel[1]);
    out.stride_h = int(strides[0]);
    out.stride_w = int(strides[1]);
    out.pad_h = int(pads[0]);
    out.pad_w = int(pads[1]);
    out.groups = int(attr_int(n, "group", 1));
    if (pads[2] != pads[0] || pads[3] != pads[1] || dilations != std::vector<int64_t>{1, 1}) {
      fail(n.outputs[0], "a symmetrically padded, undilated convolution");
    }
    const int channels = transposed ? int(w->dims[1]) * out.groups : int(w->dims[0]);
    if (transposed && (out.groups != 1 || out.pad_h != 0 || out.pad_w != 0)) {
      fail(n.outputs[0], "a groups-1, unpadded transposed convolution");
    }
    out.out_channels = channels;
    std::vector<double> weights(w->values.begin(), w->values.end());
    std::vector<double> bias(size_t(channels), 0.0);
    if (n.inputs.size() > 2 && !n.inputs[2].empty()) {
      const auto b = float_constant(n.inputs[2]);
      if (!b) fail(n.outputs[0], "a convolution with a constant bias");
      std::copy(b->begin(), b->end(), bias.begin());
    }
    const size_t per_output = size_t(out.kernel_h) * out.kernel_w * (transposed ? 1 : size_t(w->dims[1]));
    std::string name = n.outputs[0];
    name = fuse_bias(name, channels, &bias);
    name = fuse_batch_norm(name, channels, per_output, transposed, int(w->dims[0]), &weights, &bias);
    name = fuse_bias(name, channels, &bias);
    name = fuse_activation(name, &out);
    out.weights = store(weights);
    out.bias = store(bias);
    out.has_bias = true;
    emit(std::move(out), name);
  }

  void lower_matmul(const OnnxNode& n) {
    const OnnxTensor* w = constant(n.inputs[1]);
    if (w == nullptr) {
      emit({.op = IrOp::kMatMul, .onnx_op = n.op_type, .inputs = {value_of(n.inputs[0]), value_of(n.inputs[1])}},
           n.outputs[0]);
      return;
    }
    if (w->dims.size() != 2) fail(n.outputs[0], "a MatMul by a 2-D constant");
    IrNode out{.op = IrOp::kMatMulConst, .onnx_op = n.op_type, .inputs = {value_of(n.inputs[0])}};
    out.k = int(w->dims[0]);
    out.n = int(w->dims[1]);
    std::vector<double> bias(size_t(out.n), 0.0);
    std::string name = fuse_bias(n.outputs[0], out.n, &bias);
    out.has_bias = true;
    name = fuse_activation(name, &out);
    out.weights = add_weights(w->values);
    out.bias = store(bias);
    emit(std::move(out), name);
  }

  // ReduceMean, Sub, Pow 2, ReduceMean, Add eps, Sqrt, Div, Mul gamma, Add beta.
  bool lower_layer_norm(size_t index) {
    const OnnxNode& mean = node(index);
    const std::string& x = mean.inputs[0];
    const auto& users = consumers(mean.outputs[0]);
    if (users.size() != 1 || users[0] == SIZE_MAX) return false;
    const OnnxNode& sub = node(users[0]);
    if (sub.op_type != "Sub" || sub.inputs[0] != x) return false;
    const auto& sub_users = consumers(sub.outputs[0]);
    if (sub_users.size() != 2) return false;
    std::optional<size_t> pow, div;
    for (size_t user : sub_users) {
      if (user == SIZE_MAX) return false;
      if (node(user).op_type == "Pow" && scalar(node(user).inputs[1]) == 2.0F) pow = user;
      if (node(user).op_type == "Div" && node(user).inputs[0] == sub.outputs[0]) div = user;
    }
    if (!pow || !div) return false;
    const auto variance = sole(node(*pow).outputs[0], "ReduceMean");
    if (!variance) return false;
    const auto add_eps = sole(node(*variance).outputs[0], "Add");
    if (!add_eps) return false;
    const auto eps = scalar(node(*add_eps).inputs[1]);
    const auto sqrt = sole(node(*add_eps).outputs[0], "Sqrt");
    if (!eps || !sqrt || node(*div).inputs[1] != node(*sqrt).outputs[0]) return false;
    const auto mul = sole(node(*div).outputs[0], "Mul");
    if (!mul) return false;
    const auto gamma = float_constant(node(*mul).inputs[1]);
    const auto add = sole(node(*mul).outputs[0], "Add");
    if (!gamma || !add) return false;
    const auto beta = float_constant(node(*add).inputs[1]);
    if (!beta || beta->size() != gamma->size()) return false;
    IrNode out{.op = IrOp::kLayerNorm, .onnx_op = "LayerNorm", .inputs = {value_of(x)}};
    out.alpha = *eps;
    out.n = int(gamma->size());
    out.weights = add_weights(*gamma);
    out.bias = add_weights(*beta);
    done_.insert({index, users[0], *pow, *variance, *add_eps, *sqrt, *div, *mul, *add});
    emit(std::move(out), node(*add).outputs[0]);
    return true;
  }

  void lower(size_t index) {
    const OnnxNode& n = node(index);
    const std::string& op = n.op_type;
    const std::string& out = n.outputs.empty() ? op : n.outputs[0];
    const auto inputs = [&] {
      std::vector<int> ids;
      for (const std::string& input : n.inputs) ids.push_back(input.empty() ? -1 : value_of(input));
      return ids;
    };
    if (op == "Constant") return;
    if (op == "Conv") return lower_conv(n, false);
    if (op == "ConvTranspose") return lower_conv(n, true);
    if (op == "MatMul") return lower_matmul(n);
    if (op == "ReduceMean" && lower_layer_norm(index)) return;
    if (op == "Shape" || op == "Gather" || op == "Unsqueeze" || op == "Squeeze" || op == "Slice" ||
        op == "Concat" || op == "Cast" || op == "Reshape") {
      // Integer arithmetic on shapes runs on the host; on data these are views
      // or copies the runtime decides per call from the input's host-ness.
      IrNode node_ir{.op = IrOp::kHost, .onnx_op = op, .inputs = inputs()};
      node_ir.ints = attr_ints(n, "axes", {});
      node_ir.axis = int(attr_int(n, "axis", 0));
      node_ir.alpha = float(attr_int(n, "to", 0));
      if (op == "Shape") ir_.values[size_t(value_of(out))].host = true;
      emit(std::move(node_ir), out);
      return;
    }
    if (op == "Identity" || op == "Flatten") {
      IrNode node_ir{.op = IrOp::kView, .onnx_op = op, .inputs = inputs()};
      node_ir.axis = int(attr_int(n, "axis", 1));
      emit(std::move(node_ir), out);
      return;
    }
    if (op == "Add" || op == "Sub" || op == "Mul" || op == "Div" || op == "Pow") {
      const BinaryOp kinds[] = {BinaryOp::kAdd, BinaryOp::kSub, BinaryOp::kMul, BinaryOp::kDiv, BinaryOp::kPow};
      const char* names[] = {"Add", "Sub", "Mul", "Div", "Pow"};
      IrNode node_ir{.op = IrOp::kBinary, .onnx_op = op, .inputs = inputs()};
      for (int k = 0; k < 5; ++k) {
        if (op == names[k]) node_ir.binary = kinds[k];
      }
      emit(std::move(node_ir), out);
      return;
    }
    if (op == "Relu" || op == "Sigmoid" || op == "HardSigmoid" || op == "Clip" || op == "Sqrt") {
      IrNode node_ir{.op = IrOp::kUnary, .onnx_op = op, .inputs = {value_of(n.inputs[0])}};
      node_ir.unary = op == "Relu" ? UnaryOp::kRelu
                      : op == "Sigmoid" ? UnaryOp::kSigmoid
                      : op == "HardSigmoid" ? UnaryOp::kHardSigmoid
                      : op == "Clip" ? UnaryOp::kClip
                                     : UnaryOp::kSqrt;
      node_ir.alpha = op == "HardSigmoid" ? attr_float(n, "alpha", 0.2F) : 0.0F;
      node_ir.beta = op == "HardSigmoid" ? attr_float(n, "beta", 0.5F) : 0.0F;
      if (op == "Clip") {
        const auto low = n.inputs.size() > 1 ? scalar(n.inputs[1]) : std::nullopt;
        const auto high = n.inputs.size() > 2 ? scalar(n.inputs[2]) : std::nullopt;
        node_ir.alpha = low.value_or(-INFINITY);
        node_ir.beta = high.value_or(INFINITY);
      }
      emit(std::move(node_ir), out);
      return;
    }
    if (op == "GlobalAveragePool") {
      emit({.op = IrOp::kGlobalAveragePool, .onnx_op = op, .inputs = inputs()}, out);
      return;
    }
    if (op == "AveragePool" || op == "MaxPool") {
      IrNode node_ir{.op = op == "MaxPool" ? IrOp::kMaxPool : IrOp::kAveragePool, .onnx_op = op, .inputs = inputs()};
      const auto kernel = attr_ints(n, "kernel_shape", {});
      const auto strides = attr_ints(n, "strides", {1, 1});
      const auto pads = attr_ints(n, "pads", {0, 0, 0, 0});
      if (kernel.size() != 2 || pads[2] != pads[0] || pads[3] != pads[1]) fail(out, "a symmetric 2-D pool");
      node_ir.kernel_h = int(kernel[0]);
      node_ir.kernel_w = int(kernel[1]);
      node_ir.stride_h = int(strides[0]);
      node_ir.stride_w = int(strides[1]);
      node_ir.pad_h = int(pads[0]);
      node_ir.pad_w = int(pads[1]);
      node_ir.ceil_mode = attr_int(n, "ceil_mode", 0) != 0;
      node_ir.count_include_pad = attr_int(n, "count_include_pad", 0) != 0;
      emit(std::move(node_ir), out);
      return;
    }
    if (op == "Resize") {
      const OnnxAttribute* mode = n.attribute("mode");
      const OnnxAttribute* transform = n.attribute("coordinate_transformation_mode");
      const OnnxAttribute* nearest = n.attribute("nearest_mode");
      if (!mode || mode->s != "nearest" || !transform || transform->s != "asymmetric" || !nearest ||
          nearest->s != "floor") {
        fail(out, "a nearest, asymmetric, floor resize");
      }
      emit({.op = IrOp::kResize, .onnx_op = op, .inputs = inputs()}, out);
      return;
    }
    if (op == "Transpose") {
      emit({.op = IrOp::kTranspose, .onnx_op = op, .inputs = inputs(), .ints = attr_ints(n, "perm", {})}, out);
      return;
    }
    if (op == "Softmax") {
      emit({.op = IrOp::kSoftmax, .onnx_op = op, .inputs = inputs(), .axis = int(attr_int(n, "axis", -1))}, out);
      return;
    }
    if (op == "ReduceMean") {
      IrNode node_ir{.op = IrOp::kReduceMean, .onnx_op = op, .inputs = inputs(), .ints = attr_ints(n, "axes", {})};
      if (attr_int(n, "keepdims", 1) != 1) fail(out, "a keepdims ReduceMean");
      emit(std::move(node_ir), out);
      return;
    }
    fail(std::format("node {} ({})", out, op), "an op the graph lowering knows");
  }

  // Drops nodes no output depends on (the shape subgraphs the fusions left
  // behind).
  void prune() {
    std::set<int> needed(ir_.outputs.begin(), ir_.outputs.end());
    std::vector<IrNode> kept;
    for (auto it = ir_.nodes.rbegin(); it != ir_.nodes.rend(); ++it) {
      if (!needed.contains(it->output)) continue;
      for (int input : it->inputs) {
        if (input >= 0) needed.insert(input);
      }
      kept.push_back(std::move(*it));
    }
    std::reverse(kept.begin(), kept.end());
    ir_.nodes = std::move(kept);
  }

  OnnxGraph g_;
  std::string what_;
  std::map<std::string, std::vector<size_t>> consumers_;
  std::map<std::string, int> ids_;
  std::set<size_t> done_;
  IrGraph ir_;
};

}  // namespace

IrGraph lower_onnx(const std::filesystem::path& model) {
  IrGraph graph = Lowering(read_onnx_graph(model), model.string()).run();
  fuse_merges(&graph);
  return graph;
}

}  // namespace grparse::levelzero::graph
