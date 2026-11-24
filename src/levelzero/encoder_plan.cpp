#include "levelzero/encoder_plan.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "levelzero/onnx_initializers.h"

namespace grparse::levelzero {
namespace {

// What a graph value lowered to: an activation buffer, or a small integer
// vector the shape subgraphs compute (folded here, never run).
struct Value {
  int buffer = -1;
  bool folded = false;
  std::vector<int64_t> ints;
  int reshape_channels = 0;  // a buffer viewed as [N][C][H*W] by the final Reshape
};

class Lowering {
 public:
  Lowering(const OnnxGraph& graph, std::string what) : graph_(graph), what_(std::move(what)) {
    for (const OnnxNode& node : graph_.nodes) {
      for (const std::string& input : node.inputs) ++uses_[input];
    }
    for (const std::string& output : graph_.outputs) ++uses_[output];
  }

  EncoderPlan run(int side) {
    if (graph_.inputs.size() != 1 || graph_.outputs.size() != 1) fail("the graph", "one input and one output");
    plan_.input = add_buffer(3, side, side);
    values_[graph_.inputs[0]].buffer = plan_.input;
    for (size_t index = 0; index < graph_.nodes.size(); ++index) {
      if (!consumed_.contains(index)) lower(index);
    }
    const Value& out = value(graph_.outputs[0], "the graph output");
    if (out.buffer < 0 || plan_.steps.empty() || plan_.steps.back().kind != StepKind::kTranspose) {
      fail("the graph output", "the transposed [positions][channels] features");
    }
    plan_.output = out.buffer;
    return std::move(plan_);
  }

 private:
  [[noreturn]] void fail(const std::string& where, const std::string& expected) const {
    throw std::runtime_error(std::format("{}: {} is not {}", what_, where, expected));
  }

  int add_buffer(int channels, int height, int width) {
    PlanBuffer buffer{channels, height, width, plan_.activation_floats};
    plan_.activation_floats += (buffer.size() + 15) / 16 * 16;
    plan_.buffers.push_back(buffer);
    return static_cast<int>(plan_.buffers.size() - 1);
  }

  size_t add_weights(const std::vector<double>& values) {
    plan_.weights.resize((plan_.weights.size() + 15) / 16 * 16, 0.0F);
    const size_t at = plan_.weights.size();
    for (const double v : values) plan_.weights.push_back(static_cast<float>(v));
    return at;
  }

  const Value& value(const std::string& name, const std::string& node) const {
    const auto found = values_.find(name);
    if (found == values_.end()) fail(node, "an input the lowering has seen (" + name + ")");
    return found->second;
  }

  const PlanBuffer& buffer_of(const std::string& name, const std::string& node) const {
    const Value& v = value(name, node);
    if (v.buffer < 0) fail(node, "fed an activation (" + name + ")");
    return plan_.buffers[size_t(v.buffer)];
  }

  const OnnxTensor& constant(const std::string& name, const std::string& node) const {
    const auto found = graph_.constants.find(name);
    if (found == graph_.constants.end()) fail(node, "given a constant " + name);
    return found->second;
  }

  // The one node that reads `name`, when exactly one does and it is `op`.
  std::optional<size_t> sole_consumer(const std::string& name, const char* op) const {
    if (uses_.contains(name) && uses_.at(name) != 1) return std::nullopt;
    for (size_t index = 0; index < graph_.nodes.size(); ++index) {
      const OnnxNode& node = graph_.nodes[index];
      if (std::ranges::find(node.inputs, name) == node.inputs.end()) continue;
      if (node.op_type == op) return index;
      return std::nullopt;
    }
    return std::nullopt;
  }

  std::vector<int64_t> folded(const std::string& name, const std::string& node) const {
    const auto c = graph_.constants.find(name);
    if (c != graph_.constants.end()) return c->second.ints;
    const Value& v = value(name, node);
    if (!v.folded) fail(node, "fed a shape value (" + name + ")");
    return v.ints;
  }

