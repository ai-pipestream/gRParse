#include "hailo_models.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse_session_ep.h"
#include "hailo_ocr.h"
#include "hailo_ocr_plan.h"

namespace grparse::hailo {
namespace {

std::mutex models_mutex;
std::map<ModelId, std::unique_ptr<HailoModel>> models;

int dimension(const TensorSpec& spec, size_t index) { return static_cast<int>(spec.shape[index]); }

}  // namespace

HailoModel& shared_model(ModelId id) {
  const std::lock_guard<std::mutex> lock(models_mutex);
  if (const auto found = models.find(id); found != models.end()) return *found->second;
  const PlacementRule& rule = placement_rule(id);
  if (!rule.on_device()) {
    throw std::logic_error(std::string(rule.ledger_name) + " has no HEF: " +
                           std::string(rule.cpu_reason));
  }
  const std::filesystem::path hef =
      std::filesystem::path(ort_ep_selection().hailo_hef_dir) / std::string(rule.hef);
  std::unique_ptr<HailoModel> model = make_hailo_model(
      HailoModelSpec{.hef = hef, .what = std::string(rule.ledger_name), .batch_size = rule.batch_size});
  // Loads every context once, so a HEF the device cannot run fails here, at
  // startup, and the first page pays no setup.
  model->prewarm({});
  record_native_placement(rule.ledger_name);
  return *models.emplace(id, std::move(model)).first->second;
}

namespace {

OcrModels load_ocr_models(size_t dictionary_size) {
  HailoModel& detection = shared_model(ModelId::kOcrDetection);
  HailoModel& direction = shared_model(ModelId::kOcrDirection);
  HailoModel& recognition = shared_model(ModelId::kOcrRecognition);
  const TensorSpec& det_in = detection.shapes().inputs.at(0);
  const TensorSpec& cls_in = direction.shapes().inputs.at(0);
  const TensorSpec& rec_in = recognition.shapes().inputs.at(0);
  const TensorSpec& rec_out = recognition.shapes().outputs.at(0);
  if (dimension(det_in, 0) != dimension(det_in, 1) || dimension(det_in, 2) != 3) {
    throw std::runtime_error("Hailo OCR detection HEF must take a square 3-channel tile");
  }
  if (dimension(cls_in, 0) != kRecHeight || dimension(rec_in, 0) != kRecHeight ||
      dimension(rec_in, 1) % kRecStepPixels != 0) {
    throw std::runtime_error("Hailo OCR direction and recognition HEFs must take 48-pixel-high "
                             "lines, the recognizer at a width that is a multiple of 8");
  }
  OcrShapes shapes{
      .det_tile = dimension(det_in, 0),
      .cls_width = dimension(cls_in, 1),
      .rec_width = dimension(rec_in, 1),
      .rec_classes = dimension(rec_out, 2),
  };
  if (static_cast<size_t>(shapes.rec_classes) != dictionary_size ||
      dimension(rec_out, 1) != shapes.rec_width / kRecStepPixels) {
    throw std::runtime_error(
        "Hailo OCR recognition HEF predicts " + std::to_string(dimension(rec_out, 1)) +
        " steps of " + std::to_string(shapes.rec_classes) + " classes; the dictionary has " +
        std::to_string(dictionary_size) + " and the input width gives " +
        std::to_string(shapes.rec_width / kRecStepPixels) + " steps");
  }
  // The recognizer HEF may end at its softmax or before it (models/hailo/
  // parse.py); one frame of padding grey shows which.
  const std::vector<uint8_t> grey(static_cast<size_t>(kRecHeight) * shapes.rec_width * 3, 127);
  const TensorView probe{TensorElement::kUInt8, grey.data(), {1, kRecHeight, shapes.rec_width, 3}};
  const std::vector<Tensor> answer = recognition.run(std::span(&probe, 1));
  shapes.rec_logits = rows_are_logits(answer.at(0).floats(),
                                      static_cast<size_t>(shapes.rec_width / kRecStepPixels),
                                      static_cast<size_t>(shapes.rec_classes));
  return OcrModels{&detection, &direction, &recognition, shapes};
}

std::mutex ocr_mutex;
std::optional<OcrModels> ocr_models;

}  // namespace

OcrModels shared_ocr_models(size_t dictionary_size) {
  const std::lock_guard<std::mutex> lock(ocr_mutex);
  if (!ocr_models) ocr_models = load_ocr_models(dictionary_size);
  if (static_cast<size_t>(ocr_models->shapes.rec_classes) != dictionary_size) {
    throw std::runtime_error("Hailo OCR: the dictionary changed size after the HEFs were loaded");
  }
  return *ocr_models;
}

}  // namespace grparse::hailo
