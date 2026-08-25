#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/layout_decode.h"
#include "grparse/ocr_types.h"

namespace grparse {

// Detects layout regions on a rendered page raster.  The scheduler talks to
// this interface so tests can label pages without a model.
class RegionDetector {
 public:
  virtual ~RegionDetector() = default;
  virtual std::vector<LayoutRegion> detect_regions(const cv::Mat& image) = 0;
  // Attribution for the items the detections become.  Test doubles keep the
  // generic name; the real engine names the model it loaded.
  virtual std::string model_name() const { return "layout"; }
};

// Layout detection on ONNX Runtime, bound to the process-wide execution
// provider selection so CUDA, OpenVINO, and CPU builds all take the same path.
// Which detector runs is a startup choice (LayoutModel); both decoders are
// compiled in and the hot path dispatches through one virtual call, not a
// chain of model tests.
//
// One instance owns exactly one Ort::Session and serves every inference worker
// concurrently.  Ort::Session::Run is thread-safe and the decoders keep all of
// their working state on the calling worker's stack, so a session per worker
// would buy nothing but another full copy of the weights - which for the
// query-based detector is 171 MiB each.
//
// Anti-seesaw contract: detect_regions is a batch=1 device call on the same
// raster the OCR stage already holds; it neither retains the image nor blocks
// on anything but its own inference.
class LayoutEngine final : public RegionDetector {
 public:
  // Label set of a model, index == model class id.
  static const std::vector<std::string>& labels(LayoutModel model);

  // Throws when the model file is missing or the configured execution provider
  // cannot initialize (startup fail-loud, same policy as OCR).
  LayoutEngine(const std::filesystem::path& model_path, LayoutModel model);
  ~LayoutEngine() override;
  LayoutEngine(const LayoutEngine&) = delete;
  LayoutEngine& operator=(const LayoutEngine&) = delete;

  // BGR image in, regions in that image's pixel space out.  Safe to call from
  // any number of threads at once.
  std::vector<LayoutRegion> detect_regions(const cv::Mat& image) override;

  std::string model_name() const override {
    return "layout-" + std::string(layout_model_name(model_));
  }

  LayoutModel model() const { return model_; }
  const std::vector<std::string>& labels() const { return layout_labels(model_); }

  // The shared session's counterpart to pool lease counters: how many pages
  // the session has served and the most workers ever inside it at once.
  struct Stats {
    uint64_t detections = 0;
    uint64_t peak_concurrency = 0;
  };
  Stats stats() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  LayoutModel model_;
};

}  // namespace grparse