  void lower(size_t index) {
    const OnnxNode& node = graph_.nodes[index];
    const std::string where = std::format("node {} ({})", node.outputs.empty() ? "?" : node.outputs[0], node.op_type);
    const std::string& op = node.op_type;
    if (op == "Constant") return;
    if (op == "Conv") return lower_conv(index, where);
    if (op == "Shape") {
      const PlanBuffer& b = buffer_of(node.inputs[0], where);
      fold(node, {1, b.channels, b.height, b.width});
    } else if (op == "Cast" || op == "Squeeze" || (op == "Reshape" && is_folded(node.inputs[0]))) {
      fold(node, folded(node.inputs[0], where));
    } else if (op == "Slice") {
      lower_slice(node, where);
    } else if (op == "Concat" && is_folded(node.inputs[0])) {
      std::vector<int64_t> all;
      for (const std::string& input : node.inputs) {
        const auto part = folded(input, where);
        all.insert(all.end(), part.begin(), part.end());
      }
      fold(node, all);
    } else if (op == "Concat") {
      lower_concat(node, where);
    } else if (op == "GlobalAveragePool") {
      const PlanBuffer in = buffer_of(node.inputs[0], where);
      emit(node, {.kind = StepKind::kGlobalPool, .input = values_[node.inputs[0]].buffer}, in.channels, 1, 1);
    } else if (op == "Mul") {
      const PlanBuffer in = buffer_of(node.inputs[0], where);
      const PlanBuffer factor = buffer_of(node.inputs[1], where);
      if (factor.channels != in.channels || factor.height != 1 || factor.width != 1) {
        fail(where, "a per-channel scale");
      }
      emit(node, {.kind = StepKind::kScale, .input = values_[node.inputs[0]].buffer,
                  .other = values_[node.inputs[1]].buffer}, in.channels, in.height, in.width);
    } else if (op == "Resize") {
      lower_resize(node, where);
    } else if (op == "Reshape") {
      const PlanBuffer in = buffer_of(node.inputs[0], where);
      const auto shape = folded(node.inputs[1], where);
      if (shape.size() != 3 || shape[1] != in.channels) fail(where, "a [N][C][H*W] view");
      Value& out = values_[node.outputs[0]];
      out.buffer = values_[node.inputs[0]].buffer;
      out.reshape_channels = in.channels;
    } else if (op == "Transpose") {
      const Value& in = value(node.inputs[0], where);
      const OnnxAttribute* perm = node.attribute("perm");
      if (in.reshape_channels == 0 || perm == nullptr || perm->ints != std::vector<int64_t>{0, 2, 1}) {
        fail(where, "the [N][C][H*W] -> [N][H*W][C] transpose");
      }
      const PlanBuffer b = plan_.buffers[size_t(in.buffer)];
      emit(node, {.kind = StepKind::kTranspose, .input = in.buffer}, b.height * b.width, b.channels, 1);
    } else {
      fail(where, "an op the encoder lowering knows");
    }
  }

  bool is_folded(const std::string& name) const {
    if (graph_.constants.contains(name)) return !graph_.constants.at(name).ints.empty();
    const auto found = values_.find(name);
    return found != values_.end() && found->second.folded;
  }

  void fold(const OnnxNode& node, std::vector<int64_t> ints) {
    Value& out = values_[node.outputs[0]];
    out.folded = true;
    out.ints = std::move(ints);
  }

  void emit(const OnnxNode& node, PlanStep step, int channels, int height, int width,
            const std::string* output_name = nullptr) {
    step.output = add_buffer(channels, height, width);
    step.node = node.outputs[0];
    plan_.steps.push_back(std::move(step));
    values_[output_name ? *output_name : node.outputs[0]].buffer = plan_.steps.back().output;
  }

  void lower_slice(const OnnxNode& node, const std::string& where) {
    const auto data = folded(node.inputs[0], where);
    const auto starts = folded(node.inputs[1], where);
    const auto ends = folded(node.inputs[2], where);
    if (node.inputs.size() > 3 && folded(node.inputs[3], where) != std::vector<int64_t>{0}) {
      fail(where, "a slice of axis 0");
    }
    const int64_t step = node.inputs.size() > 4 ? folded(node.inputs[4], where).at(0) : 1;
    if (starts.size() != 1 || ends.size() != 1 || step != 1) fail(where, "a unit-step 1-D slice");
    const auto n = static_cast<int64_t>(data.size());
    const auto clamp = [n](int64_t at) { return std::clamp(at < 0 ? at + n : at, int64_t{0}, n); };
    const int64_t begin = clamp(starts[0]);
    const int64_t end = clamp(ends[0]);
    fold(node, std::vector<int64_t>(data.begin() + begin, data.begin() + std::max(begin, end)));
  }

  void lower_resize(const OnnxNode& node, const std::string& where) {
    const OnnxAttribute* mode = node.attribute("mode");
    const OnnxAttribute* transform = node.attribute("coordinate_transformation_mode");
    const OnnxAttribute* nearest = node.attribute("nearest_mode");
    if (!mode || mode->s != "nearest" || !transform || transform->s != "asymmetric" || !nearest ||
        nearest->s != "floor" || node.inputs.size() != 4) {
      fail(where, "a nearest, asymmetric, floor resize to sizes");
    }
    const PlanBuffer in = buffer_of(node.inputs[0], where);
    const auto sizes = folded(node.inputs[3], where);
    if (sizes.size() != 4 || sizes[1] != in.channels) fail(where, "a resize of H and W only");
    emit(node, {.kind = StepKind::kResize, .input = values_[node.inputs[0]].buffer}, in.channels,
         int(sizes[2]), int(sizes[3]));
  }

  void lower_concat(const OnnxNode& node, const std::string& where) {
    const OnnxAttribute* axis = node.attribute("axis");
    if (axis == nullptr || axis->i != 1) fail(where, "a channel concat");
    int channels = 0;
    const PlanBuffer first = buffer_of(node.inputs[0], where);
    for (const std::string& input : node.inputs) {
      const PlanBuffer b = buffer_of(input, where);
      if (b.height != first.height || b.width != first.width) fail(where, "a concat of equal H and W");
      channels += b.channels;
    }
    const int out = add_buffer(channels, first.height, first.width);
    int offset = 0;
    for (const std::string& input : node.inputs) {
      plan_.steps.push_back({.kind = StepKind::kCopy, .input = values_[input].buffer, .output = out,
                             .channel_offset = offset, .node = node.outputs[0]});
      offset += buffer_of(input, where).channels;
    }
    values_[node.outputs[0]].buffer = out;
  }

