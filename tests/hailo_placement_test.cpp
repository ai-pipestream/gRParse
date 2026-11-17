// Placement policy under the Hailo selection: the table that says which
// model is on the device and which is pinned to CPU and why, and the session
// builder's refusal to place anything the table did not.  No device needed.

#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "../src/hailo/hailo_metrics.h"
#include "../src/hailo/hailo_placement.h"
#include "../src/hailo/hailo_runtime.h"
#include "grparse_session_ep.h"
#include "support/check.h"
#include "support/tiny_onnx_model.h"

namespace {

using grparse::hailo::ModelId;
using grparse_test::require;
using grparse_test::require_equal;

template <typename Call>
std::string error_of(Call&& call) {
  try {
    call();
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

// A device model has a HEF and no reason; a CPU model has a reason and no
// HEF; there is no third kind.
void verify_every_rule_is_a_hef_or_a_reason() {
  std::set<ModelId> seen;
  for (const auto& rule : grparse::hailo::placement_rules()) {
    require(seen.insert(rule.id).second, "one rule per model");
    require(!rule.ledger_name.empty(), "every rule names its model");
    require(rule.batch_size >= 1, "a batch holds at least one frame");
    if (rule.on_device()) {
      require(rule.hef.ends_with(".hef"), "a device model names a HEF file");
      require(rule.cpu_reason.empty(), "a device model carries no CPU reason");
    } else {
      require(rule.cpu_reason.size() >= 20,
              "a CPU pin says why in words, not in a word: " + std::string(rule.ledger_name));
    }
  }
  require_equal(seen.size(), size_t{7}, "the table covers every image model");
}

// The names are the ledger's: the golden placement series is keyed on them.
void verify_the_table_places_each_model() {
  using grparse::hailo::placement_rule;
  require(placement_rule(ModelId::kOcrDetection).on_device(), "OCR detection is on the device");
  require(placement_rule(ModelId::kOcrDirection).on_device(), "OCR direction is on the device");
  require(placement_rule(ModelId::kOcrRecognition).on_device(), "OCR recognition is on the device");
  require(placement_rule(ModelId::kLayoutAnchorFree).on_device(),
          "the anchor-free layout detector is on the device");
  require(placement_rule(ModelId::kFigureClasses).on_device(), "figure classes are on the device");
  require(!placement_rule(ModelId::kLayoutQuery).on_device(), "the query layout detector is on CPU");
  require(!placement_rule(ModelId::kTableStructure).on_device(), "table structure is on CPU");
  require_equal(placement_rule(ModelId::kOcrDetection).ledger_name, "OCR detection", "ledger name");
  require_equal(placement_rule(ModelId::kOcrDirection).ledger_name, "OCR direction", "ledger name");
  require_equal(placement_rule(ModelId::kOcrRecognition).ledger_name, "OCR recognition", "ledger name");
  require_equal(placement_rule(ModelId::kLayoutAnchorFree).ledger_name, "layout", "ledger name");
  require_equal(placement_rule(ModelId::kLayoutQuery).ledger_name, "layout", "ledger name");
  require_equal(placement_rule(ModelId::kTableStructure).ledger_name, "table structure", "ledger name");
  require_equal(placement_rule(ModelId::kFigureClasses).ledger_name, "figure classes", "ledger name");
}

void verify_a_device_model_has_no_cpu_pin() {
  const std::string error =
      error_of([] { (void)grparse::hailo::cpu_pin_reason(ModelId::kOcrRecognition); });
  require(error.contains("runs on the Hailo device"),
          "asking a device model for a CPU reason is an error, got: " + error);
  require(!grparse::hailo::cpu_pin_reason(ModelId::kTableStructure).empty(),
          "a CPU model hands over its reason");
}

void select_hailo() {
  grparse::set_ort_ep_selection(grparse::OrtEpSelection{
      .ep = grparse::OrtEp::kHailo, .hailo_device = "HAILO10H", .hailo_hef_dir = "/nonexistent"});
  grparse::reset_ort_placements();
}

// Strict placement: with the device selected, the session builders build
// nothing on their own.  A model that is not pinned fails, counted, and no
// CPU session appears in the ledger.
void verify_no_session_is_built_for_an_unplaced_model() {
  select_hailo();
  require_equal(grparse::ort_ep_name(grparse::OrtEp::kHailo), "Hailo", "the provider's name");
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "hailo-placement-test");
  const std::filesystem::path model = grparse_test::write_tiny_model();
  const uint64_t refusals = grparse::ep_fallback_count();
  const std::string error =
      error_of([&] { (void)grparse::make_session(env, model, "unplaced model"); });
  require(error.contains("the Hailo execution provider (HAILO10H) refused"),
          "an unplaced model is refused by name, got: " + error);
  require(error.contains("needs a HEF or a CPU pin"), "the refusal says what is missing: " + error);
  require_equal(grparse::ep_fallback_count(), refusals + 1, "the refusal is counted");
  const std::string ocr_error = error_of([&] {
    delete grparse::make_rapidocr_session(env, model, "unplaced OCR net", 1);
  });
  require(ocr_error.contains("refused"), "the OCR session builder refuses too: " + ocr_error);
  require(grparse::ort_placements().empty(), "a refused model leaves no placement behind");
  std::filesystem::remove(model);
}

void verify_a_pin_and_a_device_model_are_recorded() {
  select_hailo();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "hailo-placement-test");
  const std::filesystem::path model = grparse_test::write_tiny_model();
  (void)grparse::make_pinned_cpu_session(
      env, model, "table structure",
      grparse::hailo::cpu_pin_reason(ModelId::kTableStructure));
  grparse::record_native_placement("OCR detection");
  grparse::record_native_placement("OCR detection");
  const auto placements = grparse::ort_placements();
  require_equal(placements.size(), size_t{2}, "one entry per model");
  require(placements[0].requested == grparse::OrtEp::kHailo &&
              placements[0].actual == grparse::OrtEp::kCpu && placements[0].device.empty(),
          "the pin is requested Hailo, actual CPU");
  require_equal(placements[0].pin_reason, grparse::hailo::cpu_pin_reason(ModelId::kTableStructure),
                "the pin carries the table's reason");
  require(placements[1].requested == grparse::OrtEp::kHailo &&
              placements[1].actual == grparse::OrtEp::kHailo,
          "a device model is requested Hailo, actual Hailo");
  require_equal(placements[1].device, "HAILO10H", "with the device as it identified itself");
  require_equal(placements[1].sessions, uint64_t{2}, "and one count per load");
  require(placements[1].pin_reason.empty(), "a device model has no pin");
  std::filesystem::remove(model);
}

void verify_metrics_render() {
  require(grparse::hailo::render_hailo_metrics({}).empty(), "no device, no series");
  const std::string text = grparse::hailo::render_hailo_metrics(
      {{"OCR detection", 12, 0}, {"OCR recognition", 340, 2}});
  require(text.contains("grparse_hailo_frames_total{model=\"OCR detection\"} 12\n"),
          "frames per model: " + text);
  require(text.contains("grparse_hailo_frames_total{model=\"OCR recognition\"} 340\n"),
          "frames per model: " + text);
  require(text.contains("grparse_hailo_model_reloads_total{model=\"OCR detection\"} 0\n"),
          "a healthy model was never reloaded: " + text);
  require(text.contains("grparse_hailo_model_reloads_total{model=\"OCR recognition\"} 2\n"),
          "reloads after device failures are counted: " + text);
  require(!text.contains("device_ram") && !text.contains("utilization"),
          "the chip's own statistics are not queried or rendered: " + text);
}

// In a build without HailoRT the device can be named but never opened.
void verify_a_build_without_hailort_cannot_open_the_device() {
  if (grparse::hailo::runtime_built()) return;
  const std::string error = error_of([] { (void)grparse::hailo::open_device(); });
  require(error.contains("this build has no HailoRT"),
          "opening the device without HailoRT says so, got: " + error);
  require(grparse::hailo::model_frame_counts().empty(), "and no frames");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("hailo-placement-test", "the device or a reason, never neither", {
      verify_every_rule_is_a_hef_or_a_reason,
      verify_the_table_places_each_model,
      verify_a_device_model_has_no_cpu_pin,
      verify_no_session_is_built_for_an_unplaced_model,
      verify_a_pin_and_a_device_model_are_recorded,
      verify_metrics_render,
      verify_a_build_without_hailort_cannot_open_the_device,
  });
}
