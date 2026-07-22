#include "grparse/layout_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"

namespace grparse {
namespace {

// Model geometry and thresholds mirror RapidLayout's pp_layout_publaynet
// handler, which is the reference this implementation is golden-tested
// against.  The export is a raw PicoDet head: four pyramid levels of class
// scores plus DFL box distributions that must be decoded here.
constexpr int kInputHeight = 800;
constexpr int kInputWidth = 608;
constexpr std::array<int, 4> kStrides = {8, 16, 32, 64};
constexpr int kRegBins = 8;  // DFL bins per box side
constexpr float kConfidenceThreshold = 0.5F;
constexpr float kNmsIou = 0.5F;
constexpr int kNmsCandidates = 200;
constexpr int kKeepTopK = 100;

// Reference preprocessing applies these to the image's channel order as
// loaded (BGR); replicated verbatim so goldens match.
constexpr std::array<float, 3> kMean = {0.485F, 0.456F, 0.406F};
constexpr std::array<float, 3> kStd = {0.229F, 0.224F, 0.225F};

struct Candidate {
  float score = 0.0F;
  int label = 0;
  // Box in model input space (608x800), clipped there before rescaling.
  float left = 0.0F, top = 0.0F, right = 0.0F, bottom = 0.0F;
};

float boxes_iou(const Candidate& a, const Candidate& b) {
  const float left = std::max(a.left, b.left);
  const float top = std::max(a.top, b.top);
  const float right = std::min(a.right, b.right);
  const float bottom = std::min(a.bottom, b.bottom);
  const float intersection = std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
  const float area_a = std::max(0.0F, a.right - a.left) * std::max(0.0F, a.bottom - a.top);
  const float area_b = std::max(0.0F, b.right - b.left) * std::max(0.0F, b.bottom - b.top);
  return intersection / (area_a + area_b - intersection + 1e-5F);
}

// Reference hard_nms: per class, consider the highest-scoring candidates and
// greedily keep boxes that overlap kept ones by at most kNmsIou.
void append_class_nms(std::vector<Candidate>& picked, std::vector<Candidate> candidates) {
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  if (candidates.size() > static_cast<size_t>(kNmsCandidates)) candidates.resize(kNmsCandidates);
  size_t kept = 0;
  for (size_t index = 0; index < candidates.size() && kept < static_cast<size_t>(kKeepTopK);
       ++index) {
    bool suppressed = false;
    for (size_t earlier = picked.size() - kept; earlier < picked.size(); ++earlier) {
      if (boxes_iou(candidates[index], picked[earlier]) > kNmsIou) {
        suppressed = true;
        break;
      }
    }
    if (!suppressed) {
      picked.push_back(candidates[index]);
      ++kept;
    }
  }
}

}  // namespace

const std::vector<std::string>& LayoutEngine::labels() {
  static const std::vector<std::string> kLabels = {"text", "title", "list", "table", "figure"};
  return kLabels;
}

class LayoutEngine::Impl {
 public:
  explicit Impl(const std::filesystem::path& model_path)
      : env_(ORT_LOGGING_LEVEL_ERROR, "grparse-layout") {
    if (!std::filesystem::exists(model_path)) {
      throw std::runtime_error("Layout model is missing: " + model_path.string());
    }
    options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    // Same provider decision point as the OCR sessions.
    append_execution_provider(options_, -1);
    session_ = Ort::Session(env_, model_path.c_str(), options_);

    Ort::AllocatorWithDefaultOptions allocator;
    input_name_ = session_.GetInputNameAllocated(0, allocator).get();
    const size_t output_count = session_.GetOutputCount();
    if (output_count != kStrides.size() * 2) {
      throw std::runtime_error("Layout model has an unexpected output count");
    }
    for (size_t index = 0; index < output_count; ++index) {
      output_names_.push_back(session_.GetOutputNameAllocated(index, allocator).get());
    }
    for (const auto& name : output_names_) output_name_pointers_.push_back(name.c_str());
  }

  std::vector<LayoutRegion> detect_regions(const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3) {
      throw std::invalid_argument("Layout detection expects a non-empty BGR image");
    }

    // Preprocess: plain resize (no aspect preservation, per reference),
    // scale to [0,1], per-channel normalize, HWC -> CHW.
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(kInputWidth, kInputHeight));
    std::vector<float> tensor(static_cast<size_t>(3) * kInputHeight * kInputWidth);
    const size_t plane = static_cast<size_t>(kInputHeight) * kInputWidth;
    for (int row = 0; row < kInputHeight; ++row) {
      const cv::Vec3b* pixels = resized.ptr<cv::Vec3b>(row);
      for (int column = 0; column < kInputWidth; ++column) {
        const size_t offset = static_cast<size_t>(row) * kInputWidth + column;
        for (int channel = 0; channel < 3; ++channel) {
          tensor[plane * static_cast<size_t>(channel) + offset] =
              (static_cast<float>(pixels[column][channel]) / 255.0F - kMean[channel]) /
              kStd[channel];
        }
      }
    }

