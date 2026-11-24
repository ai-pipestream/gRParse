#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "grparse_inference_model.h"
#include "grparse_session_ep.h"

namespace grparse {

namespace {

// Detector input sizes, width x height.  A read is padded to the listed size
// of the smallest area that covers it.  Chosen from the reads of 20 ACM
// papers in selective mode plus the test corpus (273 detector runs, 153
// distinct sizes without buckets, 104 at multiples of 64): the widths are a
// small picture, a column-wide picture and a page-wide one at 200 DPI, and
// the last three are full pages (letter upright and turned, A4 inside the
// first) and the square that covers every read at the default
// GRPARSE_OCR_MAX_SIDE.  Over those reads the padded area is 1.31 times the
// unpadded area.
constexpr std::array<OcrDetSize, 9> kDetSizes = {{
    {320, 320},
    {640, 480},
    {1088, 544},
    {1088, 832},
    {1088, 1472},
    {2144, 1088},
    {1664, 2144},
    {2144, 1664},
    {2144, 2144},
}};
// Past the list (GRPARSE_OCR_MAX_SIDE above its default) each side rounds up
// to a multiple of this.
constexpr int kDetOverflowStep = 512;

// Line widths at height 48, about a quarter apart.  Chosen from the 40,332
// lines of the same reads plus the papers read in full (1,311 distinct
// widths without buckets): over them the padded width is 1.11 times the
// unpadded width.  Past the last entry the width rounds up to a multiple of
// kRecOverflowStep.
constexpr std::array<int, 14> kRecWidths = {64,  96,  128,  192,  256,  320,  448,
                                            576, 768, 1024, 1280, 1600, 2048, 2560};
constexpr int kRecOverflowStep = 640;

int env_step(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return 0;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (*end != '\0' || parsed < 0 || parsed > 4096 || (parsed != 0 && parsed % 32 != 0)) {
    throw std::invalid_argument(std::string(name) + " must be 0 or a multiple of 32 up to 4096");
  }
  return static_cast<int>(parsed);
}

bool on_gpu_device(const std::string& device) { return device.find("GPU") != std::string::npos; }

// Everything the bucket functions read, resolved together from the
// environment and the provider selection.
struct BucketConfig {
  // Which nets get fixed shapes (GRPARSE_OCR_SHAPE_BUCKETS).
  bool det = false;
  bool rec = false;
  // GRPARSE_OCR_DET_STEP / GRPARSE_OCR_REC_STEP: a multiple of 32 rounds the
  // detector sides or the line width up to a multiple of it instead of using
  // the lists; 0 (the default) uses the lists.
  int det_step = 0;
  int rec_step = 0;
  // GRPARSE_OCR_BUCKET_PAD.
  OcrPad det_pad = OcrPad::kTile;
  OcrPad rec_pad = OcrPad::kConstant;
};

BucketConfig read_config() {
  BucketConfig config;
  const char* value = std::getenv("GRPARSE_OCR_SHAPE_BUCKETS");
  const std::string name = value == nullptr || *value == '\0' ? "auto" : value;
  if (name == "on") {
    config.det = config.rec = true;
  } else if (name == "det") {
    config.det = true;
  } else if (name == "rec") {
    config.rec = true;
  } else if (name == "auto") {
    // Both nets on the provider that compiles per input shape; CPU and CUDA
    // run every shape with the same kernels and keep their exact inputs.
    const OrtEpSelection selection = ort_ep_selection();
    config.det = config.rec =
        selection.ep == OrtEp::kOpenVino && on_gpu_device(selection.openvino_device);
  } else if (name != "off") {
    throw std::invalid_argument("GRPARSE_OCR_SHAPE_BUCKETS must be auto, on, det, rec or off");
  }
  config.det_step = env_step("GRPARSE_OCR_DET_STEP");
  config.rec_step = env_step("GRPARSE_OCR_REC_STEP");
  const char* pad = std::getenv("GRPARSE_OCR_BUCKET_PAD");
  const std::string pad_mode = pad == nullptr || *pad == '\0' ? "auto" : pad;
  if (pad_mode == "replicate") {
    config.det_pad = config.rec_pad = OcrPad::kReplicate;
  } else if (pad_mode == "zero") {
    config.det_pad = OcrPad::kConstant;
  } else if (pad_mode != "auto" && pad_mode != "tile") {
    throw std::invalid_argument("GRPARSE_OCR_BUCKET_PAD must be auto, tile, replicate or zero");
  }
  return config;
}

// The resolved settings.  Read on every line, so the fast path is one atomic
// load; a reset swaps in a null and the next reader resolves again.  Earlier
// configs stay alive in `resolved` because a reader may still hold one.
std::mutex config_mutex;
std::deque<BucketConfig> resolved;
std::atomic<const BucketConfig*> active{nullptr};

const BucketConfig& config() {
  if (const BucketConfig* current = active.load(std::memory_order_acquire)) return *current;
  std::lock_guard<std::mutex> lock(config_mutex);
  if (const BucketConfig* current = active.load(std::memory_order_acquire)) return *current;
  const BucketConfig& fresh = resolved.emplace_back(read_config());
  active.store(&fresh, std::memory_order_release);
  return fresh;
}

int round_up(int value, int step) { return (std::max(value, 1) + step - 1) / step * step; }

// The listed size of the smallest area that covers the read; the first one
// when two tie.  A read no listed size covers gets its own rounded sides.
OcrDetSize listed_det_size(int width, int height) {
  const OcrDetSize* best = nullptr;
  for (const OcrDetSize& size : kDetSizes) {
    if (size.width < width || size.height < height) continue;
    if (best == nullptr || size.width * size.height < best->width * best->height) best = &size;
  }
  if (best != nullptr) return *best;
  return {round_up(width, kDetOverflowStep), round_up(height, kDetOverflowStep)};
}

}  // namespace

void reset_ocr_shape_buckets() {
  std::lock_guard<std::mutex> lock(config_mutex);
  active.store(nullptr, std::memory_order_release);
}

bool ocr_shape_buckets() { return config().det || config().rec; }

OcrDetSize ocr_det_bucket(int width, int height) {
  const BucketConfig& settings = config();
  if (!settings.det) return {width, height};
  if (settings.det_step > 0) {
    return {round_up(width, settings.det_step), round_up(height, settings.det_step)};
  }
  return listed_det_size(width, height);
}

int ocr_rec_bucket(int width) {
  const BucketConfig& settings = config();
  if (!settings.rec) return width;
  if (settings.rec_step > 0) return round_up(width, settings.rec_step);
  for (const int bucket : kRecWidths) {
    if (width <= bucket) return bucket;
  }
  return round_up(width, kRecOverflowStep);
}

OcrPad ocr_det_pad() { return config().det_pad; }

OcrPad ocr_rec_pad() { return config().rec_pad; }

size_t ocr_det_bucket_count() { return kDetSizes.size(); }

size_t ocr_rec_bucket_count(int max_rec_width) {
  size_t count = 0;
  for (const int bucket : kRecWidths) {
    ++count;
    if (max_rec_width <= bucket) return count;
  }
  // The last listed width is itself a multiple of the overflow step.
  return count + static_cast<size_t>(round_up(max_rec_width, kRecOverflowStep) - kRecWidths.back()) /
                     kRecOverflowStep;
}

int ocr_det_max_side(int configured) { return configured; }

std::pair<int, int> ocr_det_bucket_shape(int width, int height) {
  const OcrDetSize size = ocr_det_bucket(width, height);
  return {size.width, size.height};
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
