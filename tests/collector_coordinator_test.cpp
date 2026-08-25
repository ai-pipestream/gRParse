#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include <grpcpp/client_context.h>

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

  require(grparse::route_collector("report.docx", "") == parsev1::COLLECTOR_LIBREOFFICE,
          "office formats route to libreoffice");
  require(grparse::route_collector("scan.pdf", "") == parsev1::COLLECTOR_GRPARSE_CV,
          "pdf routes to the CV path");
  require(grparse::route_collector("page.png", "") == parsev1::COLLECTOR_GRPARSE_CV,
          "raster routes to the CV path");
  require(grparse::route_collector("book.epub", "") == parsev1::COLLECTOR_EPUB,
          "epub routes to the epub collector");
  require(grparse::route_collector("upload.bin", "application/epub+zip") ==
              parsev1::COLLECTOR_EPUB,
          "epub content type routes without the extension");
  require(grparse::route_collector("thread.EML", "") == parsev1::COLLECTOR_EMAIL,
          "eml routes to the email collector, case-insensitively");
  require(grparse::route_collector("note.msg", "") == parsev1::COLLECTOR_EMAIL,
          "msg routes to the email collector");
  require(grparse::route_collector("upload.bin", "message/rfc822") ==
              parsev1::COLLECTOR_EMAIL,
          "rfc822 content type routes to the email collector");
  require(grparse::route_collector("article.xml", "") == parsev1::COLLECTOR_XML,
          "xml routes to the xml collector");
  require(grparse::route_collector("paper.nxml", "") == parsev1::COLLECTOR_XML,
          "nxml routes to the xml collector");
  require(grparse::route_collector("upload.bin", "text/xml") == parsev1::COLLECTOR_XML,
          "xml content type routes to the xml collector");
  require(grparse::route_collector("page.xhtml", "") == parsev1::COLLECTOR_MARKUP,
          "the +xml suffix family belongs to the markup collector, not xml");
  require(grparse::route_collector("handbook.confluence", "") ==
              parsev1::COLLECTOR_CONFLUENCE,
          "the wiki storage suffix routes to the in-process storage handler");
  require(grparse::route_collector("handbook.storage.xhtml", "") ==
              parsev1::COLLECTOR_CONFLUENCE,
          "the storage double suffix outranks the markup route its extension "
          "would otherwise take");
  require(grparse::route_collector(
              "upload.bin",
              "application/vnd.atlassian.confluence.storage+xhtml") ==
              parsev1::COLLECTOR_CONFLUENCE,
          "the storage content type routes to the storage handler");
  require(grparse::route_collector("archive.dclx", "") == parsev1::COLLECTOR_XML,
          "the doclang archive routes to the xml collector");
  require(grparse::route_collector("BOOK.TAR.GZ", "") == parsev1::COLLECTOR_XML,
          "a tar.gz routes to the xml collector for its METS export sniff");
  require(grparse::route_collector("upload.bin", "application/mets+xml") ==
              parsev1::COLLECTOR_XML,
          "the METS content type routes to the xml collector");
  require(grparse::route_collector("notes.md", "") == parsev1::COLLECTOR_MARKUP,
          "markdown routes to the markup collector");
  require(grparse::route_collector("index.html", "") == parsev1::COLLECTOR_MARKUP,
          "html routes to the markup collector");
  require(grparse::route_collector("upload.bin", "text/html") ==
              parsev1::COLLECTOR_MARKUP,
          "html content type routes without the extension");
  require(grparse::route_collector("guide.adoc", "") == parsev1::COLLECTOR_MARKUP,
          "asciidoc routes to the markup collector");
  require(grparse::route_collector("paper.tex", "") == parsev1::COLLECTOR_MARKUP,
          "latex routes to the markup collector");
  require(grparse::route_collector("captions.vtt", "") == parsev1::COLLECTOR_MARKUP,
          "webvtt routes to the markup collector");
  require(grparse::route_collector("plan.boxnote", "") == parsev1::COLLECTOR_MARKUP,
          "boxnote routes to the markup collector");
  require(grparse::route_collector("export.json", "") == parsev1::COLLECTOR_MARKUP,
          "json routes to the markup collector's docling reader");
  require(grparse::route_collector("talk.mp3", "") == parsev1::COLLECTOR_ASR,
          "audio routes to the asr collector");
  require(grparse::route_collector("clip.mkv", "") == parsev1::COLLECTOR_ASR,
          "video routes to the asr collector");
  require(grparse::route_collector("upload.bin", "audio/flac") == parsev1::COLLECTOR_ASR,
          "audio content type routes to the asr collector");
  require(grparse::route_collector("upload.bin", "video/webm") == parsev1::COLLECTOR_ASR,
          "video content type routes to the asr collector");
  require(grparse::route_collector("EXTRACT.DAT", "") == parsev1::COLLECTOR_GRPARSE_CV,
          "ebcdic never routes by format; only an explicit selection reaches it");
  require(grparse::route_collector("data.csv", "") == parsev1::COLLECTOR_LIBREOFFICE,
          "csv stays an office format even though it is text");
  require(grparse::route_collector("crawl.warc", "") == parsev1::COLLECTOR_FASTWARC,
          "warc routes to the fastwarc collector");
  require(grparse::route_collector("crawl.warc.gz", "") == parsev1::COLLECTOR_FASTWARC,
          "gzipped warc routes to the fastwarc collector");
  require(grparse::route_collector("CRAWL.WARC.ZST", "") == parsev1::COLLECTOR_FASTWARC,
          "zstd warc routes to the fastwarc collector, case-insensitively");
  require(grparse::route_collector("crawl.warc.lz4", "") == parsev1::COLLECTOR_FASTWARC,
          "lz4 warc routes to the fastwarc collector");
  require(grparse::route_collector("upload.bin", "application/warc") ==
              parsev1::COLLECTOR_FASTWARC,
          "the warc content type routes without the extension");
  require(grparse::route_collector("data.gz", "") == parsev1::COLLECTOR_GRPARSE_CV,
          "a bare .gz is not a warc archive");
  require(grparse::route_collector("archive.tar.gz", "") == parsev1::COLLECTOR_XML,
          "a tar.gz still routes to the xml collector, not fastwarc");

  namespace markupv1 = ai::pipestream::markup::v1;
  require(grparse::markup_format_for("notes.md", "") ==
              markupv1::MARKUP_FORMAT_MARKDOWN,
          "md resolves the markdown hint");
  require(grparse::markup_format_for("upload.bin", "text/markdown") ==
              markupv1::MARKUP_FORMAT_MARKDOWN,
          "the markdown content type resolves the hint without the extension");
  require(grparse::markup_format_for("INDEX.HTM", "") ==
              markupv1::MARKUP_FORMAT_HTML,
          "the html hint is case-insensitive");
  require(grparse::markup_format_for("guide.asciidoc", "") ==
              markupv1::MARKUP_FORMAT_ASCIIDOC,
          "asciidoc resolves its hint");
  require(grparse::markup_format_for("paper.latex", "") ==
              markupv1::MARKUP_FORMAT_LATEX,
          "latex resolves its hint");
  require(grparse::markup_format_for("captions.vtt", "") ==
              markupv1::MARKUP_FORMAT_VTT,
          "vtt resolves its hint");
  require(grparse::markup_format_for("plan.boxnote", "") ==
              markupv1::MARKUP_FORMAT_BOXNOTE,
          "boxnote resolves its hint");
  require(grparse::markup_format_for("export.json", "application/json") ==
              markupv1::MARKUP_FORMAT_DOCLING_JSON,
          "json resolves the docling reader hint");
  require(grparse::markup_format_for("upload.bin", "") ==
              markupv1::MARKUP_FORMAT_UNSPECIFIED,
          "an unrecognized name leaves the collector to sniff");

  auto plan = grparse::resolve_collectors({}, parsev1::COLLECTOR_GRPARSE_CV);
  require(plan.size() == 1 && plan[0] == parsev1::COLLECTOR_GRPARSE_CV,
          "empty selection becomes the routed default");
  plan = grparse::resolve_collectors({}, parsev1::COLLECTOR_ASR);
  require(plan.size() == 1 && plan[0] == parsev1::COLLECTOR_ASR,
          "empty selection follows whatever the router chose");
  plan = grparse::resolve_collectors(
      {parsev1::COLLECTOR_UNSPECIFIED, parsev1::COLLECTOR_LIBREOFFICE,
       parsev1::COLLECTOR_GRPARSE_CV, parsev1::COLLECTOR_LIBREOFFICE},
      parsev1::COLLECTOR_GRPARSE_CV);
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

