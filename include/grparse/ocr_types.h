#pragma once

#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace grparse {

struct OcrLine {
  std::string text;
  std::vector<cv::Point> polygon;
  std::optional<float> confidence;
};

struct OcrPage {
  enum class Source { kOcr, kDigitalPdf };

  int width = 0;
  int height = 0;
  std::vector<OcrLine> lines;
  Source source = Source::kOcr;
};

}  // namespace grparse
