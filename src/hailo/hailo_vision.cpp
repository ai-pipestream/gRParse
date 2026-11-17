#include "hailo_vision.h"

#include <stdexcept>
#include <string>

#include <opencv2/imgproc.hpp>

namespace grparse::hailo {
namespace {

// The image at the HEF's size as one continuous uint8 frame.
cv::Mat sized_frame(const cv::Mat& image, int width, int height) {
  cv::Mat resized;
  cv::resize(image, resized, cv::Size(width, height));
  return resized.isContinuous() ? resized : resized.clone();
}

TensorView frame_view(const cv::Mat& frame) {
  return TensorView{TensorElement::kUInt8, frame.data, {1, frame.rows, frame.cols, 3}};
}

}  // namespace

AnchorFreeLayout::AnchorFreeLayout(HailoModel& model, int grid_width, int grid_height,
                                   std::span<const int, kLayoutLevels> strides, int class_count,
                                   int reg_bins)
    : model_(model), grid_width_(grid_width), grid_height_(grid_height) {
  const HailoModelShapes& shapes = model.shapes();
  if (shapes.inputs.size() != 1 || shapes.inputs[0].shape != std::vector<int64_t>{grid_height, grid_width, 3}) {
    throw std::runtime_error("the layout HEF does not take one " + std::to_string(grid_width) +
                             "x" + std::to_string(grid_height) + " image");
  }
  std::array<bool, kLayoutLevels> have_scores{};
  std::array<bool, kLayoutLevels> have_distributions{};
  for (size_t output = 0; output < shapes.outputs.size(); ++output) {
    const std::vector<int64_t>& shape = shapes.outputs[output].shape;
    const int64_t cells = shape[0] * shape[1];
    for (size_t level = 0; level < kLayoutLevels; ++level) {
      const int stride = strides[level];
      const int64_t level_cells = static_cast<int64_t>((grid_height + stride - 1) / stride) *
                                  ((grid_width + stride - 1) / stride);
      if (cells != level_cells) continue;
      if (shape[2] == class_count) {
        score_outputs_[level] = output;
        have_scores[level] = true;
      } else if (shape[2] == 4 * reg_bins) {
        distribution_outputs_[level] = output;
        have_distributions[level] = true;
      }
    }
  }
  for (size_t level = 0; level < kLayoutLevels; ++level) {
    if (!have_scores[level] || !have_distributions[level]) {
      throw std::runtime_error("the layout HEF has no score or box output for stride " +
                               std::to_string(strides[level]));
    }
  }
}

LayoutLevels AnchorFreeLayout::run(const cv::Mat& image) {
  const cv::Mat frame = sized_frame(image, grid_width_, grid_height_);
  const TensorView input = frame_view(frame);
  LayoutLevels levels;
  levels.tensors = model_.run(std::span(&input, 1));
  for (size_t level = 0; level < kLayoutLevels; ++level) {
    levels.scores[level] = levels.tensors[score_outputs_[level]].floats();
    levels.distributions[level] = levels.tensors[distribution_outputs_[level]].floats();
  }
  return levels;
}

FigureLogits::FigureLogits(HailoModel& model, int class_count)
    : model_(model), side_(0), class_count_(static_cast<size_t>(class_count)) {
  const HailoModelShapes& shapes = model.shapes();
  if (shapes.inputs.size() != 1 || shapes.outputs.size() != 1 ||
      shapes.inputs[0].shape[0] != shapes.inputs[0].shape[1] || shapes.inputs[0].shape[2] != 3) {
    throw std::runtime_error("the figure classifier HEF does not take one square image");
  }
  const std::vector<int64_t>& out = shapes.outputs[0].shape;
  if (out[0] * out[1] * out[2] != class_count) {
    throw std::runtime_error("the figure classifier HEF predicts " +
                             std::to_string(out[0] * out[1] * out[2]) +
                             " classes; this build expects " + std::to_string(class_count));
  }
  side_ = static_cast<int>(shapes.inputs[0].shape[0]);
}

std::vector<float> FigureLogits::run(const cv::Mat& crop) {
  const cv::Mat frame = sized_frame(crop, side_, side_);
  const TensorView input = frame_view(frame);
  const std::vector<Tensor> outputs = model_.run(std::span(&input, 1));
  const float* logits = outputs.at(0).floats();
  return std::vector<float>(logits, logits + class_count_);
}

}  // namespace grparse::hailo
