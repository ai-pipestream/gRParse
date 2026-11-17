#include "hailo_placement.h"

#include <array>
#include <stdexcept>
#include <string>

namespace grparse::hailo {
namespace {

// The CPU reasons quote what the Hailo Dataflow Compiler 5.4.0 said for
// hw_arch hailo10h (models/hailo/probe_parse.py; models/hailo/README.md has
// the probe output).
constexpr std::array<PlacementRule, 7> kRules{{
    {ModelId::kOcrDetection, "OCR detection", "det_640.hef", 8, ""},
    {ModelId::kOcrDirection, "OCR direction", "cls_48x192.hef", 8, ""},
    {ModelId::kOcrRecognition, "OCR recognition", "rec_48x320.hef", 16, ""},
    {ModelId::kLayoutAnchorFree, "layout", "layout_800x608.hef", 1, ""},
    {ModelId::kLayoutQuery, "layout", "", 1,
     "the Hailo compiler has no GridSample, GatherElements or TopK, which the query decoder "
     "needs; GRPARSE_LAYOUT_MODEL=picodet runs on the device"},
    {ModelId::kTableStructure, "table structure", "", 1,
     "the Hailo compiler cannot parse the structure decoder's Loop"},
    {ModelId::kFigureClasses, "figure classes", "figure_224.hef", 8, ""},
}};

}  // namespace

std::span<const PlacementRule> placement_rules() { return kRules; }

const PlacementRule& placement_rule(ModelId id) {
  for (const PlacementRule& rule : kRules) {
    if (rule.id == id) return rule;
  }
  throw std::logic_error("no Hailo placement rule for model " + std::to_string(static_cast<int>(id)));
}

std::string_view cpu_pin_reason(ModelId id) {
  const PlacementRule& rule = placement_rule(id);
  if (rule.on_device()) {
    throw std::logic_error(std::string(rule.ledger_name) + " runs on the Hailo device; it has no CPU pin");
  }
  return rule.cpu_reason;
}

}  // namespace grparse::hailo
