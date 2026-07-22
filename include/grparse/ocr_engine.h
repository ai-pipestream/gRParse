#pragma once

#include <filesystem>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "OcrLite.h"
#include "grparse/ocr_types.h"

namespace grparse {

class OcrEngine {
 public:
  using Line = OcrLine;
  using Page = OcrPage;

  OcrEngine(const std::filesystem::path& model_directory, int gpu_index);

  Page extract_page(const cv::Mat& image);
  static constexpr const char* execution_provider() { return "CUDA"; }

 private:
  std::unique_ptr<OcrLite> engine_;
};

class PageRecognizer {
 public:
  virtual ~PageRecognizer() = default;
  virtual OcrPage extract_page(const cv::Mat& image) = 0;
};

class OcrEnginePool final : public PageRecognizer {
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

  OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count,
                int gpu_index);

  Lease acquire();
  OcrPage extract_page(const cv::Mat& image) override;
  size_t size() const;

 private:
  void release(size_t index);

  std::vector<std::unique_ptr<OcrEngine>> engines_;
  std::deque<size_t> available_;
  std::mutex mutex_;
  std::condition_variable available_cv_;
};

}  // namespace grparse
