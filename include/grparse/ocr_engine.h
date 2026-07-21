#pragma once

#include <filesystem>
#include <condition_variable>
#include <deque>
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

  Page extract_page(const cv::Mat& image);
  static constexpr const char* execution_provider() { return "CUDA"; }

 private:
  std::unique_ptr<OcrLite> engine_;
};

class OcrEnginePool {
 public:
  class Lease {
   public:
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    ~Lease();

    OcrEngine& engine() const;

   private:
    friend class OcrEnginePool;
    Lease(OcrEnginePool* pool, size_t index);
    void release();

    OcrEnginePool* pool_ = nullptr;
    size_t index_ = 0;
  };

  OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count);

  Lease acquire();
  OcrEngine::Page extract_page(const cv::Mat& image);
  size_t size() const;

 private:
  void release(size_t index);

  std::vector<std::unique_ptr<OcrEngine>> engines_;
  std::deque<size_t> available_;
  std::mutex mutex_;
  std::condition_variable available_cv_;
};

}  // namespace grparse
