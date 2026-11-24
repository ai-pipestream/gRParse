#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include "grparse_inference_model.h"
#include "grparse_session_ep.h"

namespace grparse {

namespace {

// Line widths at height 48: short labels to full-width lines; past the last
// entry the width rounds up to a multiple of 640.
constexpr std::array<int, 10> kRecWidths = {160, 320, 480, 640, 960, 1280, 1600, 1920, 2560, 3200};
constexpr int kDetStepDefault = 64;

int env_step(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (*end != '\0' || parsed < 0 || parsed > 4096 || (parsed != 0 && parsed % 32 != 0)) {
    throw std::invalid_argument(std::string(name) + " must be 0 or a multiple of 32 up to 4096");
  }
  return static_cast<int>(parsed);
}

// GRPARSE_OCR_DET_STEP: the detector side rounds up to a multiple of it
// (default 64). GRPARSE_OCR_REC_STEP: the recognizer width rounds up to a
// multiple of it; 0 (the default) uses the fixed width list.
int det_step() {
  static const int step = env_step("GRPARSE_OCR_DET_STEP", kDetStepDefault);
  return step == 0 ? kDetStepDefault : step;
}

int rec_step() {
  static const int step = env_step("GRPARSE_OCR_REC_STEP", 0);
  return step;
}

bool on_gpu_device(const std::string& device) {
  return device.rfind("GPU", 0) == 0 || device.find("GPU") != std::string::npos;
}

}  // namespace

// Which nets get fixed shapes: both, the detector only, the recognizer only,
// or neither (GRPARSE_OCR_SHAPE_BUCKETS=on|det|rec|off; auto is "det" for the
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
    // auto buckets the detector only: padding a line changes the recognizer
    // output (its context spans the full line), while the detector at
    // multiples of 64 with repeated edges stays within GPU noise of the
    // unpadded run (101 ACM papers, OpenVINO CPU: 150 figure items, 0 body).
    const OrtEpSelection selection = ort_ep_selection();
    const bool gpu = selection.ep == OrtEp::kOpenVino && on_gpu_device(selection.openvino_device);
    // The Level Zero runtime takes any size as it comes; only the OpenVINO
    // provider compiles per shape.
    return BucketMode{gpu && !level_zero_ocr(), false};
  }();
  return mode;
}

bool ocr_shape_buckets() { return bucket_mode().det || bucket_mode().rec; }

int ocr_det_bucket(int side) {
  if (!bucket_mode().det) return side;
  const int step = det_step();
  if (side <= 0) return step;
  return (side + step - 1) / step * step;
}

int ocr_rec_bucket(int width) {
  if (!bucket_mode().rec) return width;
  if (const int step = rec_step(); step > 0) return (std::max(width, 1) + step - 1) / step * step;
  for (const int bucket : kRecWidths) {
    if (width <= bucket) return bucket;
  }
  constexpr int kStep = 640;
  return (width + kStep - 1) / kStep * kStep;
}

bool ocr_pad_replicate() {
  static const bool replicate = [] {
    const char* value = std::getenv("GRPARSE_OCR_BUCKET_PAD");
    const std::string mode = value == nullptr || *value == '\0' ? "replicate" : value;
    if (mode == "replicate") return true;
    if (mode == "zero") return false;
    throw std::invalid_argument("GRPARSE_OCR_BUCKET_PAD must be replicate or zero");
  }();
  return replicate;
}

int ocr_det_max_side(int configured) { return configured; }

std::pair<int, int> ocr_det_bucket_shape(int width, int height) {
  if (!bucket_mode().det) return {width, height};
  return {ocr_det_bucket(width), ocr_det_bucket(height)};
}

std::string_view ocr_net_name(OcrNet net) {
  switch (net) {
    case OcrNet::kDetection: return "OCR detection";
    case OcrNet::kDirection: return "OCR direction";
    case OcrNet::kRecognition: return "OCR recognition";
  }
  return "OCR";
}

}  // namespace grparse
