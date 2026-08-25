#include "grparse/layout_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <print>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"

namespace grparse {
namespace {

// One inference plus decode for a model family.  Implementations are immutable
// after bind(): every buffer a detect() call needs lives on the calling
// worker's stack, which is what lets one shared session serve all of them.
class LayoutStrategy {
 public:
  virtual ~LayoutStrategy() = default;
  // Validates the graph against what the decode expects and caches its
  // input/output names.  Throws when the file is not the model it claims.
  virtual void bind(Ort::Session& session) = 0;
  virtual std::vector<LayoutRegion> detect(Ort::Session& session, const cv::Mat& image) const = 0;
};

Ort::MemoryInfo cpu_memory() {
  return Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
}

// ---------------------------------------------------------------------------
// Query-based detector: 300 queries, boxes already in page pixels, no NMS.
// ---------------------------------------------------------------------------

constexpr int kQueryInputSide = 640;

class QueryDetectorStrategy final : public LayoutStrategy {
 public:
  void bind(Ort::Session& session) override {
    Ort::AllocatorWithDefaultOptions allocator;
    if (session.GetInputCount() != 2 || session.GetOutputCount() != 3) {
      throw std::runtime_error(
          "Layout model does not look like the query-based detector (expected 2 inputs and 3 "
          "outputs)");
    }
    for (size_t index = 0; index < 2; ++index) {
      input_names_.push_back(session.GetInputNameAllocated(index, allocator).get());
    }
    for (size_t index = 0; index < 3; ++index) {
      output_names_.push_back(session.GetOutputNameAllocated(index, allocator).get());
    }
    if (input_names_[0] != "images" || input_names_[1] != "orig_target_sizes") {
      throw std::runtime_error("Layout model inputs must be images and orig_target_sizes");
    }
    labels_index_ = name_index("labels");
    boxes_index_ = name_index("boxes");
    scores_index_ = name_index("scores");
    for (const auto& name : input_names_) input_pointers_.push_back(name.c_str());
    for (const auto& name : output_names_) output_pointers_.push_back(name.c_str());
  }

  std::vector<LayoutRegion> detect(Ort::Session& session, const cv::Mat& image) const override {
    // Preprocess: bilinear resize to the fixed square, channel order swapped
    // to RGB, raw bytes.  This graph rescales and normalizes internally, so
    // neither happens here.
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(kQueryInputSide, kQueryInputSide), 0, 0, cv::INTER_LINEAR);
    std::vector<uint8_t> pixels(static_cast<size_t>(3) * kQueryInputSide * kQueryInputSide);
    const size_t plane = static_cast<size_t>(kQueryInputSide) * kQueryInputSide;
    for (int row = 0; row < kQueryInputSide; ++row) {
      const cv::Vec3b* source = resized.ptr<cv::Vec3b>(row);
      for (int column = 0; column < kQueryInputSide; ++column) {
        const size_t offset = static_cast<size_t>(row) * kQueryInputSide + column;
        for (int channel = 0; channel < 3; ++channel) {
          // BGR pixel -> RGB tensor channel.
          pixels[plane * static_cast<size_t>(channel) + offset] = source[column][2 - channel];
        }
      }
    }

    // Width first: the graph rescales its normalized boxes by this pair, so
    // transposing it silently produces boxes for a page of the wrong shape.
    std::array<int64_t, 2> original_size = {static_cast<int64_t>(image.cols),
                                            static_cast<int64_t>(image.rows)};
    const std::array<int64_t, 4> image_shape = {1, 3, kQueryInputSide, kQueryInputSide};
    const std::array<int64_t, 2> size_shape = {1, 2};
    const Ort::MemoryInfo memory = cpu_memory();
    std::array<Ort::Value, 2> inputs = {
        Ort::Value::CreateTensor<uint8_t>(memory, pixels.data(), pixels.size(), image_shape.data(),
                                          image_shape.size()),
        Ort::Value::CreateTensor<int64_t>(memory, original_size.data(), original_size.size(),
                                          size_shape.data(), size_shape.size())};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, input_pointers_.data(), inputs.data(),
                               inputs.size(), output_pointers_.data(), output_pointers_.size());

