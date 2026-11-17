#pragma once

// The two single-shot vision models on the device: the anchor-free layout
// detector and the figure classifier.  Each resizes its image to the HEF's
// one input size and hands the pixels over as they are (the chip
// normalizes); the engines keep their own decode
// (src/layout_engine.cpp, src/figure_classifier.cpp).

#include <array>
#include <span>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse_inference_model.h"
#include "hailo_runtime.h"

namespace grparse::hailo {

inline constexpr size_t kLayoutLevels = 4;

// One page through the layout detector: the pointers are into `tensors`, one
// per pyramid level in stride order.
struct LayoutLevels {
  std::vector<Tensor> tensors;
  std::array<const float*, kLayoutLevels> scores{};
  std::array<const float*, kLayoutLevels> distributions{};
};

class AnchorFreeLayout final {
 public:
  // Checks the HEF against the detector's geometry and finds which output is
  // which by its shape: `class_count` features are a level's scores,
  // 4 * `reg_bins` its box distributions, and the cell count names the
  // level.  Throws std::runtime_error when the HEF does not fit.
  AnchorFreeLayout(HailoModel& model, int grid_width, int grid_height,
                   std::span<const int, kLayoutLevels> strides, int class_count, int reg_bins);

  // `image` is BGR, any size.
  LayoutLevels run(const cv::Mat& image);

 private:
  HailoModel& model_;
  int grid_width_;
  int grid_height_;
  std::array<size_t, kLayoutLevels> score_outputs_{};
  std::array<size_t, kLayoutLevels> distribution_outputs_{};
};

class FigureLogits final {
 public:
  // Throws std::runtime_error unless the HEF takes one square image and
  // answers `class_count` logits.
  FigureLogits(HailoModel& model, int class_count);

  // `crop` is BGR, any size; the result is the model's logits.
  std::vector<float> run(const cv::Mat& crop);

 private:
  HailoModel& model_;
  int side_;
  size_t class_count_;
};

}  // namespace grparse::hailo
