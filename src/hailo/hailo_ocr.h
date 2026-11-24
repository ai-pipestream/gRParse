#pragma once

// The OCR page pipeline on fixed-shape device models: the detector reads the
// page as overlapping square tiles whose probability maps are joined before
// boxes are cut, the direction net reads each crop at its one size, and the
// recognizer reads a line as overlapping windows whose time steps are joined
// before decoding (hailo_ocr_plan.h has the arithmetic).  Everything around
// the three inference calls is the CPU path's own code: RapidOCR's scale
// rule, box extraction, crop rotation and dictionary.
//
// The models are InferenceModel, so tests drive the pipeline with fakes and
// no device (tests/hailo_ocr_test.cpp).

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "OcrStruct.h"
#include "grparse_inference_model.h"

namespace grparse {
struct OcrDetectOptions;
}

namespace grparse::hailo {

// The fixed sizes the three HEFs were compiled for.
struct OcrShapes {
  int det_tile = 640;     // the detector's square input side
  int cls_width = 192;    // direction net input width, height 48
  int rec_width = 320;    // recognizer input width, height 48
  int rec_classes = 0;    // recognizer output classes per time step
};

// Not owning: the models outlive every pipeline that uses them.
struct OcrModels {
  InferenceModel* detection = nullptr;
  InferenceModel* direction = nullptr;
  InferenceModel* recognition = nullptr;
  OcrShapes shapes;
};

// Neighbouring detector tiles share this many pixels, so a line cut by a
// tile edge is whole in the kept part of one of the two.
inline constexpr int kDetOverlapPixels = 64;
// Neighbouring recognizer windows share this many time steps (64 pixels),
// room for the seam to fall between two glyphs.
inline constexpr int kRecOverlapSteps = 8;

class HailoOcr final {
 public:
  // `keys` is the dictionary as RapidOCR lays it out: the blank first, the
  // space last.  Throws std::invalid_argument when a model is missing or the
  // recognizer's class count is not the dictionary's size.
  HailoOcr(OcrModels models, std::vector<std::string> keys);

  // What OcrLite::detect answers, box points in `image` coordinates.
  std::vector<TextBlock> detect(const cv::Mat& image, const OcrDetectOptions& options);

 private:
  cv::Mat probability_map(const cv::Mat& resized);
  std::vector<Angle> angles(const std::vector<cv::Mat>& crops, bool do_angle, bool most_angle);
  std::vector<TextLine> read_lines(const std::vector<cv::Mat>& crops);

  OcrModels models_;
  std::vector<std::string> keys_;
};

// The dictionary file with RapidOCR's two additions.  Throws
// std::runtime_error when the file cannot be read.
std::vector<std::string> load_ocr_keys(const std::filesystem::path& keys_file);

// The three OCR HEFs of the selection's HEF directory, loaded once per process on the open
// device, checked against the dictionary size, warmed, and recorded in the
// placement ledger.  Throws when a HEF is missing, does not load, or has a
// shape this pipeline cannot use.
OcrModels shared_ocr_models(size_t dictionary_size);

}  // namespace grparse::hailo