    const auto shape = outputs[scores_index_].GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 2 || shape[0] != 1) {
      throw std::runtime_error("Layout model returned an unexpected score shape");
    }
    return decode_query_detector(outputs[labels_index_].GetTensorData<int64_t>(),
                                 outputs[boxes_index_].GetTensorData<float>(),
                                 outputs[scores_index_].GetTensorData<float>(),
                                 static_cast<size_t>(shape[1]), image.cols, image.rows);
  }

 private:
  size_t name_index(const std::string& name) const {
    for (size_t index = 0; index < output_names_.size(); ++index) {
      if (output_names_[index] == name) return index;
    }
    throw std::runtime_error("Layout model has no output named " + name);
  }

  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  std::vector<const char*> input_pointers_;
  std::vector<const char*> output_pointers_;
  size_t labels_index_ = 0;
  size_t boxes_index_ = 0;
  size_t scores_index_ = 0;
};

// ---------------------------------------------------------------------------
// Anchor-free detector: four pyramid levels of class scores plus DFL box
// distributions that must be decoded and suppressed here.  Geometry and
// thresholds mirror the handler this decode is golden-tested against.
// ---------------------------------------------------------------------------

constexpr int kGridHeight = 800;
constexpr int kGridWidth = 608;
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
  // Box in model input space, clipped there before rescaling.
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
  std::ranges::sort(candidates,
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

class AnchorFreeStrategy final : public LayoutStrategy {
 public:
  void bind(Ort::Session& session) override {
    Ort::AllocatorWithDefaultOptions allocator;
    input_name_ = session.GetInputNameAllocated(0, allocator).get();
    const size_t output_count = session.GetOutputCount();
    if (output_count != kStrides.size() * 2) {
      throw std::runtime_error("Layout model has an unexpected output count");
    }
    for (size_t index = 0; index < output_count; ++index) {
      output_names_.push_back(session.GetOutputNameAllocated(index, allocator).get());
    }
    for (const auto& name : output_names_) output_pointers_.push_back(name.c_str());
  }

  std::vector<LayoutRegion> detect(Ort::Session& session, const cv::Mat& image) const override {
    // Preprocess: plain resize (no aspect preservation, per reference),
    // scale to [0,1], per-channel normalize, HWC -> CHW.
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(kGridWidth, kGridHeight));
    std::vector<float> tensor(static_cast<size_t>(3) * kGridHeight * kGridWidth);
    const size_t plane = static_cast<size_t>(kGridHeight) * kGridWidth;
    for (int row = 0; row < kGridHeight; ++row) {
      const cv::Vec3b* pixels = resized.ptr<cv::Vec3b>(row);
      for (int column = 0; column < kGridWidth; ++column) {
        const size_t offset = static_cast<size_t>(row) * kGridWidth + column;
        for (int channel = 0; channel < 3; ++channel) {
          tensor[plane * static_cast<size_t>(channel) + offset] =
              (static_cast<float>(pixels[column][channel]) / 255.0F - kMean[channel]) /
              kStd[channel];
        }
      }
    }

    const std::array<int64_t, 4> input_shape = {1, 3, kGridHeight, kGridWidth};
    const Ort::MemoryInfo memory = cpu_memory();
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                       input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name_.c_str()};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                               output_pointers_.data(), output_pointers_.size());

    // Decode each pyramid level: the first half of the outputs are class
    // scores [1, N, C], the second half the matching DFL box distributions
    // [1, N, 4 * kRegBins].
    const auto& names = layout_labels(LayoutModel::kPicoDet);
    const int class_count = static_cast<int>(names.size());
    std::vector<Candidate> candidates;
    for (size_t level = 0; level < kStrides.size(); ++level) {
      const float* scores = outputs[level].GetTensorData<float>();
      const float* distributions = outputs[level + kStrides.size()].GetTensorData<float>();
      const int stride = kStrides[level];
      const int cells_high = (kGridHeight + stride - 1) / stride;
      const int cells_wide = (kGridWidth + stride - 1) / stride;
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
        candidate.left = std::clamp(center_x - distances[0], 0.0F, static_cast<float>(kGridWidth));
        candidate.top = std::clamp(center_y - distances[1], 0.0F, static_cast<float>(kGridHeight));
        candidate.right = std::clamp(center_x + distances[2], 0.0F, static_cast<float>(kGridWidth));
        candidate.bottom =
            std::clamp(center_y + distances[3], 0.0F, static_cast<float>(kGridHeight));
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

    const float scale_x = static_cast<float>(kGridWidth) / static_cast<float>(image.cols);
    const float scale_y = static_cast<float>(kGridHeight) / static_cast<float>(image.rows);
    std::vector<LayoutRegion> regions;
    regions.reserve(picked.size());
    for (const auto& candidate : picked) {
      LayoutRegion region;
      region.label = names[static_cast<size_t>(candidate.label)];
      region.confidence = candidate.score;
      region.left = static_cast<int>(std::lround(candidate.left / scale_x));
      region.top = static_cast<int>(std::lround(candidate.top / scale_y));
      region.right = static_cast<int>(std::lround(candidate.right / scale_x));
      region.bottom = static_cast<int>(std::lround(candidate.bottom / scale_y));
      regions.push_back(std::move(region));
    }
    sort_regions(regions);
    return regions;
  }

 private:
  std::string input_name_;
  std::vector<std::string> output_names_;
  std::vector<const char*> output_pointers_;
};

