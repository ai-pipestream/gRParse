#include "grparse/collector_coordinator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <utility>

#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(),
                         [](unsigned char letter) { return std::tolower(letter); });
  return value;
}

bool extension_in(const std::string& extension,
                  std::initializer_list<const char*> known) {
  return std::ranges::any_of(
      known, [&extension](const char* candidate) { return extension == candidate; });
}

// True when `value` ends with `suffix`. Used for the double extensions
// (.tar.gz) that std::filesystem's single-extension accessor cannot see.
bool ends_with(const std::string& value, const std::string& suffix) {
  return value.ends_with(suffix);
}

}  // namespace

CollectorDeadline capped_collector_deadline(CollectorDeadline inbound,
                                            std::chrono::system_clock::duration cap) {
  // now() + cap cannot overflow for the caps this repo uses (minutes), and
  // kNoCollectorDeadline is time_point::max(), so the unset case falls out
  // of the min without a branch of its own.
  return std::min(inbound, std::chrono::system_clock::now() + cap);
}

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
    // The collector's document-level account is kept whole and its
    // answers are attributed, so a value another collector displaces is
    // still on the wire under the collector that gave it.
    pipestream::document::v1::CollectorSource claimant;
    claimant.set_collector(collector_name(collectors[index].id));
    merge_documents(std::move(outcome.document), &result.document, claimant);
    ++result.succeeded;
  }
  return result;
}

bool office_format(const std::string& filename, const std::string& content_type) {
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  if (extension_in(extension,
                   {".doc", ".docx", ".docm", ".dot", ".dotx", ".odt", ".ott",
                    ".fodt", ".rtf", ".xls", ".xlsx", ".xlsm", ".xlsb", ".ods",
                    ".ots", ".fods", ".csv", ".ppt", ".pptx", ".pptm", ".odp",
                    ".otp", ".fodp"})) {
    return true;
  }
  const std::string type = lowercase(content_type);
  return type.contains("officedocument") || type.contains("msword") ||
         type.contains("ms-excel") || type.contains("ms-powerpoint") ||
         type.contains("opendocument") ||
         type == "text/csv" || type == "application/rtf";
}

bool poi_format(const std::string& filename, const std::string& content_type) {
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  // Exactly grPOIc's six DocumentFormat values; the macro/template and ODF
  // siblings stay with libreoffice alone.
  if (extension_in(extension,
                   {".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx"})) {
    return true;
  }
  const std::string type = lowercase(content_type);
  return type.contains("officedocument") || type.contains("msword") ||
         type.contains("ms-excel") || type.contains("ms-powerpoint");
}

bool calamine_workbook_format(const std::string& filename, const std::string& content_type) {
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  if (extension_in(extension, {".xls", ".xlsx", ".xlsm", ".xlsb", ".ods"})) {
    return true;
  }
  const std::string type = lowercase(content_type);
  return type.contains("ms-excel") || type.contains("spreadsheetml") ||
         type == "application/vnd.oasis.opendocument.spreadsheet";
}

void append_office_fanout(std::vector<pipestream::parse::v1::Collector>* plan,
                          const std::string& filename, const std::string& content_type,
                          bool poi_configured, bool calamine_configured) {
  const auto missing = [plan](pipestream::parse::v1::Collector id) {
    return std::ranges::find(*plan, id) == plan->end();
  };
  if (poi_configured && poi_format(filename, content_type) &&
      missing(pipestream::parse::v1::COLLECTOR_POI)) {
    plan->push_back(pipestream::parse::v1::COLLECTOR_POI);
  }
  if (calamine_configured && calamine_workbook_format(filename, content_type) &&
      missing(pipestream::parse::v1::COLLECTOR_CALAMINE)) {
    plan->push_back(pipestream::parse::v1::COLLECTOR_CALAMINE);
  }
}

