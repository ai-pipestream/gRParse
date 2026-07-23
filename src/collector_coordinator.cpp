#include "grparse/collector_coordinator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>
#include <utility>

#include "grparse/document_merge.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char letter) { return std::tolower(letter); });
  return value;
}

}  // namespace

CoordinatorResult run_collectors(std::vector<PlannedCollector> collectors,
                                 pipestream::document::v1::Document base) {
  CoordinatorResult result;
  result.document = std::move(base);

  std::vector<std::future<CollectorOutcome>> outcomes;
  outcomes.reserve(collectors.size());
  for (auto& collector : collectors) {
    outcomes.push_back(std::async(std::launch::async, collector.run));
  }
  // The merge happens in plan order once everything has arrived, so the
  // merged arena numbering is deterministic regardless of finish order.
  for (size_t index = 0; index < collectors.size(); ++index) {
    CollectorOutcome outcome = outcomes[index].get();
    for (auto& warning : outcome.warnings) {
      result.warnings.emplace_back(collectors[index].id, std::move(warning));
    }
    if (!outcome.success) {
      result.failures.push_back(
          {collectors[index].id, std::move(outcome.error), outcome.code});
      continue;
    }
    merge_documents(std::move(outcome.document), &result.document);
    ++result.succeeded;
  }
  return result;
}

bool office_format(const std::string& filename, const std::string& content_type) {
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  for (const char* known :
       {".doc", ".docx", ".docm", ".dot", ".dotx", ".odt", ".ott", ".fodt",
        ".rtf", ".xls", ".xlsx", ".xlsm", ".xlsb", ".ods", ".ots", ".fods",
        ".csv", ".ppt", ".pptx", ".pptm", ".odp", ".otp", ".fodp"}) {
    if (extension == known) return true;
  }
  const std::string type = lowercase(content_type);
  return type.find("officedocument") != std::string::npos ||
         type.find("msword") != std::string::npos ||
         type.find("ms-excel") != std::string::npos ||
         type.find("ms-powerpoint") != std::string::npos ||
         type.find("opendocument") != std::string::npos ||
         type == "text/csv" || type == "application/rtf";
}

std::vector<pipestream::parse::v1::Collector> resolve_collectors(
    const std::vector<pipestream::parse::v1::Collector>& requested,
    bool office) {
  std::vector<pipestream::parse::v1::Collector> plan;
  for (const auto collector : requested) {
    if (collector == pipestream::parse::v1::COLLECTOR_UNSPECIFIED) continue;
    if (std::find(plan.begin(), plan.end(), collector) == plan.end()) {
      plan.push_back(collector);
    }
  }
  if (plan.empty()) {
    plan.push_back(office ? pipestream::parse::v1::COLLECTOR_LIBREOFFICE
                          : pipestream::parse::v1::COLLECTOR_GRPARSE_CV);
  }
  return plan;
}

}  // namespace grparse