std::unique_ptr<LayoutStrategy> make_strategy(LayoutModel model) {
  if (model == LayoutModel::kHeron) return std::make_unique<QueryDetectorStrategy>();
  return std::make_unique<AnchorFreeStrategy>();
}

}  // namespace

const std::vector<std::string>& LayoutEngine::labels(LayoutModel model) {
  return layout_labels(model);
}

class LayoutEngine::Impl {
 public:
  Impl(const std::filesystem::path& model_path, LayoutModel model)
      : env_(ORT_LOGGING_LEVEL_ERROR, "grparse-layout"), strategy_(make_strategy(model)) {
    if (!std::filesystem::exists(model_path)) {
      throw std::runtime_error("Layout model for GRPARSE_LAYOUT_MODEL=" +
                               std::string(layout_model_name(model)) +
                               " is missing: " + model_path.string() +
                               " (see models/README.md)");
    }
    // Same provider decision point as the OCR sessions. Precision is pinned
    // PER MODEL: the query detector's marginal detections do not survive
    // half precision, so it demands single precision on every device; the
    // anchor-free detector is accuracy-safe at half precision (2e-4 output
    // delta) and single precision trips a GPU-plugin runtime-compile
    // failure on its graph on Intel devices, so it keeps the provider
    // default. One session serves every worker, so it is the one that may
    // have all the cores; the pooled sessions divide them.
    const OrtPrecision precision = model == LayoutModel::kHeron
                                       ? OrtPrecision::kFloat32
                                       : OrtPrecision::kProviderDefault;
    session_ = make_session(env_, model_path, "layout", precision, kIntraOpAllCores);
    strategy_->bind(session_);
    // Some provider failures only surface at the first inference (a runtime
    // kernel compile, not session creation), which would otherwise leave a
    // running server failing every page. Probe once; on failure retreat to
    // CPU exactly like a creation failure would have.
    const cv::Mat probe(64, 64, CV_8UC3, cv::Scalar(255, 255, 255));
    try {
      (void)strategy_->detect(session_, probe);
    } catch (const std::exception& error) {
      std::println(stderr,
                   "gRParse layout: the configured execution provider failed its first "
                   "inference ({}); this model runs on CPU",
                   error.what());
      session_ = make_cpu_session(env_, model_path, kIntraOpAllCores);
      strategy_->bind(session_);
      (void)strategy_->detect(session_, probe);  // a CPU failure is fatal
    }
  }

  std::vector<LayoutRegion> detect_regions(const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3) {
      throw std::invalid_argument("Layout detection expects a non-empty BGR image");
    }
    const uint64_t inside = in_flight_.fetch_add(1) + 1;
    uint64_t peak = peak_concurrency_.load();
    while (inside > peak && !peak_concurrency_.compare_exchange_weak(peak, inside)) {
    }
    try {
      auto regions = strategy_->detect(session_, image);
      in_flight_.fetch_sub(1);
      detections_.fetch_add(1);
      return regions;
    } catch (...) {
      in_flight_.fetch_sub(1);
      throw;
    }
  }

  Stats stats() const { return Stats{detections_.load(), peak_concurrency_.load()}; }

 private:
  Ort::Env env_;
  Ort::Session session_{nullptr};
  std::unique_ptr<LayoutStrategy> strategy_;
  std::atomic<uint64_t> in_flight_{0};
  std::atomic<uint64_t> peak_concurrency_{0};
  std::atomic<uint64_t> detections_{0};
};

LayoutEngine::LayoutEngine(const std::filesystem::path& model_path, LayoutModel model)
    : impl_(std::make_unique<Impl>(model_path, model)), model_(model) {}

LayoutEngine::~LayoutEngine() = default;

std::vector<LayoutRegion> LayoutEngine::detect_regions(const cv::Mat& image) {
  return impl_->detect_regions(image);
}

LayoutEngine::Stats LayoutEngine::stats() const { return impl_->stats(); }

}  // namespace grparse