pipestream::parse::v1::Collector route_collector(const std::string& filename,
                                                 const std::string& content_type) {
  if (office_format(filename, content_type)) {
    return pipestream::parse::v1::COLLECTOR_LIBREOFFICE;
  }
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  const std::string type = lowercase(content_type);
  // WARC before everything content-sniffable: the archive wraps HTTP
  // responses whose bodies are html/xml/json, and routing on the wrapper's
  // own name keeps the container with the collector that unpacks it.
  if (extension == ".warc" || ends_with(lowercase(filename), ".warc.gz") ||
      ends_with(lowercase(filename), ".warc.zst") ||
      ends_with(lowercase(filename), ".warc.lz4") || type == "application/warc") {
    return pipestream::parse::v1::COLLECTOR_FASTWARC;
  }
  // The wiki storage dialect before the markup route: its ".storage.xhtml"
  // suffix ends in an extension the markup collector otherwise claims, and
  // the macro layer is exactly what routing there would lose.
  if (confluence_storage_format(filename, content_type)) {
    return pipestream::parse::v1::COLLECTOR_CONFLUENCE;
  }
  if (extension == ".epub" || type == "application/epub+zip") {
    return pipestream::parse::v1::COLLECTOR_EPUB;
  }
  if (extension_in(extension, {".eml", ".msg"}) || type == "message/rfc822" ||
      type == "application/vnd.ms-outlook") {
    return pipestream::parse::v1::COLLECTOR_EMAIL;
  }
  // Deliberately narrow: only names that say "XML document", never the
  // "+xml" suffix family (xhtml, svg, ...), whose members belong to other
  // collectors or to none. The archive forms are the xml collector's too:
  // a .dclx is a zip wrapping a doclang document, and a .tar.gz is routed
  // for the Google Books METS export it may be; the collector rejects a
  // tarball that is not one, and that failure degrades per-collector.
  if (extension_in(extension, {".xml", ".nxml", ".xbrl", ".dclx"}) ||
      ends_with(lowercase(filename), ".tar.gz") ||
      type == "application/xml" || type == "text/xml" ||
      type == "application/mets+xml") {
    return pipestream::parse::v1::COLLECTOR_XML;
  }
  if (markup_format_for(filename, content_type) !=
      pipestream::markup::v1::MARKUP_FORMAT_UNSPECIFIED) {
    return pipestream::parse::v1::COLLECTOR_MARKUP;
  }
  if (extension_in(extension,
                   {".mp3", ".wav", ".m4a", ".aac", ".flac", ".ogg", ".oga",
                    ".opus", ".wma", ".amr", ".mp4", ".m4v", ".mkv", ".webm",
                    ".mov", ".avi", ".mpg", ".mpeg", ".wmv"}) ||
      type.starts_with("audio/") || type.starts_with("video/")) {
    return pipestream::parse::v1::COLLECTOR_ASR;
  }
  return pipestream::parse::v1::COLLECTOR_GRPARSE_CV;
}

pipestream::parse::v1::Collector route_document(const std::string& filename,
                                                 const std::string& declared_content_type,
                                                 std::string_view bytes) {
  // A name that declares a type keeps its say: "image.png" holding text
  // bytes is a bad image, not a markup document. Only a name that declares
  // nothing (no extension, an unknown one) lets the bytes route.
  const std::filesystem::path path(filename);
  if (!declared_content_type.empty() || extension_mimetype(path) != "application/octet-stream") {
    return route_collector(filename, declared_content_type);
  }
  const MimetypeResolution resolved = resolve_mimetype(declared_content_type, bytes, path);
  const bool informative = resolved.mimetype != "application/octet-stream";
  return route_collector(filename, informative ? resolved.mimetype : declared_content_type);
}

pipestream::markup::v1::MarkupFormat markup_format_for(
    const std::string& filename, const std::string& content_type) {
  const std::string extension =
      lowercase(std::filesystem::path(filename).extension().string());
  const std::string type = lowercase(content_type);
  if (extension_in(extension, {".md", ".markdown", ".mdown"}) ||
      type == "text/markdown" || type == "text/x-markdown") {
    return pipestream::markup::v1::MARKUP_FORMAT_MARKDOWN;
  }
  if (extension_in(extension, {".html", ".htm", ".xhtml"}) ||
      type == "text/html" || type == "application/xhtml+xml") {
    return pipestream::markup::v1::MARKUP_FORMAT_HTML;
  }
  if (extension_in(extension, {".adoc", ".asciidoc"}) ||
      type == "text/asciidoc" || type == "text/x-asciidoc") {
    return pipestream::markup::v1::MARKUP_FORMAT_ASCIIDOC;
  }
  if (extension_in(extension, {".tex", ".latex"}) || type == "text/x-tex" ||
      type == "application/x-tex" || type == "text/x-latex") {
    return pipestream::markup::v1::MARKUP_FORMAT_LATEX;
  }
  if (extension == ".vtt" || type == "text/vtt") {
    return pipestream::markup::v1::MARKUP_FORMAT_VTT;
  }
  if (extension == ".boxnote") {
    return pipestream::markup::v1::MARKUP_FORMAT_BOXNOTE;
  }
  // Bare JSON is routed the way docling routes it: to the Docling JSON
  // reader, which validates and rejects a payload that is not a serialized
  // DoclingDocument. That rejection degrades per-collector rather than
  // being guessed around here.
  if (extension == ".json" || type == "application/json") {
    return pipestream::markup::v1::MARKUP_FORMAT_DOCLING_JSON;
  }
  // Plain text last, after every format a name or type names outright: it
  // is read as Markdown, of which it is the trivial case.
  if (extension == ".txt" || type == "text/plain") {
    return pipestream::markup::v1::MARKUP_FORMAT_MARKDOWN;
  }
  return pipestream::markup::v1::MARKUP_FORMAT_UNSPECIFIED;
}

std::vector<pipestream::parse::v1::Collector> resolve_collectors(
    const std::vector<pipestream::parse::v1::Collector>& requested,
    pipestream::parse::v1::Collector routed) {
  std::vector<pipestream::parse::v1::Collector> plan;
  for (const auto collector : requested) {
    if (collector == pipestream::parse::v1::COLLECTOR_UNSPECIFIED) continue;
    if (std::ranges::find(plan, collector) == plan.end()) {
      plan.push_back(collector);
    }
  }
  if (plan.empty()) plan.push_back(routed);
  return plan;
}

}  // namespace grparse
