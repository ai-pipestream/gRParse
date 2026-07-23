#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "grparse/collector_coordinator.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

grparse::CollectorOutcome text_outcome(const std::string& text) {
  grparse::CollectorOutcome outcome;
  outcome.document = base_document();
  auto* base = outcome.document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text(text);
  outcome.document.mutable_body()->add_children()->set_ref("#/texts/0");
  outcome.success = true;
  return outcome;
}

void verify_routing() {
  require(grparse::office_format("report.docx", ""), "docx routes to office");
  require(grparse::office_format("Sheet.XLSX", ""), "extension match is case-insensitive");
  require(grparse::office_format("data.csv", ""), "csv routes to office");
  require(!grparse::office_format("scan.pdf", ""), "pdf stays on the CV path");
  require(!grparse::office_format("page.png", ""), "images stay on the CV path");
  require(grparse::office_format("upload.bin",
                                 "application/vnd.oasis.opendocument.text"),
          "content type routes when the extension does not");

  auto plan = grparse::resolve_collectors({}, false);
  require(plan.size() == 1 && plan[0] == parsev1::COLLECTOR_GRPARSE_CV,
          "empty selection routes non-office to CV");
  plan = grparse::resolve_collectors({}, true);
  require(plan.size() == 1 && plan[0] == parsev1::COLLECTOR_LIBREOFFICE,
          "empty selection routes office to libreoffice");
  plan = grparse::resolve_collectors(
      {parsev1::COLLECTOR_UNSPECIFIED, parsev1::COLLECTOR_LIBREOFFICE,
       parsev1::COLLECTOR_GRPARSE_CV, parsev1::COLLECTOR_LIBREOFFICE},
      false);
  require(plan.size() == 2 && plan[0] == parsev1::COLLECTOR_LIBREOFFICE &&
              plan[1] == parsev1::COLLECTOR_GRPARSE_CV,
          "explicit selection wins verbatim, deduplicated, order kept");
}

void verify_scatter_gather_merges_additively() {
  std::vector<grparse::PlannedCollector> plan;
  plan.push_back({parsev1::COLLECTOR_GRPARSE_CV,
                  [] { return text_outcome("from cv"); }});
  plan.push_back({parsev1::COLLECTOR_LIBREOFFICE, [] {
                    auto outcome = text_outcome("from libreoffice");
                    outcome.warnings.push_back("office warning");
                    return outcome;
                  }});
  auto result = grparse::run_collectors(std::move(plan), base_document());
  require(result.succeeded == 2 && result.failures.empty(),
          "both collectors contribute");
  require(result.document.texts_size() == 2, "outputs merge into one document");
  require(result.document.texts(0).text().base().text() == "from cv" &&
              result.document.texts(1).text().base().text() == "from libreoffice",
          "merge order follows the plan, not finish order");
  require(result.document.texts(1).text().base().self_ref() == "#/texts/1",
          "second collector's items renumber past the first");
  require(result.warnings.size() == 1 &&
              result.warnings[0].first == parsev1::COLLECTOR_LIBREOFFICE &&
              result.warnings[0].second == "office warning",
          "warnings stay attributed to their collector");
}

void verify_failure_isolation() {
  std::vector<grparse::PlannedCollector> plan;
  plan.push_back({parsev1::COLLECTOR_LIBREOFFICE, [] {
                    grparse::CollectorOutcome outcome;
                    outcome.error = "collector went away";
                    outcome.code = grpc::StatusCode::UNAVAILABLE;
                    return outcome;
                  }});
  plan.push_back({parsev1::COLLECTOR_GRPARSE_CV,
                  [] { return text_outcome("survivor"); }});
  auto result = grparse::run_collectors(std::move(plan), base_document());
  require(result.succeeded == 1, "the surviving collector still lands");
  require(result.document.texts_size() == 1 &&
              result.document.texts(0).text().base().text() == "survivor",
          "the merged document holds the survivor's output");
  require(result.failures.size() == 1 &&
              result.failures[0].id == parsev1::COLLECTOR_LIBREOFFICE &&
              result.failures[0].error == "collector went away" &&
              result.failures[0].code == grpc::StatusCode::UNAVAILABLE,
          "the failed collector degrades to a failure entry");
}

}  // namespace

int main() {
  try {
    verify_routing();
    verify_scatter_gather_merges_additively();
    verify_failure_isolation();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "collector-coordinator-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
