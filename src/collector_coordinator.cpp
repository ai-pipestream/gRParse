#include "grparse/collector_coordinator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
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

bool extension_in(const std::string& extension,
                  std::initializer_list<const char*> known) {
  return std::any_of(known.begin(), known.end(),
                     [&extension](const char* candidate) { return extension == candidate; });
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

pipestream::parse::v1::Collector route_collector(const std::string& filename,
                                                 const std::string& content_type) {
  if (office_format(filename, content_type)) {
    return pipestream::parse::v1::COLLECTOR_LIBREOFFICE;
  }
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  const std::string type = lowercase(content_type);
  if (extension == ".epub" || type == "application/epub+zip") {
    return pipestream::parse::v1::COLLECTOR_EPUB;
  }
  if (extension_in(extension, {".eml", ".msg"}) || type == "message/rfc822" ||
      type == "application/vnd.ms-outlook") {
    return pipestream::parse::v1::COLLECTOR_EMAIL;
  }
  // Deliberately narrow: only names that say "XML document", never the
  // "+xml" suffix family (xhtml, svg, ...), whose members belong to other
  // collectors or to none.
  if (extension_in(extension, {".xml", ".nxml", ".xbrl"}) ||
      type == "application/xml" || type == "text/xml") {
    return pipestream::parse::v1::COLLECTOR_XML;
  }
  if (extension_in(extension,
                   {".mp3", ".wav", ".m4a", ".aac", ".flac", ".ogg", ".oga",
                    ".opus", ".wma", ".amr", ".mp4", ".m4v", ".mkv", ".webm",
                    ".mov", ".avi", ".mpg", ".mpeg", ".wmv"}) ||
      type.rfind("audio/", 0) == 0 || type.rfind("video/", 0) == 0) {
    return pipestream::parse::v1::COLLECTOR_ASR;
  }
  return pipestream::parse::v1::COLLECTOR_GRPARSE_CV;
}

std::vector<pipestream::parse::v1::Collector> resolve_collectors(
    const std::vector<pipestream::parse::v1::Collector>& requested,
    pipestream::parse::v1::Collector routed) {
  std::vector<pipestream::parse::v1::Collector> plan;
  for (const auto collector : requested) {
    if (collector == pipestream::parse::v1::COLLECTOR_UNSPECIFIED) continue;
    if (std::find(plan.begin(), plan.end(), collector) == plan.end()) {
      plan.push_back(collector);
    }
  }
  if (plan.empty()) plan.push_back(routed);
  return plan;
}

}  // namespace grparse
