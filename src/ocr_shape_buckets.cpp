#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "grparse_session_ep.h"

namespace grparse {

namespace {

// Line widths at height 48: short labels to full-width lines; past the last
// entry the width rounds up to a multiple of 640.
constexpr std::array<int, 10> kRecWidths = {160, 320, 480, 640, 960, 1280, 1600, 1920, 2560, 3200};
constexpr int kDetStep = 256;

bool on_gpu_device(const std::string& device) {
  return device.rfind("GPU", 0) == 0 || device.find("GPU") != std::string::npos;
}

}  // namespace

bool ocr_shape_buckets() {
  static const bool enabled = [] {
    const char* value = std::getenv("GRPARSE_OCR_SHAPE_BUCKETS");
    const std::string mode = value == nullptr || *value == '\0' ? "auto" : value;
    if (mode == "on") return true;
    if (mode == "off") return false;
    if (mode != "auto") {
      throw std::invalid_argument("GRPARSE_OCR_SHAPE_BUCKETS must be auto, on or off");
    }
    const OrtEpSelection selection = ort_ep_selection();
    return selection.ep == OrtEp::kOpenVino && on_gpu_device(selection.openvino_device);
  }();
  return enabled;
}

int ocr_det_bucket(int side) {
  if (side <= 0) return kDetStep;
  return (side + kDetStep - 1) / kDetStep * kDetStep;
}

int ocr_rec_bucket(int width) {
  for (const int bucket : kRecWidths) {
    if (width <= bucket) return bucket;
  }
  constexpr int kStep = 640;
  return (width + kStep - 1) / kStep * kStep;
}

}  // namespace grparse
