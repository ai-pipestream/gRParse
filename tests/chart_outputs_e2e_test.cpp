// End to end proof of Docling chart2summary and chart2code through a real
// grpc-enrich: a ConvertDocumentOptions naming a typed admin preset
// (ChartExtractionPresetSpec with chart2csv, chart2summary and chart2code)
// resolves through the chart-extraction policy, the derender leg dials the
// grpc-enrich at GRPARSE_E2E_ENRICH_TARGET with the chart PNG at
// GRPARSE_E2E_CHART_PNG, and the table, summary and code the VLM behind it
// answers land in the picture's meta.tabular_chart, meta.description and
// meta.code. Skips (exit 77) when the variables are unset, so the unit
// suite stays hermetic; scripts/test/chart-outputs-e2e-test.sh starts the
// backends and runs it.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

#include <grpcpp/grpcpp.h>

#include "../src/source_parse.h"
#include "grparse/base64.h"
#include "grparse/chart_derender.h"
#include "grparse/chart_extraction_policy.h"
#include "support/check.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = pipestream::parse::v1;
using grparse_test::require;

const char* variable(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? value : nullptr;
}

docv1::Document chart_document(const std::string& png) {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.set_name("chart.png");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  docv1::PictureItem* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  document.mutable_body()->add_children()->set_ref(picture->self_ref());
  auto* classification = picture->add_annotations()->mutable_classification();
  classification->set_kind("classification");
  classification->set_provenance("figure-classifier");
  auto* top = classification->add_predicted_classes();
  top->set_class_name("bar_chart");
  top->set_confidence(0.95);
  picture->mutable_image()->set_mimetype("image/png");
  picture->mutable_image()->set_uri("data:image/png;base64," +
                                    grparse::encode_base64(png.data(), png.size()));
  return document;
}

}  // namespace

int main() {
  const char* target = variable("GRPARSE_E2E_ENRICH_TARGET");
  const char* png_path = variable("GRPARSE_E2E_CHART_PNG");
  if (target == nullptr || png_path == nullptr) {
    std::println("chart-outputs-e2e-test: skipped (GRPARSE_E2E_ENRICH_TARGET and "
                 "GRPARSE_E2E_CHART_PNG unset)");
    return 77;
  }
  return grparse_test::run_test_main("chart-outputs-e2e-test", "all checks passed", {[&] {
    std::ifstream in(png_path, std::ios::binary);
    const std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(png.size() > 8 && png.starts_with("\x89PNG"), "the chart fixture is a PNG");

    const std::string presets =
        R"({"e2e":{"model":"granite-vision-e2e","engine_type":"VLM_ENGINE_TYPE_API_OPENAI",)"
        R"("chart2csv":true,"chart2summary":true,"chart2code":true}})";
    const grparse::ChartExtractionPolicy policy =
        grparse::chart_extraction_policy_from_env([&](const char* name) -> const char* {
          if (std::string(name) == "GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS") {
            return presets.c_str();
          }
          return nullptr;
        });
    parsev1::ConvertDocumentOptions options;
    options.set_do_chart_extraction(true);
    options.set_chart_extraction_preset("e2e");
    const auto resolved = grparse::resolve_chart_extraction(options, policy, "e2e");
    require(resolved.has_value() && resolved->has_value(),
            "the typed preset resolves from the request");
    require((*resolved)->chart2csv && (*resolved)->chart2summary && (*resolved)->chart2code,
            "all three outputs are on");

    grparse::ChartDerenderOptions derender;
    derender.target = target;
    derender.timeout = std::chrono::milliseconds(60000);
    derender.chart_extraction = **resolved;
    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    docv1::Document document = chart_document(png);
    const grparse::ChartDerenderReport report =
        grparse::derender_charts(channel, derender, &document);
    for (const std::string& warning : report.warnings) std::println("warning: {}", warning);
    std::println("report: candidates={} derendered={} tables={} summaries={} codes={} skipped={}",
                 report.candidates, report.derendered, report.chart_tables,
                 report.chart_summaries, report.chart_codes, report.skipped);
    require(report.candidates == 1 && report.derendered == 1 && report.chart_tables == 1 &&
                report.chart_summaries == 1 && report.chart_codes == 1 && report.skipped == 0,
            "the real grpc-enrich answers table, summary and code for the chart");

    const docv1::PictureItem& picture = document.pictures(0);
    std::println("meta.description: {}", picture.meta().description().text());
    std::println("meta.code ({}): {}", docv1::CodeLanguageLabel_Name(picture.meta().code().language()),
                 picture.meta().code().text());
    require(picture.meta().description().text() ==
                "Revenue is highest in the North and lowest in the South.",
            "chart2summary lands in meta.description");
    require(picture.meta().description().created_by() == "granite-vision-e2e",
            "the summary is attributed to the preset's model");
    require(picture.meta().code().language() == docv1::CODE_LANGUAGE_LABEL_PYTHON,
            "chart2code is Python");
    require(picture.meta().code().text().contains("plt.bar(") &&
                !picture.meta().code().text().contains("```"),
            "chart2code lands in meta.code without the reply's fence");
    require(picture.meta().has_tabular_chart(), "chart2csv lands in meta.tabular_chart");
  }});
}
