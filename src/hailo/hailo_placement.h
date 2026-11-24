#pragma once

// Where each model runs when the Hailo device is selected.  One table, in
// code: a model is on the device with a HEF, or pinned to CPU with the
// reason the compiler gave.  Nothing is placed by default, so a model that
// is not in the table cannot run at all under this selection.
// tests/hailo_placement_test.cpp holds the table to that.

#include <cstdint>
#include <span>
#include <string_view>

namespace grparse::hailo {

enum class ModelId {
  kOcrDetection,
  kOcrDirection,
  kOcrRecognition,
  kLayoutAnchorFree,
  kLayoutQuery,
  kTableStructure,
  kFigureClasses,
};

struct PlacementRule {
  ModelId id;
  // The model's name in the placement ledger and metrics.
  std::string_view ledger_name;
  // File name under the HEF directory; empty for a CPU pin.
  std::string_view hef;
  // Frames per context switch on the device (see HailoModelSpec).
  uint16_t batch_size = 1;
  // Why the model is on CPU; empty for a device model.
  std::string_view cpu_reason;

  bool on_device() const { return !hef.empty(); }
};

std::span<const PlacementRule> placement_rules();

const PlacementRule& placement_rule(ModelId id);

// The reason a CPU-pinned model carries into make_pinned_cpu_session.
// Throws std::logic_error for a device model: it has no CPU session to pin.
std::string_view cpu_pin_reason(ModelId id);

}  // namespace grparse::hailo
