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

// Which nets get fixed shapes: both, the detector only, the recognizer only,
// or neither (GRPARSE_OCR_SHAPE_BUCKETS=on|det|rec|off; auto is "on" for the
// OpenVINO provider on a GPU device, "off" otherwise).
struct BucketMode {
  bool det = false;
  bool rec = false;
};

const BucketMode& bucket_mode() {
  static const BucketMode mode = [] {
    const char* value = std::getenv("GRPARSE_OCR_SHAPE_BUCKETS");
    const std::string name = value == nullptr || *value == '\0' ? "auto" : value;
    if (name == "on") return BucketMode{true, true};
    if (name == "off") return BucketMode{};
    if (name == "det") return BucketMode{true, false};
    if (name == "rec") return BucketMode{false, true};
    if (name != "auto") {
      throw std::invalid_argument("GRPARSE_OCR_SHAPE_BUCKETS must be auto, on, det, rec or off");
    }
    const OrtEpSelection selection = ort_ep_selection();
    const bool gpu = selection.ep == OrtEp::kOpenVino && on_gpu_device(selection.openvino_device);
    return BucketMode{gpu, gpu};
  }();
  return mode;
}

bool ocr_shape_buckets() { return bucket_mode().det || bucket_mode().rec; }

int ocr_det_bucket(int side) {
  if (!bucket_mode().det) return side;
  if (side <= 0) return kDetStep;
  return (side + kDetStep - 1) / kDetStep * kDetStep;
}

int ocr_rec_bucket(int width) {
  if (!bucket_mode().rec) return width;
  for (const int bucket : kRecWidths) {
    if (width <= bucket) return bucket;
  }
  constexpr int kStep = 640;
  return (width + kStep - 1) / kStep * kStep;
}

}  // namespace grparse
