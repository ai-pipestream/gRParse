#include "grparse/figure_classifier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#include "grparse_session_ep.h"

namespace grparse {
namespace {

// Preprocessing mirrors docling-ibm-models' DocumentFigureClassifierPredictor,
// which this export is verified against: RGB order (unlike the OCR-family
// models, which keep loaded BGR), 224x224 resize, 1/255 scale, and the
// model's own normalization constants.
constexpr int kInputSize = 224;
constexpr std::array<float, 3> kMean = {0.485F, 0.456F, 0.406F};
constexpr std::array<float, 3> kStd = {0.47853944F, 0.4732864F, 0.47434163F};

}  // namespace

const std::vector<std::string>& FigureClassifierEngine::labels() {
  // id2label order from ds4sd/DocumentFigureClassifier config.json.
  static const std::vector<std::string> kLabels = {
      "bar_chart", "bar_code",       "chemistry_markush_structure",
      "chemistry_molecular_structure", "flow_chart", "icon",
      "line_chart", "logo",          "map",
      "other",     "pie_chart",      "qr_code",
      "remote_sensing", "screenshot", "signature",
      "stamp"};
  return kLabels;
}

class FigureClassifierEngine::Impl {
 public:
  explicit Impl(const std::filesystem::path& model_path)
      : env_(ORT_LOGGING_LEVEL_ERROR, "grparse-figure-classifier") {
    if (!std::filesystem::exists(model_path)) {
      throw std::runtime_error("Figure classifier model is missing: " + model_path.string());
    }
    options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    // Same provider decision point as the other model sessions.
    append_execution_provider(options_, -1);
    session_ = Ort::Session(env_, model_path.c_str(), options_);

    Ort::AllocatorWithDefaultOptions allocator;
    input_name_ = session_.GetInputNameAllocated(0, allocator).get();
    output_name_ = session_.GetOutputNameAllocated(0, allocator).get();
  }

  std::vector<FigureClass> classify(const cv::Mat& crop) {
    if (crop.empty() || crop.type() != CV_8UC3) {
      throw std::invalid_argument("Figure classification expects a non-empty BGR crop");
    }
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(kInputSize, kInputSize));

    std::vector<float> tensor(static_cast<size_t>(3) * kInputSize * kInputSize);
    const size_t plane = static_cast<size_t>(kInputSize) * kInputSize;
    for (int row = 0; row < kInputSize; ++row) {
      const cv::Vec3b* pixels = resized.ptr<cv::Vec3b>(row);
      for (int column = 0; column < kInputSize; ++column) {
        const size_t offset = static_cast<size_t>(row) * kInputSize + column;
        for (int channel = 0; channel < 3; ++channel) {
          // BGR pixel -> RGB tensor channel.
          tensor[plane * static_cast<size_t>(channel) + offset] =
              (static_cast<float>(pixels[column][2 - channel]) / 255.0F - kMean[channel]) /
              kStd[channel];
        }
      }
    }

    const std::array<int64_t, 4> input_shape = {1, 3, kInputSize, kInputSize};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                       input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name_.c_str()};
    const char* output_names[] = {output_name_.c_str()};
    auto outputs =
        session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1, output_names, 1);

    const auto& class_labels = labels();
    const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (shape.back() != static_cast<int64_t>(class_labels.size())) {
      throw std::runtime_error("Figure classifier output does not match the label set");
    }
    const float* logits = outputs[0].GetTensorData<float>();

    float highest = logits[0];
    for (size_t index = 1; index < class_labels.size(); ++index) {
      highest = std::max(highest, logits[index]);
    }
    float total = 0.0F;
    std::vector<FigureClass> classes;
    classes.reserve(class_labels.size());
    for (size_t index = 0; index < class_labels.size(); ++index) {
      const float weight = std::exp(logits[index] - highest);
      total += weight;
      classes.push_back({class_labels[index], weight});
    }
    for (auto& entry : classes) entry.confidence /= total;
    std::sort(classes.begin(), classes.end(), [](const FigureClass& a, const FigureClass& b) {
      if (a.confidence != b.confidence) return a.confidence > b.confidence;
      return a.label < b.label;
    });
    return classes;
  }

 private:
  Ort::Env env_;
  Ort::SessionOptions options_;
  Ort::Session session_{nullptr};
  std::string input_name_;
  std::string output_name_;
};

FigureClassifierEngine::FigureClassifierEngine(const std::filesystem::path& model_path)
    : impl_(std::make_unique<Impl>(model_path)) {}

FigureClassifierEngine::~FigureClassifierEngine() = default;

std::vector<FigureClass> FigureClassifierEngine::classify(const cv::Mat& crop) {
  return impl_->classify(crop);
}

FigureClassifierPool::FigureClassifierPool(const std::filesystem::path& model_path,
                                           size_t worker_count)
    : engines_(worker_count,
               [model_path] { return std::make_unique<FigureClassifierEngine>(model_path); }) {
  engines_.prime();
}

std::vector<FigureClass> FigureClassifierPool::classify(const cv::Mat& crop) {
  auto lease = engines_.acquire();
  try {
    return lease->classify(crop);
  } catch (...) {
    // Same policy as OCR: a session that throws mid-inference may be wedged.
    lease.discard();
    throw;
  }
}

}  // namespace grparse
