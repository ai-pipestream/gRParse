#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// Input shapes of the three RapidOcrOnnx nets: the buckets that keep them to
// a fixed set (patches/rapidocr-shape-buckets.patch and
// patches/rapidocr-shape-lists.patch, src/ocr_shape_buckets.cpp) and the
// ledger of the shapes the nets actually ran (src/ocr_shape_ledger.cpp).
//
// This header lives in compat/ because that directory is already on the
// RapidOcrOnnx include path.

namespace grparse {

// Fixed input shapes for the OCR nets.  The OpenVINO GPU plugin compiles a
// network again for every new input shape: with page, picture and line sizes
// left free, one 101-paper run left 16,822 compiled blobs, and the first page
// of each new size spent 7 to 15 s in OCR.  When on, the detector input is
// padded to a size from a short list and its map is cropped back, and a line
// is padded to a width from a short list whose extra time steps are not
// decoded.  GRPARSE_OCR_SHAPE_BUCKETS=auto (default: both nets for the
// OpenVINO provider on a GPU device, neither otherwise), on, det, rec or off.
bool ocr_shape_buckets();

// The detector input size a `width` x `height` image is padded to.
struct OcrDetSize {
  int width = 0;
  int height = 0;
  bool operator==(const OcrDetSize&) const = default;
};
OcrDetSize ocr_det_bucket(int width, int height);
// The same size as a (width, height) pair, the form the patched detector and
// the native backends read.
std::pair<int, int> ocr_det_bucket_shape(int width, int height);
// The detector's longest side: `configured` on every backend of this build.
int ocr_det_max_side(int configured);
// The smallest recognizer width bucket at least `width` pixels.
int ocr_rec_bucket(int width);
// What bucket padding is filled with.
//   kTile       the image repeated (detector only).  The detector pools over
//               its whole input, so padding that looks like the page moves
//               its output least: on tests/data/report_page.png the bucketed
//               CPU read changes 1 of 36 lines, against 12 with either
//               padding below.
//   kReplicate  the edge pixels repeated.
//   kConstant   white for the detector; for the recognizer zeros after
//               normalization, the padding the model was trained with and
//               the batched path uses (1 of 36 lines, against 5 with
//               replicate).
// GRPARSE_OCR_BUCKET_PAD=auto or tile (default: the detector tiled, the
// recognizer constant), replicate or zero (both nets).
enum class OcrPad { kTile, kReplicate, kConstant };
OcrPad ocr_det_pad();
OcrPad ocr_rec_pad();
// How many detector sizes and recognizer widths the default lists hold up to
// the detector's largest side at the default GRPARSE_OCR_MAX_SIDE and a line
// `max_rec_width` wide: the bound on distinct shapes, and so on kernel
// compiles, the buckets promise.
size_t ocr_det_bucket_count();
size_t ocr_rec_bucket_count(int max_rec_width);
// Forgets the resolved bucket settings, so the next call reads the
// environment and the provider selection again.  set_ort_ep_selection calls
// it (auto depends on the provider); tests call it after changing the
// environment.
void reset_ocr_shape_buckets();

// The nets, in pipeline order: text detection, line angle, line recognition.
// grparse_inference_model.h names them for placement lines (ocr_net_name).
enum class OcrNet { kDetection, kDirection, kRecognition };
// "det", "cls" or "rec": the metric label.
std::string_view ocr_net_label(OcrNet net);

// One input tensor shape (NCHW without the fixed channel count) a net ran
// with, and how many runs used it.
struct OcrInputShape {
  OcrNet net = OcrNet::kDetection;
  int batch = 0;
  int height = 0;
  int width = 0;
  uint64_t runs = 0;
};

// Called by the patched nets before every session run.
void note_ocr_input_shape(OcrNet net, int64_t batch, int64_t height, int64_t width);
// Every distinct shape seen since the process started or the ledger was last
// reset, ordered by net, height, width and batch.
std::vector<OcrInputShape> ocr_input_shapes();
// How many distinct shapes one net has run.  On a provider that compiles per
// input shape this is the number of compiles the net has caused; exported as
// grparse_ocr_input_shapes{net}.
size_t ocr_input_shape_count(OcrNet net);
// Empties the ledger; tests call it between documents.
void reset_ocr_input_shapes();

}  // namespace grparse
