#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "OcrLite.h"

namespace grparse {

class OcrEngine {
 public:
  struct Line {
    std::string text;
    std::vector<cv::Point> polygon;
  };

  struct Page {
    int width;
    int height;
    std::vector<Line> lines;
  };

  explicit OcrEngine(const std::filesystem::path& model_directory);

  Page extract_page(const std::filesystem::path& image_path);
  static constexpr const char* execution_provider() { return "CUDA"; }

 private:
  std::unique_ptr<OcrLite> engine_;
  std::mutex mutex_;
};

}  // namespace grparse
