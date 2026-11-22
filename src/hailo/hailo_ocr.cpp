#include "hailo_ocr.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#include "OcrUtils.h"
#include "grparse/ocr_engine.h"
#include "hailo_ocr_plan.h"

// RapidOCR's box extraction (DbNet.cpp): contours of the binary map, scored
// against the probability map, unclipped and scaled back to the page.
std::vector<TextBox> findRsBoxes(const cv::Mat& predMat, const cv::Mat& dilateMat, ScaleParam& s,
                                 const float boxScoreThresh, const float unClipRatio);

namespace grparse::hailo {
namespace {

// Windows per recognizer call: each window's scores are about 1 MiB of
// floats, so a page's windows go through in bounded pieces.
constexpr size_t kRecChunk = 32;
constexpr size_t kClsChunk = 64;
// The recognizer's padding: zero after its normalization, as the CPU path's
// batched recognizer pads.
constexpr uint8_t kRecPad = 127;

TensorView batch_view(const std::vector<uint8_t>& pixels, size_t frames, int height, int width) {
  return TensorView{TensorElement::kUInt8, pixels.data(),
                    {static_cast<int64_t>(frames), height, width, 3}};
}

// Height 48 at the crop's own aspect, cut or padded with white to the net's
// width (RapidOCR's adjustTargetImg, which the direction HEF was calibrated
// on), for a crop of any size.
cv::Mat fit_direction_input(const cv::Mat& crop, int width) {
  const float scale = static_cast<float>(kRecHeight) / static_cast<float>(crop.rows);
  const int scaled_width = std::max(1, static_cast<int>(static_cast<float>(crop.cols) * scale));
  cv::Mat scaled;
  cv::resize(crop, scaled, cv::Size(scaled_width, kRecHeight));
  cv::Mat fitted(kRecHeight, width, CV_8UC3, cv::Scalar(255, 255, 255));
  const int kept = std::min(width, scaled_width);
  scaled(cv::Rect(0, 0, kept, kRecHeight)).copyTo(fitted(cv::Rect(0, 0, kept, kRecHeight)));
  return fitted;
}

void copy_rows(const cv::Mat& source, uint8_t* frame, int frame_width) {
  for (int row = 0; row < source.rows; ++row) {
    std::memcpy(frame + static_cast<size_t>(row) * frame_width * 3, source.ptr<uint8_t>(row),
                static_cast<size_t>(source.cols) * 3);
  }
}

}  // namespace

HailoOcr::HailoOcr(OcrModels models, std::vector<std::string> keys)
    : models_(models), keys_(std::move(keys)) {
  if (models_.detection == nullptr || models_.direction == nullptr ||
      models_.recognition == nullptr) {
    throw std::invalid_argument("Hailo OCR needs its detection, direction and recognition models");
  }
  if (models_.shapes.rec_classes != static_cast<int>(keys_.size())) {
    throw std::invalid_argument(
        "Hailo OCR: the recognizer predicts " + std::to_string(models_.shapes.rec_classes) +
        " classes and the dictionary has " + std::to_string(keys_.size()));
  }
}

cv::Mat HailoOcr::probability_map(const cv::Mat& resized) {
  const int tile = models_.shapes.det_tile;
  const auto columns = plan_tiles(resized.cols, tile, kDetOverlapPixels);
  const auto rows = plan_tiles(resized.rows, tile, kDetOverlapPixels);
  const size_t frame_bytes = static_cast<size_t>(tile) * tile * 3;
  const size_t frames = columns.size() * rows.size();
  // White where a tile reaches past the page, the page's own border colour.
  std::vector<uint8_t> pixels(frames * frame_bytes, uint8_t{255});
  size_t frame = 0;
  for (const TileSpan& row : rows) {
    for (const TileSpan& column : columns) {
      const cv::Rect area(column.start, row.start, std::min(tile, resized.cols - column.start),
                          std::min(tile, resized.rows - row.start));
      copy_rows(resized(area), pixels.data() + frame * frame_bytes, tile);
      ++frame;
    }
  }
  const TensorView input = batch_view(pixels, frames, tile, tile);
  const std::vector<Tensor> outputs = models_.detection->run(std::span(&input, 1));
  const size_t map_size = static_cast<size_t>(tile) * tile;
  if (outputs.size() != 1 || outputs[0].element_count() != frames * map_size) {
    throw std::runtime_error("Hailo OCR detection returned an unexpected output shape");
  }
  cv::Mat map(resized.rows, resized.cols, CV_32F);
  const float* scores = outputs[0].floats();
  frame = 0;
  for (const TileSpan& row : rows) {
    for (const TileSpan& column : columns) {
      const float* tile_scores = scores + frame * map_size;
      const size_t width = static_cast<size_t>(column.keep_end - column.keep_begin);
      for (int y = row.keep_begin; y < row.keep_end; ++y) {
        const float* from = tile_scores + static_cast<size_t>(y - row.start) * tile +
                            (column.keep_begin - column.start);
        std::memcpy(map.ptr<float>(y) + column.keep_begin, from, width * sizeof(float));
      }
      ++frame;
    }
  }
  return map;
}

std::vector<Angle> HailoOcr::angles(const std::vector<cv::Mat>& crops, bool do_angle,
                                    bool most_angle) {
  std::vector<Angle> result(crops.size(), Angle{do_angle ? 0 : -1, 0.0F, 0.0});
  if (!do_angle) return result;
  const int width = models_.shapes.cls_width;
  const size_t frame_bytes = static_cast<size_t>(kRecHeight) * width * 3;
  std::vector<size_t> readable;
  for (size_t i = 0; i < crops.size(); ++i) {
    if (!crops[i].empty()) readable.push_back(i);
  }
  std::vector<AngleScore> scores(crops.size());
  for (size_t first = 0; first < readable.size(); first += kClsChunk) {
    const size_t count = std::min(kClsChunk, readable.size() - first);
    std::vector<uint8_t> pixels(count * frame_bytes);
    for (size_t i = 0; i < count; ++i) {
      const cv::Mat sized = fit_direction_input(crops[readable[first + i]], width);
      copy_rows(sized, pixels.data() + i * frame_bytes, width);
    }
    const TensorView input = batch_view(pixels, count, kRecHeight, width);
    const std::vector<Tensor> outputs = models_.direction->run(std::span(&input, 1));
    if (outputs.size() != 1 || outputs[0].element_count() != count * 2) {
      throw std::runtime_error("Hailo OCR direction returned an unexpected output shape");
    }
    const float* values = outputs[0].floats();
    for (size_t i = 0; i < count; ++i) {
      const float upright = values[i * 2];
      const float flipped = values[i * 2 + 1];
      scores[readable[first + i]] =
          flipped > upright ? AngleScore{1, flipped} : AngleScore{0, upright};
    }
  }
  const int page = most_angle ? vote_page_angle(scores) : 0;
  for (size_t i = 0; i < crops.size(); ++i) {
    result[i].index = most_angle ? page : confident_angle(scores[i]);
    result[i].score = scores[i].score;
  }
  return result;
}

std::vector<TextLine> HailoOcr::read_lines(const std::vector<cv::Mat>& crops) {
  const int width = models_.shapes.rec_width;
  const size_t frame_bytes = static_cast<size_t>(kRecHeight) * width * 3;
  const size_t classes = static_cast<size_t>(models_.shapes.rec_classes);
  struct Line {
    cv::Mat sized;
    RecPlan plan;
    std::vector<std::vector<StepScore>> windows;
  };
  struct WindowRef {
    size_t line;
    size_t window;
  };
  std::vector<Line> lines(crops.size());
  std::vector<WindowRef> pending;
  for (size_t i = 0; i < crops.size(); ++i) {
    if (crops[i].empty()) continue;
    const float scale = static_cast<float>(kRecHeight) / static_cast<float>(crops[i].rows);
    const int line_width = std::max(1, static_cast<int>(static_cast<float>(crops[i].cols) * scale));
    cv::resize(crops[i], lines[i].sized, cv::Size(line_width, kRecHeight));
    lines[i].plan = plan_rec_windows(line_width, width, kRecOverlapSteps);
    lines[i].windows.resize(lines[i].plan.window_starts.size());
    for (size_t w = 0; w < lines[i].windows.size(); ++w) pending.push_back({i, w});
  }
  for (size_t first = 0; first < pending.size(); first += kRecChunk) {
    const size_t count = std::min(kRecChunk, pending.size() - first);
    std::vector<uint8_t> pixels(count * frame_bytes, kRecPad);
    for (size_t i = 0; i < count; ++i) {
      const Line& line = lines[pending[first + i].line];
      const int left = line.plan.window_starts[pending[first + i].window] * kRecStepPixels;
      const cv::Rect area(left, 0, std::min(width, line.sized.cols - left), kRecHeight);
      copy_rows(line.sized(area), pixels.data() + i * frame_bytes, width);
    }
    const TensorView input = batch_view(pixels, count, kRecHeight, width);
    const std::vector<Tensor> outputs = models_.recognition->run(std::span(&input, 1));
    const size_t steps = static_cast<size_t>(width / kRecStepPixels);
    if (outputs.size() != 1 || outputs[0].element_count() != count * steps * classes) {
      throw std::runtime_error("Hailo OCR recognition returned an unexpected output shape");
    }
    for (size_t i = 0; i < count; ++i) {
      lines[pending[first + i].line].windows[pending[first + i].window] =
          argmax_steps(outputs[0].floats() + i * steps * classes, steps, classes);
    }
  }
  std::vector<TextLine> result(crops.size());
  for (size_t i = 0; i < crops.size(); ++i) {
    if (crops[i].empty()) continue;
    const std::vector<StepScore> steps = stitch_steps(lines[i].plan, lines[i].windows);
    DecodedLine decoded = ctc_collapse(steps, keys_);
    result[i] = TextLine{std::move(decoded.text), std::move(decoded.char_scores), 0.0};
  }
  return result;
}

std::vector<TextBlock> HailoOcr::detect(const cv::Mat& image, const OcrDetectOptions& options) {
  // The CPU path's geometry (OcrLite::detect): a white border, then the
  // longest side scaled to the limit, both sides rounded to the detector's
  // stride.
  const int longest = std::max(image.cols, image.rows);
  const int limit = options.max_side_len <= 0 || options.max_side_len > longest
                        ? longest
                        : options.max_side_len;
  cv::Mat padded;
  if (options.padding > 0) {
    cv::copyMakeBorder(image, padded, options.padding, options.padding, options.padding,
                       options.padding, cv::BORDER_ISOLATED, cv::Scalar(255, 255, 255));
  } else {
    padded = image;
  }
  ScaleParam scale = getScaleParam(padded, limit + 2 * options.padding);
  cv::Mat resized;
  cv::resize(padded, resized, cv::Size(scale.dstWidth, scale.dstHeight));

  const cv::Mat scores = probability_map(resized);
  cv::Mat levels(scores.rows, scores.cols, CV_8UC1);
  for (int row = 0; row < scores.rows; ++row) {
    const float* from = scores.ptr<float>(row);
    uint8_t* to = levels.ptr<uint8_t>(row);
    for (int column = 0; column < scores.cols; ++column) {
      to[column] = static_cast<uint8_t>(std::clamp(from[column], 0.0F, 1.0F) * 255.0F);
    }
  }
  cv::Mat binary;
  cv::threshold(levels, binary, options.box_thresh * 255.0, 255.0, cv::THRESH_BINARY);
  cv::Mat dilated;
  cv::dilate(binary, dilated, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));
  const std::vector<TextBox> boxes =
      findRsBoxes(scores, dilated, scale, detect_box_score(options), options.un_clip_ratio);