  void lower_conv(size_t index, const std::string& where) {
    const OnnxNode& node = graph_.nodes[index];
    const PlanBuffer in = buffer_of(node.inputs[0], where);
    const OnnxTensor& w = constant(node.inputs[1], where);
    const auto ints = [&](const char* name, std::vector<int64_t> fallback) {
      const OnnxAttribute* a = node.attribute(name);
      return a == nullptr ? fallback : a->ints;
    };
    const auto kernel = ints("kernel_shape", {});
    const auto strides = ints("strides", {1, 1});
    const auto pads = ints("pads", {0, 0, 0, 0});
    const auto dilations = ints("dilations", {1, 1});
    const OnnxAttribute* group_attribute = node.attribute("group");
    const int64_t groups = group_attribute ? group_attribute->i : 1;
    if (w.dims.size() != 4 || kernel.size() != 2 || kernel[0] != kernel[1] || strides[0] != strides[1] ||
        std::ranges::any_of(pads, [&](int64_t p) { return p != pads[0]; }) || dilations != std::vector<int64_t>{1, 1}) {
      fail(where, "a square, evenly padded, undilated 2-D convolution");
    }
    const int out_channels = int(w.dims[0]);
    const int k = int(kernel[0]), s = int(strides[0]), p = int(pads[0]);
    std::vector<double> weights(w.values.begin(), w.values.end());
    std::vector<double> bias(size_t(out_channels), 0.0);
    if (node.inputs.size() > 2) {
      const OnnxTensor& b = constant(node.inputs[2], where);
      std::copy(b.values.begin(), b.values.end(), bias.begin());
    }
    std::string output = node.outputs[0];
    PlanStep step{.kernel = k, .stride = s, .pad = p};
    if (const auto bn = sole_consumer(output, "BatchNormalization")) {
      const OnnxNode& norm = graph_.nodes[*bn];
      const auto& gamma = constant(norm.inputs[1], where).values;
      const auto& beta = constant(norm.inputs[2], where).values;
      const auto& mean = constant(norm.inputs[3], where).values;
      const auto& var = constant(norm.inputs[4], where).values;
      const OnnxAttribute* eps_attribute = norm.attribute("epsilon");
      const double eps = eps_attribute ? eps_attribute->f : 1e-5;
      const size_t per_output = weights.size() / size_t(out_channels);
      for (size_t co = 0; co < size_t(out_channels); ++co) {
        const double scale = gamma[co] / std::sqrt(double(var[co]) + eps);
        for (size_t at = 0; at < per_output; ++at) weights[co * per_output + at] *= scale;
        bias[co] = (bias[co] - mean[co]) * scale + beta[co];
      }
      consumed_.insert(*bn);
      output = norm.outputs[0];
    }
    for (const char* act : {"HardSwish", "Relu", "HardSigmoid"}) {
      const auto consumer = sole_consumer(output, act);
      if (!consumer) continue;
      const OnnxNode& a = graph_.nodes[*consumer];
      step.activation = std::string(act) == "HardSwish" ? Activation::kHardSwish
                        : std::string(act) == "Relu"    ? Activation::kRelu
                                                        : Activation::kHardSigmoid;
      if (step.activation == Activation::kHardSigmoid) {
        step.alpha = a.attribute("alpha") ? a.attribute("alpha")->f : 0.2F;
        step.beta = a.attribute("beta") ? a.attribute("beta")->f : 0.5F;
      }
      consumed_.insert(*consumer);
      output = a.outputs[0];
      break;
    }
    if (groups == in.channels && groups == out_channels && w.dims[1] == 1) {
      step.kind = StepKind::kDepthwise;
    } else if (groups == 1 && k == 1 && s == 1 && p == 0 && w.dims[1] == in.channels) {
      step.kind = StepKind::kPointwise;
    } else if (groups == 1 && w.dims[1] == in.channels) {
      step.kind = StepKind::kConv;
    } else {
      fail(where, "a dense, depthwise or pointwise convolution");
    }
    step.input = values_[node.inputs[0]].buffer;
    step.weights = add_weights(weights);
    step.bias = add_weights(bias);
    const int height = (in.height + 2 * p - k) / s + 1;
    const int width = (in.width + 2 * p - k) / s + 1;
    emit(node, std::move(step), out_channels, height, width, &output);
    plan_.steps.back().node = output;
  }

  const OnnxGraph& graph_;
  std::string what_;
  std::map<std::string, int> uses_;
  std::map<std::string, Value> values_;
  std::set<size_t> consumed_;
  EncoderPlan plan_;
};

}  // namespace

EncoderPlan plan_encoder(const std::filesystem::path& encoder_model, int side) {
  const OnnxGraph graph = read_onnx_graph(encoder_model);
  return Lowering(graph, encoder_model.string()).run(side);
}

}  // namespace grparse::levelzero
