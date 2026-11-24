#include "ocr_profiles.h"

#include <algorithm>
#include <atomic>
#include <climits>

#include "grparse_session_ep.h"

namespace grparse {
namespace trt {
namespace {

constexpr int kDetectorMinSide = 32;
// An upright letter page at 200 DPI as the detector reads it (A4 fits
// inside): width x height.
constexpr int kDetectorTunedWidth = 1664;
constexpr int kDetectorTunedHeight = 2144;
constexpr int64_t kLineHeight = 48;
constexpr int64_t kDirectionWidth = 192;
// The widest line a batch carries, and the width a batch is tuned for.
constexpr int64_t kRecBatchMaxWidth = 2560;
constexpr int64_t kRecTunedWidth = 576;
constexpr int64_t kRecWideTunedWidth = 3200;

}  // namespace

int detector_side_limit(int max_side_len, int padding) {
  return std::max(kDetectorMinSide, (max_side_len + 2 * padding) / 32 * 32);
}

std::vector<ShapeProfile> detector_profiles(int side_limit) {
  const int64_t limit = std::max(kDetectorMinSide, side_limit);
  ShapeProfile profile;
  profile.inputs.push_back(ShapeRange{
      {1, 3, kDetectorMinSide, kDetectorMinSide},
      {1, 3, std::min<int64_t>(kDetectorTunedHeight, limit),
       std::min<int64_t>(kDetectorTunedWidth, limit)},
      {1, 3, limit, limit}});
  return {profile};
}

std::vector<ShapeProfile> recognizer_profiles(size_t batch) {
  const auto rows = static_cast<int64_t>(std::max<size_t>(1, batch));
  ShapeProfile lines;
  lines.inputs.push_back(ShapeRange{{1, 3, kLineHeight, kRecMinWidth},
                                    {rows, 3, kLineHeight, kRecTunedWidth},
                                    {rows, 3, kLineHeight, kRecBatchMaxWidth}});
  // Past that a line runs alone: a batch of them would need activation
  // memory for `rows` times the width.  Such lines are rare and mostly just
  // past the batch limit, which is what this one is tuned for.
  ShapeProfile wide;
  wide.inputs.push_back(ShapeRange{{1, 3, kLineHeight, kRecMinWidth},
                                   {1, 3, kLineHeight, kRecWideTunedWidth},
                                   {1, 3, kLineHeight, kRecMaxWidth}});
  return {lines, wide};
}

std::vector<ShapeProfile> direction_profiles(size_t batch) {
  const auto rows = static_cast<int64_t>(std::max<size_t>(1, batch));
  ShapeProfile profile;
  profile.inputs.push_back(ShapeRange{{1, 3, kLineHeight, kDirectionWidth},
                                      {rows, 3, kLineHeight, kDirectionWidth},
                                      {rows, 3, kLineHeight, kDirectionWidth}});
  return {profile};
}

}  // namespace trt

namespace {

bool tensorrt_selected() {
  return native_tensorrt_build() && ort_ep_selection().ep == OrtEp::kCuda;
}

// The defaults of OcrDetectOptions (include/grparse/ocr_engine.h) until the
// OCR engine reports the configured ones.
std::atomic<int> detector_limit{trt::detector_side_limit(2048, 50)};

}  // namespace

void set_ocr_detector_side_limit(int max_side_len, int padding) {
  detector_limit.store(trt::detector_side_limit(max_side_len, padding));
}

ModelShapePolicy ocr_net_shapes(OcrNet net) {
  ModelShapePolicy policy;
  // Native OpenVINO: fixed batches for the direction and recognition nets
  // (src/ocr_shape_buckets.cpp); empty on every other selection.
  if (!tensorrt_selected()) return openvino_ocr_net_shapes(net);
  switch (net) {
    case OcrNet::kDetection:
      policy.profiles = trt::detector_profiles(detector_limit.load());
      break;
    case OcrNet::kDirection:
      policy.profiles = trt::direction_profiles(ocr_batch_size());
      break;
    case OcrNet::kRecognition:
      policy.profiles = trt::recognizer_profiles(ocr_batch_size());
      break;
  }
  return policy;
}

// TensorRT builds every profile into the engine at startup, so there is no
// per-shape compile left to warm; ONNX Runtime has none either.  Native
// OpenVINO compiles per static shape, so it warms every bucketed one
// (src/ocr_shape_buckets.cpp).
std::vector<std::vector<TensorSpec>> ocr_prewarm_sets(OcrNet net) {
  return openvino_ocr_prewarm_sets(net);
}

int ocr_rec_min_width() { return tensorrt_selected() ? trt::kRecMinWidth : 1; }

int ocr_rec_max_width() { return tensorrt_selected() ? trt::kRecMaxWidth : INT_MAX; }

}  // namespace grparse
