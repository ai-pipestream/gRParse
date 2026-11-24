#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "levelzero/graph_ir.h"

namespace grparse::levelzero::graph {
namespace {

// The rewrites of fuse_merges over one graph.  A value's readers are the
// nodes that take it as an input; a graph output counts as one more reader,
// so a value the caller receives is never fused away.
class Fuser {
 public:
  explicit Fuser(IrGraph* graph) : g_(*graph) {}

  void run() {
    index();
    for (size_t at = 0; at < g_.nodes.size(); ++at) excite(at);
    for (size_t at = 0; at < g_.nodes.size(); ++at) upsampled_add(at);
    for (size_t at = 0; at < g_.nodes.size(); ++at) concat(at);
    std::vector<IrNode> kept;
    for (size_t at = 0; at < g_.nodes.size(); ++at) {
      if (!dropped_.contains(at)) kept.push_back(std::move(g_.nodes[at]));
    }
    g_.nodes = std::move(kept);
  }

 private:
  void index() {
    for (size_t at = 0; at < g_.nodes.size(); ++at) {
      producer_[g_.nodes[at].output] = at;
      for (int input : g_.nodes[at].inputs) {
        if (input >= 0) ++readers_[input];
      }
    }
    for (int output : g_.outputs) ++readers_[output];
  }

  IrNode* producer(int value) {
    const auto found = producer_.find(value);
    return found == producer_.end() || dropped_.contains(found->second) ? nullptr : &g_.nodes[found->second];
  }

  // The node at `at` when it alone reads `value`.
  bool sole_reader(int value) const {
    const auto found = readers_.find(value);
    return found != readers_.end() && found->second == 1;
  }

  void drop(const IrNode* node) { dropped_.insert(size_t(node - g_.nodes.data())); }

  // Whether `scale` is a squeeze-excite gate of `x`: convolutions over the
  // global average pool of x, so it is [n, c, 1, 1] whatever x's size.
  bool gate_of(int scale, int x) {
    const IrNode* node = producer(scale);
    for (int depth = 0; depth < 3 && node != nullptr && node->op == IrOp::kConv; ++depth) {
      node = producer(node->inputs[0]);
    }
    return node != nullptr && node->op == IrOp::kGlobalAveragePool && node->inputs[0] == x;
  }

  // Add(x, Mul(x, gate)) -> x * (1 + gate).
  void excite(size_t at) {
    IrNode& add = g_.nodes[at];
    if (add.op != IrOp::kBinary || add.binary != BinaryOp::kAdd || add.inputs.size() != 2) return;
    for (int side = 0; side < 2; ++side) {
      const int x = add.inputs[size_t(side)];
      IrNode* mul = producer(add.inputs[size_t(1 - side)]);
      if (mul == nullptr || mul->op != IrOp::kBinary || mul->binary != BinaryOp::kMul || !sole_reader(mul->output)) continue;
      const int gate = mul->inputs[0] == x ? mul->inputs[1] : mul->inputs[1] == x ? mul->inputs[0] : -1;
      if (gate < 0 || !gate_of(gate, x)) continue;
      drop(mul);
      add.op = IrOp::kScaleMerge;
      add.onnx_op = "ScaleMerge";
      add.inputs = {x, gate, -1};
      add.ints = {1, 1};
      return;
    }
  }

  // The whole-number factor of a Resize by constant scales [1, 1, f, f].
  std::optional<int64_t> factor(const IrNode& resize) const {
    if (resize.op != IrOp::kResize || resize.inputs.size() < 3 || resize.inputs[2] < 0) return std::nullopt;
    if (resize.inputs.size() > 3 && resize.inputs[3] >= 0) return std::nullopt;
    const IrValue& scales = g_.values[size_t(resize.inputs[2])];
    if (!scales.constant || scales.host || scales.shape != std::vector<int64_t>{4}) return std::nullopt;
    const float* f = g_.weights.data() + scales.weights;
    if (f[0] != 1.0F || f[1] != 1.0F || f[2] != f[3] || f[2] < 1.0F || f[2] != std::floor(f[2])) return std::nullopt;
    return int64_t(f[2]);
  }

  // Add(a, Resize(y)) -> a + nearest(y), and a's own excite with it.
  void upsampled_add(size_t at) {
    IrNode& add = g_.nodes[at];
    if (add.op != IrOp::kBinary || add.binary != BinaryOp::kAdd || add.inputs.size() != 2) return;
    for (int side = 0; side < 2; ++side) {
      IrNode* resize = producer(add.inputs[size_t(side)]);
      if (resize == nullptr || !sole_reader(resize->output)) continue;
      const auto f = factor(*resize);
      if (!f) continue;
      int x = add.inputs[size_t(1 - side)], gate = -1;
      IrNode* base = producer(x);
      if (base != nullptr && base->op == IrOp::kScaleMerge && base->inputs[2] < 0 && base->ints[0] == 1 &&
          sole_reader(base->output)) {
        x = base->inputs[0];
        gate = base->inputs[1];
        drop(base);
      }
      const int y = resize->inputs[0];
      drop(resize);
      add.op = IrOp::kScaleMerge;
      add.onnx_op = "ScaleMerge";
      add.inputs = {x, gate, y};
      add.ints = {1, *f};
      return;
    }
  }

  // Concat over channels of resized and excited parts -> each part written
  // into its channels once.
  void concat(size_t at) {
    IrNode& node = g_.nodes[at];
    if (node.op != IrOp::kHost || node.onnx_op != "Concat" || node.axis != 1) return;
    std::vector<int> inputs;
    std::vector<int64_t> factors;
    std::vector<IrNode*> absorbed;
    for (int part : node.inputs) {
      int x = part, gate = -1;
      int64_t f = 1;
      IrNode* from = part >= 0 && sole_reader(part) ? producer(part) : nullptr;
      if (from != nullptr && factor(*from)) {
        f = *factor(*from);
        absorbed.push_back(from);
        x = from->inputs[0];
        from = sole_reader(x) ? producer(x) : nullptr;
      }
      if (from != nullptr && from->op == IrOp::kScaleMerge && from->inputs[2] < 0 && from->ints[0] == 1) {
        absorbed.push_back(from);
        x = from->inputs[0];
        gate = from->inputs[1];
      }
      inputs.insert(inputs.end(), {x, gate});
      factors.push_back(f);
    }
    if (absorbed.empty()) return;
    for (IrNode* gone : absorbed) drop(gone);
    node.op = IrOp::kConcatScaled;
    node.onnx_op = "ConcatScaled";
    node.inputs = std::move(inputs);
    node.ints = std::move(factors);
  }

  IrGraph& g_;
  std::map<int, size_t> producer_;
  std::map<int, int> readers_;
  std::set<size_t> dropped_;
};

}  // namespace

void fuse_merges(IrGraph* graph) { Fuser(graph).run(); }

}  // namespace grparse::levelzero::graph
