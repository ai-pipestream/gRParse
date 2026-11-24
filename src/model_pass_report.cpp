#include "model_pass_report.h"

#include "option_rejection.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

void PassDemand::note(const OcrPage& page) {
  for (const LayoutRegion& region : page.regions) {
    tables = tables || region.label == "table";
    pictures = pictures || region.label == "picture";
  }
}

std::vector<Warning> disabled_pass_warnings(const PageScheduler::ModelPasses& passes,
                                            const PageScheduler::OcrTuning& tuning,
                                            const PassDemand& demand) {
  std::vector<Warning> warnings;
  if (!passes.layout) {
    warnings.emplace_back(
        docv1::WARNING_CODE_LAYOUT_PASS_DISABLED,
        "layout pass disabled: this deployment runs without the layout model "
        "(GRPARSE_LAYOUT=off), so no region of the document carries a model-assigned label: "
        "no tables, pictures, headings or formulas were told apart from plain text");
    return warnings;
  }
  if (!passes.table_structure && demand.tables && tuning.do_table_structure.value_or(true)) {
    warnings.emplace_back(
        docv1::WARNING_CODE_TABLE_STRUCTURE_PASS_DISABLED,
        "table-structure pass disabled: this deployment runs without the table-structure "
        "model (GRPARSE_TABLE_STRUCTURE=off), so table cells come from line geometry alone");
  }
  if (!passes.figure_classification && demand.pictures &&
      tuning.do_picture_classification.value_or(true)) {
    warnings.emplace_back(
        docv1::WARNING_CODE_FIGURE_CLASSIFICATION_PASS_DISABLED,
        "figure classification pass disabled: this deployment runs without the figure "
        "classifier (GRPARSE_FIGURE_CLASSES=off), so pictures carry no class");
  }
  return warnings;
}

grpc::Status refuse_disabled_passes(const parsev1::ConvertDocumentOptions& options,
                                    const PageScheduler::ModelPasses& passes,
                                    const std::string& surface) {
  // The same reason extract_form_fields gets when no PDF backend reads
  // widgets: the option names work this deployment has no engine for.
  const auto refused = [&](const char* option, const char* model, const char* setting) {
    return option_rejected(
        option, parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED, "true",
        surface + ": " + option + " asks for the " + model +
            ", which this deployment runs without (" + setting +
            (passes.layout ? "=off" : "=off, or GRPARSE_LAYOUT=off, which takes away the "
                                      "regions it works on") +
            "); leave the option unset to take the document without it");
  };
  if (options.has_do_table_structure() && options.do_table_structure() &&
      !passes.table_structure) {
    return refused("do_table_structure", "table-structure model", "GRPARSE_TABLE_STRUCTURE");
  }
  if (options.has_do_picture_classification() && options.do_picture_classification() &&
      !passes.figure_classification) {
    return refused("do_picture_classification", "figure classifier", "GRPARSE_FIGURE_CLASSES");
  }
  return grpc::Status::OK;
}

}  // namespace grparse