  std::vector<cv::Mat> crops;
  crops.reserve(boxes.size());
  for (const TextBox& box : boxes) crops.push_back(getRotateCropImage(padded, box.boxPoint));
  const std::vector<Angle> crop_angles = angles(crops, options.do_angle, options.most_angle);
  for (size_t i = 0; i < crops.size(); ++i) {
    if (crop_angles[i].index == 1 && !crops[i].empty()) crops[i] = matRotateClockWise180(crops[i]);
  }
  const std::vector<TextLine> lines = read_lines(crops);

  std::vector<TextBlock> blocks;
  blocks.reserve(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    std::vector<cv::Point> points;
    points.reserve(4);
    for (const cv::Point& point : boxes[i].boxPoint) {
      points.emplace_back(point.x - options.padding, point.y - options.padding);
    }
    blocks.push_back(TextBlock{std::move(points), boxes[i].score, crop_angles[i].index,
                               crop_angles[i].score, 0.0, lines[i].text, lines[i].charScores, 0.0,
                               0.0});
  }
  return blocks;
}

std::vector<std::string> load_ocr_keys(const std::filesystem::path& keys_file) {
  std::ifstream in(keys_file);
  if (!in) throw std::runtime_error("OCR dictionary cannot be read: " + keys_file.string());
  std::vector<std::string> keys{"#"};
  for (std::string line; std::getline(in, line);) keys.push_back(line);
  keys.emplace_back(" ");
  return keys;
}

}  // namespace grparse::hailo
