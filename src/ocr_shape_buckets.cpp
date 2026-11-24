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
// Native OpenVINO compiles one model per static input shape, so the
// detector input is letterboxed (padded right and bottom with repeated edge
// pixels, the map cropped back) into one of twelve buckets: longest side
// 640, 960, 1280 or 1600, and short side three quarters of it (portrait or
// landscape) or square for inputs squarer than 4:3.
// The page is scaled so its longest side is at most 1600, which also keeps
// the B70 clear of the full 2048 x 1600 detector input that faulted it
// (2026-11-16).  GRPARSE_OCR_DET_LADDER=grid selects the older 7 x 7 grid of
// independent sides (320..1600) for comparison.
constexpr std::array<int, 4> kDetLongSides = {640, 960, 1280, 1600};
constexpr std::array<int, 7> kDetSides = {320, 480, 640, 800, 960, 1280, 1600};
constexpr int kDetMaxSide = 1600;

int short_side_of(int long_side) { return long_side * 3 / 4 / 32 * 32; }

bool det_grid_ladder() {
  static const bool grid = [] {
    const char* value = std::getenv("GRPARSE_OCR_DET_LADDER");
    const std::string mode = value == nullptr || *value == '\0' ? "letterbox" : value;
    if (mode == "letterbox") return false;
    if (mode == "grid") return true;
    throw std::invalid_argument("GRPARSE_OCR_DET_LADDER must be letterbox or grid");
  }();
  return grid;
}

// The direction and recognition nets' fixed batch on native OpenVINO.
constexpr int64_t kNativeBatch = 8;
constexpr int kRecHeight = 48;
constexpr int kClsWidth = 192;

// Native OpenVINO, on any device: it compiles per static shape everywhere.
bool native_openvino() {
#ifdef GRPARSE_NATIVE_OPENVINO
  return ort_ep_selection().ep == OrtEp::kOpenVino;
#else
  return false;
#endif
}

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
    // Native OpenVINO buckets both nets: it compiles one model per static
    // shape, so every shape has to come from a finite set.
    if (native_openvino()) return BucketMode{true, true};
    const OrtEpSelection selection = ort_ep_selection();
    const bool gpu = selection.ep == OrtEp::kOpenVino && on_gpu_device(selection.openvino_device);
    return BucketMode{gpu, false};
  }();
  return mode;
}

bool ocr_shape_buckets() { return bucket_mode().det || bucket_mode().rec; }

int ocr_det_bucket(int side) {
  if (!bucket_mode().det) return side;
  if (native_openvino() && det_grid_ladder()) {
    for (const int bucket : kDetSides) {
      if (side <= bucket) return bucket;
    }
  }
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

int ocr_det_max_side(int configured) {
  return native_openvino() ? std::min(configured, kDetMaxSide) : configured;
}

std::pair<int, int> ocr_det_bucket_shape(int width, int height) {
  if (!bucket_mode().det) return {width, height};
  if (!native_openvino() || det_grid_ladder()) return {ocr_det_bucket(width), ocr_det_bucket(height)};
  // The smallest-area bucket that covers the input: the least padding, so
  // the least detector work.
  std::pair<int, int> best{0, 0};
  int64_t best_area = -1;
  for (const int bucket : kDetLongSides) {
    const int narrow = short_side_of(bucket);
    for (const std::pair<int, int>& candidate :
         {std::pair{bucket, narrow}, std::pair{narrow, bucket}, std::pair{bucket, bucket}}) {
      if (candidate.first < width || candidate.second < height) continue;
      const int64_t area = int64_t{candidate.first} * candidate.second;
      if (best_area < 0 || area < best_area) {
        best = candidate;
        best_area = area;
      }
    }
  }
  if (best_area >= 0) return best;
  return {std::max(width, kDetMaxSide), std::max(height, kDetMaxSide)};
}

std::string_view ocr_net_name(OcrNet net) {
  switch (net) {
    case OcrNet::kDetection: return "OCR detection";
    case OcrNet::kDirection: return "OCR direction";
    case OcrNet::kRecognition: return "OCR recognition";
  }
  return "OCR";
}

// The native OpenVINO half of ocr_net_shapes and ocr_prewarm_sets
// (src/tensorrt/ocr_profiles.cpp holds the functions and the TensorRT half):
// empty unless the selection is OpenVINO in a native OpenVINO build.
ModelShapePolicy openvino_ocr_net_shapes(OcrNet net) {
  if (!native_openvino() || net == OcrNet::kDetection) return {};
  return ModelShapePolicy{{kNativeBatch}};
}

std::vector<std::vector<TensorSpec>> openvino_ocr_prewarm_sets(OcrNet net) {
  std::vector<std::vector<TensorSpec>> sets;
  if (!native_openvino()) return sets;
  switch (net) {
    case OcrNet::kDetection:
      if (det_grid_ladder()) {
        for (const int height : kDetSides) {
          for (const int width : kDetSides) {
            sets.push_back({{TensorElement::kFloat32, {1, 3, height, width}}});
          }
        }
      } else {
        for (const int bucket : kDetLongSides) {
          const int narrow = short_side_of(bucket);
          sets.push_back({{TensorElement::kFloat32, {1, 3, bucket, narrow}}});
          sets.push_back({{TensorElement::kFloat32, {1, 3, narrow, bucket}}});
          sets.push_back({{TensorElement::kFloat32, {1, 3, bucket, bucket}}});
        }
      }
      break;
    case OcrNet::kDirection:
      sets.push_back({{TensorElement::kFloat32, {kNativeBatch, 3, kRecHeight, kClsWidth}}});
      break;
    case OcrNet::kRecognition:
      for (const int width : kRecWidths) {
        sets.push_back({{TensorElement::kFloat32, {kNativeBatch, 3, kRecHeight, width}}});
      }
      break;
  }
  return sets;
}

}  // namespace grparse