// Every collector leg runs until the sooner of the inbound call's deadline
// and its own static cap. The ClientContext round-trip is part of the check
// because a capped instant that never reaches a context caps nothing.
void verify_collector_deadline_caps_at_the_sooner_instant() {
  using std::chrono::system_clock;
  constexpr auto kCap = std::chrono::minutes{5};

  const auto impatient = system_clock::now() + std::chrono::seconds{2};
  require(grparse::capped_collector_deadline(impatient, kCap) == impatient,
          "an inbound deadline inside the cap is the leg's deadline");

  const auto patient = system_clock::now() + std::chrono::hours{2};
  const auto ceiling = grparse::capped_collector_deadline(patient, kCap);
  require(ceiling < patient && ceiling <= system_clock::now() + kCap,
          "a client more patient than the cap gets the cap, measured from now");

  const auto unset =
      grparse::capped_collector_deadline(grparse::kNoCollectorDeadline, kCap);
  require(unset > system_clock::now() && unset <= system_clock::now() + kCap,
          "an unset inbound deadline leaves the leg on its own cap");

  // The deadline gRPC carries is the capped instant, not the cap: a leg
  // dialed for an impatient client answers when that client gives up.
  grpc::ClientContext context;
  context.set_deadline(grparse::capped_collector_deadline(impatient, kCap));
  const auto carried = context.deadline();
  require(carried >= impatient - std::chrono::milliseconds{1} &&
              carried <= impatient + std::chrono::milliseconds{1},
          "the leg's context carries the capped instant");
}

}  // namespace

int main() {
  try {
    verify_routing();
    verify_scatter_gather_merges_additively();
    verify_failure_isolation();
    verify_collector_deadline_caps_at_the_sooner_instant();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "collector-coordinator-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