    const std::array<int64_t, 4> input_shape = {1, 3, kInputHeight, kInputWidth};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                       input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name_.c_str()};
    auto outputs = session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                                output_name_pointers_.data(), output_name_pointers_.size());

    // Decode each pyramid level: outputs [0..3] are class scores [1, N, 5],
    // outputs [4..7] the matching DFL box distributions [1, N, 4 * kRegBins].
    const int class_count = static_cast<int>(labels().size());
    std::vector<Candidate> candidates;
    for (size_t level = 0; level < kStrides.size(); ++level) {
      const float* scores = outputs[level].GetTensorData<float>();
      const float* distributions = outputs[level + kStrides.size()].GetTensorData<float>();
      const int stride = kStrides[level];
      const int cells_high = (kInputHeight + stride - 1) / stride;
      const int cells_wide = (kInputWidth + stride - 1) / stride;
      const int cells = cells_high * cells_wide;

      for (int cell = 0; cell < cells; ++cell) {
        const float* cell_scores = scores + static_cast<ptrdiff_t>(cell) * class_count;
        // The reference thresholds every class independently, so a cell can
        // contribute one candidate per class that clears the bar.
        float best_score = cell_scores[0];
        for (int label = 1; label < class_count; ++label) {
          best_score = std::max(best_score, cell_scores[label]);
        }
        if (best_score <= kConfidenceThreshold) continue;

        // DFL: softmax over kRegBins bins per side, expectation * stride.
        const float* cell_bins = distributions + static_cast<ptrdiff_t>(cell) * 4 * kRegBins;
        std::array<float, 4> distances{};
        for (int side = 0; side < 4; ++side) {
          const float* bins = cell_bins + static_cast<ptrdiff_t>(side) * kRegBins;
          float highest = bins[0];
          for (int bin = 1; bin < kRegBins; ++bin) highest = std::max(highest, bins[bin]);
          float total = 0.0F;
          float expectation = 0.0F;
          for (int bin = 0; bin < kRegBins; ++bin) {
            const float weight = std::exp(bins[bin] - highest);
            total += weight;
            expectation += weight * static_cast<float>(bin);
          }
          distances[side] = expectation / total * static_cast<float>(stride);
        }

        const float center_x =
            (static_cast<float>(cell % cells_wide) + 0.5F) * static_cast<float>(stride);
        const float center_y =
            (static_cast<float>(cell / cells_wide) + 0.5F) * static_cast<float>(stride);
        Candidate candidate;
        // Clip in model space before rescaling, matching the reference.
        candidate.left = std::clamp(center_x - distances[0], 0.0F, static_cast<float>(kInputWidth));
        candidate.top = std::clamp(center_y - distances[1], 0.0F, static_cast<float>(kInputHeight));
        candidate.right =
            std::clamp(center_x + distances[2], 0.0F, static_cast<float>(kInputWidth));
        candidate.bottom =
            std::clamp(center_y + distances[3], 0.0F, static_cast<float>(kInputHeight));
        for (int label = 0; label < class_count; ++label) {
          if (cell_scores[label] > kConfidenceThreshold) {
            candidate.score = cell_scores[label];
            candidate.label = label;
            candidates.push_back(candidate);
          }
        }
      }
    }

    // Class-wise NMS, then rescale from model input space to image pixels.
    std::vector<Candidate> picked;
    for (int label = 0; label < class_count; ++label) {
      std::vector<Candidate> of_class;
      for (const auto& candidate : candidates) {
        if (candidate.label == label) of_class.push_back(candidate);
      }
      if (!of_class.empty()) append_class_nms(picked, std::move(of_class));
    }

    const float scale_x = static_cast<float>(kInputWidth) / static_cast<float>(image.cols);
    const float scale_y = static_cast<float>(kInputHeight) / static_cast<float>(image.rows);
    std::vector<LayoutRegion> regions;
    regions.reserve(picked.size());
    for (const auto& candidate : picked) {
      LayoutRegion region;
      region.label = labels()[static_cast<size_t>(candidate.label)];
      region.confidence = candidate.score;
      region.box.left = static_cast<int>(std::lround(candidate.left / scale_x));
      region.box.top = static_cast<int>(std::lround(candidate.top / scale_y));
      region.box.right = static_cast<int>(std::lround(candidate.right / scale_x));
      region.box.bottom = static_cast<int>(std::lround(candidate.bottom / scale_y));
      regions.push_back(std::move(region));
    }
    // Deterministic order for downstream assembly: by confidence, then geometry.
    std::sort(regions.begin(), regions.end(), [](const LayoutRegion& a, const LayoutRegion& b) {
      if (a.confidence != b.confidence) return a.confidence > b.confidence;
      if (a.box.top != b.box.top) return a.box.top < b.box.top;
      return a.box.left < b.box.left;
    });
    return regions;
  }

 private:
  Ort::Env env_;
  Ort::SessionOptions options_;
  Ort::Session session_{nullptr};
  std::string input_name_;
  std::vector<std::string> output_names_;
  std::vector<const char*> output_name_pointers_;
};

LayoutEngine::LayoutEngine(const std::filesystem::path& model_path)
    : impl_(std::make_unique<Impl>(model_path)) {}

LayoutEngine::~LayoutEngine() = default;

std::vector<LayoutRegion> LayoutEngine::detect_regions(const cv::Mat& image) {
  return impl_->detect_regions(image);
}

LayoutEnginePool::LayoutEnginePool(const std::filesystem::path& model_path, size_t worker_count)
    : engines_(worker_count, [model_path] { return std::make_unique<LayoutEngine>(model_path); }) {
  engines_.prime();
}

std::vector<LayoutRegion> LayoutEnginePool::detect_regions(const cv::Mat& image) {
  auto lease = engines_.acquire();
  try {
    return lease->detect_regions(image);
  } catch (...) {
    // Same policy as OCR: a session that throws mid-inference may be wedged;
    // rebuild it on next acquire instead of reusing it.
    lease.discard();
    throw;
  }
}

}  // namespace grparse
