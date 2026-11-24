#pragma once

// The SLANet+ encoder (slanet_plus_encoder.onnx: PP-LCNet backbone and
// CSP-PAN neck) lowered from its ONNX graph to the steps the Level Zero
// kernels in table_encoder.cl run: every convolution with its batch norm
// folded in and its activation fused, the shape-computing subgraphs folded to
// constants, and each tensor a buffer of known shape.  An op or a pattern the
// lowering does not know fails it, naming the node; nothing is skipped.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace grparse::levelzero {

enum class Activation { kNone = 0, kHardSwish = 1, kRelu = 2, kHardSigmoid = 3 };

// One activation tensor of one image, channel-major ([C][H][W]).
struct PlanBuffer {
  int channels = 0;
  int height = 0;
  int width = 0;
  size_t offset = 0;  // floats from the start of an image's activation block
  size_t size() const { return size_t(channels) * height * width; }
};

enum class StepKind {
  kConv,        // groups 1, any kernel and stride
  kDepthwise,   // groups == channels
  kPointwise,   // 1x1, stride 1, groups 1
  kGlobalPool,  // mean over H x W
  kScale,       // in[c][h][w] * other[c]
  kResize,      // nearest, asymmetric, floor
  kCopy,        // a concat input into its channel range of the output
  kTranspose,   // [C][H*W] -> [H*W][C], the encoder's output layout
};

struct PlanStep {
  StepKind kind = StepKind::kConv;
  int input = -1;
  int other = -1;   // kScale's per-channel factors
  int output = -1;
  int channel_offset = 0;  // kCopy: first output channel written
  int kernel = 1;
  int stride = 1;
  int pad = 0;
  size_t weights = 0;  // floats into the packed weights: [Co][Ci/g][k][k], BN folded
  size_t bias = 0;     // floats into the packed weights: [Co]
  Activation activation = Activation::kNone;
  float alpha = 0.0F;  // HardSigmoid
  float beta = 0.0F;
  std::string node = {};  // the ONNX node it came from, for errors and logs
};

struct EncoderPlan {
  std::vector<PlanBuffer> buffers;
  std::vector<PlanStep> steps;
  std::vector<float> weights;  // every step's weights and biases, 16-float aligned
  int input = -1;              // [3][488][488]
  int output = -1;             // [256][96] after the transpose
  size_t activation_floats = 0;  // one image's buffers, end to end
};

// Lowers the encoder for a [1, 3, side, side] input.  Throws naming the node
// on anything it does not lower.
EncoderPlan plan_encoder(const std::filesystem::path& encoder_model, int side);

}  // namespace grparse::levelzero
