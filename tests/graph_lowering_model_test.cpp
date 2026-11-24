// The graph lowering on the real models the Level Zero runtime runs (the
// three OCR nets and the table encoder): what the fusions leave is pinned,
// so a change to graph_ir.cpp or graph_fuse.cpp that still lowers but stops
// fusing, or fuses something new, fails here on a host with no GPU.  Per
// model: the operations by kind, the fused activations by kind, the weight
// block's size, and that every operation reads only what the input, a
// constant or an earlier operation provides.
//
// A deliberate lowering change re-pins the summaries below from this test's
// own failure message, which prints the new one.  Skips (77) without the
// model files.
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "levelzero/graph_ir.h"
#include "support/check.h"

namespace {

using grparse::levelzero::graph::Activation;
using grparse::levelzero::graph::IrGraph;
using grparse::levelzero::graph::IrNode;
using grparse::levelzero::graph::IrOp;
using grparse::levelzero::graph::lower_onnx;
using grparse_test::require;
using grparse_test::require_equal;

const char* op_name(IrOp op) {
  switch (op) {
    case IrOp::kConv: return "Conv";
    case IrOp::kConvTranspose: return "ConvTranspose";
    case IrOp::kMatMulConst: return "MatMulConst";
    case IrOp::kMatMul: return "MatMul";
    case IrOp::kLayerNorm: return "LayerNorm";
    case IrOp::kBinary: return "Binary";
    case IrOp::kUnary: return "Unary";
    case IrOp::kGlobalAveragePool: return "GlobalAveragePool";
    case IrOp::kAveragePool: return "AveragePool";
    case IrOp::kMaxPool: return "MaxPool";
    case IrOp::kResize: return "Resize";
    case IrOp::kScaleMerge: return "ScaleMerge";
    case IrOp::kConcatScaled: return "ConcatScaled";
    case IrOp::kTranspose: return "Transpose";
    case IrOp::kSlice: return "Slice";
    case IrOp::kConcat: return "Concat";
    case IrOp::kSoftmax: return "Softmax";
    case IrOp::kReduceMean: return "ReduceMean";
    case IrOp::kView: return "View";
    case IrOp::kHost: return "Host";
  }
  return "?";
}

const char* activation_name(Activation activation) {
  switch (activation) {
    case Activation::kNone: return "none";
    case Activation::kHardSwish: return "hard-swish";
    case Activation::kRelu: return "relu";
    case Activation::kHardSigmoid: return "hard-sigmoid";
    case Activation::kSigmoid: return "sigmoid";
    case Activation::kSwish: return "swish";
    case Activation::kHardSwishOp: return "hard-swish-op";
  }
  return "?";
}

std::string counted(const std::map<std::string, int>& counts) {
  std::string out;
  for (const auto& [name, count] : counts) out += std::format("{}{}={}", out.empty() ? "" : " ", name, count);
  return out;
}

// "operations | activations of the weighted operations | depthwise | weights".
std::string summary(const IrGraph& graph) {
  std::map<std::string, int> operations, activations;
  int depthwise = 0;
  for (const IrNode& node : graph.nodes) {
    ++operations[op_name(node.op)];
    if (node.op == IrOp::kConv || node.op == IrOp::kConvTranspose || node.op == IrOp::kMatMulConst) {
      ++activations[activation_name(node.activation)];
    }
    if (node.op == IrOp::kConv && node.groups > 1) ++depthwise;
  }
  return std::format("{} | {} | depthwise={} | weights={}", counted(operations), counted(activations), depthwise,
                     graph.weights.size());
}

void require_in_order(const IrGraph& graph, const std::string& model) {
  std::set<int> available{graph.input};
  for (size_t id = 0; id < graph.values.size(); ++id) {
    if (graph.values[id].constant) available.insert(int(id));
  }
  for (const IrNode& node : graph.nodes) {
    for (const int input : node.inputs) {
      require(input < 0 || available.contains(input),
              std::format("{}: {} ({}) reads {} before anything produces it", model, node.name, op_name(node.op),
                          input < 0 ? "?" : graph.values[size_t(input)].name));
    }
    require(!available.contains(node.output) || node.output == graph.input,
            std::format("{}: {} is produced twice", model, node.name));
    available.insert(node.output);
  }
  for (const int output : graph.outputs) {
    require(available.contains(output), std::format("{}: a graph output is never produced", model));
  }
}

struct Pinned {
  const char* file;
  size_t outputs;
  const char* summary;
};

// One line a model; see the header comment for how to re-pin.
const Pinned kModels[] = {
    {"ch_PP-OCRv3_det_infer.onnx", 1,
     "Binary=10 ConcatScaled=1 Conv=72 ConvTranspose=2 GlobalAveragePool=8 ScaleMerge=4 | "
     "hard-sigmoid=8 hard-swish=20 none=23 relu=22 sigmoid=1 | "
     "depthwise=15 | "
     "weights=575697"},
    {"ch_ppocr_mobile_v2.0_cls_infer.onnx", 1,
     "Binary=16 Conv=53 GlobalAveragePool=10 Host=7 MatMulConst=1 MaxPool=1 Softmax=1 View=1 | "
     "hard-sigmoid=9 hard-swish=18 none=12 relu=15 | "
     "depthwise=11 | "
     "weights=128754"},
    {"ch_PP-OCRv3_rec_infer.onnx", 1,
     "AveragePool=1 Binary=10 Conv=36 GlobalAveragePool=2 Host=25 LayerNorm=5 MatMul=4 MatMulConst=9 Softmax=3 "
     "Transpose=9 View=9 | "
     "hard-sigmoid=2 hard-swish=27 none=7 relu=2 swish=7 | "
     "depthwise=13 | "
     "weights=2635809"},
    {"slanet_plus_encoder.onnx", 1,
     "Binary=2 Conv=77 GlobalAveragePool=2 Host=64 Resize=3 Transpose=1 | "
     "hard-sigmoid=2 hard-swish-op=73 relu=2 | "
     "depthwise=22 | "
     "weights=1341408"},
};

}  // namespace

int main() {
  const char* models_env = std::getenv("GRPARSE_TEST_MODELS_DIR");
  const std::filesystem::path models = models_env == nullptr ? "models" : models_env;
  for (const Pinned& pinned : kModels) {
    if (!std::filesystem::is_regular_file(models / pinned.file)) {
      std::println(stderr, "graph-lowering-model-test: {} is absent from {}", pinned.file, models.string());
      return grparse_test::missing_model_exit_code();
    }
  }
  return grparse_test::run_test_main("graph-lowering-model-test", "lowered models ok", {[&] {
    for (const Pinned& pinned : kModels) {
      const IrGraph graph = lower_onnx(models / pinned.file);
      require_in_order(graph, pinned.file);
      require_equal(graph.outputs.size(), pinned.outputs, std::format("{}: graph outputs", pinned.file));
      require_equal(summary(graph), pinned.summary, std::format("{}: what the lowering produces", pinned.file));
    }
  }});
}
