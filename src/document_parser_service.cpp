#include "grparse/document_parser_service.h"
#include "grparse/schema_version.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <google/protobuf/arena.h>
#include <google/protobuf/descriptor.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chunking/chunker.h"
#include "grparse/base64.h"
#include "grparse/collector_coordinator.h"
#include "grparse/confluence_storage.h"
#include "grparse/document_assembly.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"
#include "grparse/document_render.h"
#include "grparse/in_memory_document.h"
#include "grparse/office_collector.h"
#include "grparse/page_projection.h"
#include "targets/target_step.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

uint64_t content_hash(const std::string& document) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : document) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// The version every image reports names its own accelerator flavor; the
// build injects it so the OpenVINO and CPU images stop claiming cuda.
#ifndef GRPARSE_ORT_PACKAGE_NAME
#define GRPARSE_ORT_PACKAGE_NAME "unknown"
#endif
constexpr const char* kServiceVersion = "grparse-0.1.0-" GRPARSE_ORT_PACKAGE_NAME;

std::string mimetype_for(const fs::path& path) {
  const auto extension = path.extension().string();
  // The wiki storage dialect names itself by suffix, and its own content
  // type is what the document's origin must carry: a ".storage.xhtml" body
  // is not the plain XHTML its final extension would otherwise claim.
  if (confluence_storage_format(path.string(), std::string())) {
    return kConfluenceStorageMimetype;
  }
  if (extension == ".pdf") return "application/pdf";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".tif" || extension == ".tiff") return "image/tiff";
  if (extension == ".docx") {
    return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  }
  if (extension == ".xlsx") {
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  }
  if (extension == ".pptx") {
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  }
  if (extension == ".odt") return "application/vnd.oasis.opendocument.text";
  if (extension == ".ods") return "application/vnd.oasis.opendocument.spreadsheet";
  if (extension == ".odp") return "application/vnd.oasis.opendocument.presentation";
  if (extension == ".doc") return "application/msword";
  if (extension == ".xls") return "application/vnd.ms-excel";
  if (extension == ".ppt") return "application/vnd.ms-powerpoint";
  if (extension == ".csv") return "text/csv";
  if (extension == ".rtf") return "application/rtf";
  if (extension == ".epub") return "application/epub+zip";
  if (extension == ".eml") return "message/rfc822";
  if (extension == ".msg") return "application/vnd.ms-outlook";
  if (extension == ".xml" || extension == ".nxml" || extension == ".xbrl") {
    return "application/xml";
  }
  if (extension == ".mp3") return "audio/mpeg";
  if (extension == ".wav") return "audio/wav";
  if (extension == ".m4a") return "audio/mp4";
  if (extension == ".flac") return "audio/flac";
  if (extension == ".ogg" || extension == ".oga" || extension == ".opus") {
    return "audio/ogg";
  }
  if (extension == ".mp4" || extension == ".m4v") return "video/mp4";
  if (extension == ".mkv") return "video/x-matroska";
  if (extension == ".webm") return "video/webm";
  if (extension == ".mov") return "video/quicktime";
  if (extension == ".png") return "image/png";
  // An extension nothing above recognizes must not masquerade as an image.
  return "application/octet-stream";
}

bool is_pdf(const std::string& content, const fs::path& filename) {
  return filename.extension() == ".pdf" || content.starts_with("%PDF-");
}

const char* collector_name(pipestream::parse::v1::Collector collector) {
  switch (collector) {
    case pipestream::parse::v1::COLLECTOR_GRPARSE_CV: return "grparse-cv";
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return "libreoffice";
    case pipestream::parse::v1::COLLECTOR_POI: return "poi";
    case pipestream::parse::v1::COLLECTOR_CALAMINE: return "calamine";
    case pipestream::parse::v1::COLLECTOR_ASR: return "asr";
    case pipestream::parse::v1::COLLECTOR_EMAIL: return "email";
    case pipestream::parse::v1::COLLECTOR_XML: return "xml";
    case pipestream::parse::v1::COLLECTOR_EBCDIC: return "ebcdic";
    case pipestream::parse::v1::COLLECTOR_EPUB: return "epub";
    case pipestream::parse::v1::COLLECTOR_MARKUP: return "markup";
    case pipestream::parse::v1::COLLECTOR_LOL_HTML: return "lol-html";
    case pipestream::parse::v1::COLLECTOR_FASTWARC: return "fastwarc";
    case pipestream::parse::v1::COLLECTOR_PDF: return "pdf";
    case pipestream::parse::v1::COLLECTOR_CONFLUENCE: return "confluence-storage";
    default: return "unspecified";
  }
}

const char* collector_target_env(pipestream::parse::v1::Collector collector) {
  switch (collector) {
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return "GRPARSE_LIBREOFFICE_TARGET";
    case pipestream::parse::v1::COLLECTOR_ASR: return "GRPARSE_ASR_TARGET";
    case pipestream::parse::v1::COLLECTOR_EMAIL: return "GRPARSE_EMAIL_TARGET";
    case pipestream::parse::v1::COLLECTOR_XML: return "GRPARSE_XML_TARGET";
    case pipestream::parse::v1::COLLECTOR_EBCDIC: return "GRPARSE_EBCDIC_TARGET";
    case pipestream::parse::v1::COLLECTOR_EPUB: return "GRPARSE_EPUB_TARGET";
    case pipestream::parse::v1::COLLECTOR_MARKUP: return "GRPARSE_MARKUP_TARGET";
    case pipestream::parse::v1::COLLECTOR_LOL_HTML: return "GRPARSE_LOL_HTML_TARGET";
    case pipestream::parse::v1::COLLECTOR_FASTWARC: return "GRPARSE_FASTWARC_TARGET";
    case pipestream::parse::v1::COLLECTOR_PDF: return "GRPARSE_PDF_TARGET";
    default: return "";
  }
}

// True for every collector run_remote_collector can dial.
bool remote_collector(pipestream::parse::v1::Collector id) {
  return *collector_target_env(id) != '\0';
}

// True for the collectors that parse in this process instead of over a
// channel. The CV pipeline is in process too but keeps its own path: it is
// page-streamed and tunable, while these are a straight bytes-in,
// Document-out fold with nothing to configure and nothing to reach.
bool local_collector(pipestream::parse::v1::Collector id) {
  return id == pipestream::parse::v1::COLLECTOR_CONFLUENCE;
}

// Runs one in-process collector. Never throws; failures are outcomes, so a
// local collector degrades exactly like a dialed one.
CollectorOutcome run_local_collector(pipestream::parse::v1::Collector id,
                                     const std::string& bytes) {
  if (id == pipestream::parse::v1::COLLECTOR_CONFLUENCE) {
    return parse_confluence_storage(bytes);
  }
  // Unreachable: the local_collector guard admits only the ids above.
  CollectorOutcome outcome;
  outcome.error = std::string("collector '") + collector_name(id) +
                  "' is not wired in yet";
  outcome.code = grpc::StatusCode::UNIMPLEMENTED;
  return outcome;
}

const char* pdf_class_name(PdfClass pdf_class) {
  switch (pdf_class) {
    case PdfClass::kTextBased: return "text-based";
    case PdfClass::kScanned: return "scanned";
    case PdfClass::kImageBased: return "image-based";
    case PdfClass::kMixed: return "mixed";
    default: return "unknown";
  }
}

// Dials one Document-emitting remote collector and returns its outcome.
// Configuration failures are outcomes too, so the parse degrades collector
// by collector no matter where the failure sits. The office collector keeps
// its own path: it streams typed events for gRParse to fold and enrich.
//
// `inbound_deadline` is the deadline of the call that asked for the parse,
// threaded down so no leg outlives the client waiting on it; each leg still
// caps itself at its own ceiling, and kNoCollectorDeadline (an inbound call
// with no deadline of its own) leaves every leg on that ceiling alone.
CollectorOutcome run_remote_collector(
    pipestream::parse::v1::Collector id,
    const std::shared_ptr<CollectorEndpoints>& endpoints,
    const std::string& document_id, const std::string& filename,
    const std::string& content_type, const std::string& bytes,
    const std::string& ebcdic_layout_json,
    const std::string& lol_html_options_json,
    CollectorDeadline inbound_deadline) {
  CollectorOutcome outcome;
  if (!remote_collector(id)) {
    outcome.error = std::string("collector '") + collector_name(id) +
                    "' is not wired in yet";
    outcome.code = grpc::StatusCode::UNIMPLEMENTED;
    return outcome;
  }
  if (endpoints == nullptr || !endpoints->has(id)) {
    outcome.error = std::string(collector_name(id)) +
                    " collector is not configured (" + collector_target_env(id) + ")";
    outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    return outcome;
  }
  switch (id) {
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE:
      return collect_office_document(endpoints->channel(id), document_id,
                                     filename, content_type, bytes,
                                     endpoints->cv_enrichment(), inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_ASR:
      if (endpoints->asr_model().empty()) {
        outcome.error = "asr collector has no model configured (GRPARSE_ASR_MODEL)";
        outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
        return outcome;
      }
      return collect_asr_document(endpoints->channel(id), endpoints->asr_model(), filename, bytes,
                                  inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_EMAIL:
      return collect_email_document(endpoints->channel(id), document_id, filename,
                                    content_type, bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_XML:
      return collect_xml_document(endpoints->channel(id), bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_EBCDIC:
      return collect_ebcdic_document(endpoints->channel(id), ebcdic_layout_json, bytes,
                                     inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_EPUB:
      return collect_epub_document(endpoints->channel(id), bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_MARKUP:
      return collect_markup_document(endpoints->channel(id), filename,
                                     content_type, bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_LOL_HTML:
      return collect_lol_html_document(endpoints->channel(id),
                                       lol_html_options_json, bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_FASTWARC:
      return collect_fastwarc_document(endpoints->channel(id), bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_PDF:
      // The plain leg, reached when the pdf collector shares a selection
      // with other collectors: its Document is the contribution. The
      // classification-driven routing lives with the plan, not here.
      return collect_pdf_document(endpoints->channel(id), bytes, inbound_deadline);
    default:
      // Unreachable: the remote_collector guard admits only the ids the
      // switch handles.
      outcome.error = std::string("collector '") + collector_name(id) +
                      "' is not wired in yet";
      outcome.code = grpc::StatusCode::UNIMPLEMENTED;
      return outcome;
  }
}

std::vector<pipestream::parse::v1::Collector> requested_collectors(
    const google::protobuf::RepeatedField<int>& raw) {
  std::vector<pipestream::parse::v1::Collector> collectors;
  collectors.reserve(raw.size());
  for (const int value : raw) {
    collectors.push_back(static_cast<pipestream::parse::v1::Collector>(value));
  }
  return collectors;
}

// The document's plain text export: its text items in arena order, which is
// each collector's emission order.
std::string document_plain_text(const pipestream::document::v1::Document& document) {
  std::string text;
  for (const auto& item : document.texts()) {
    const pipestream::document::v1::TextItemBase* base = nullptr;
    switch (item.item_case()) {
      case pipestream::document::v1::BaseTextItem::kTitle: base = &item.title().base(); break;
      case pipestream::document::v1::BaseTextItem::kSectionHeader:
        base = &item.section_header().base();
        break;
      case pipestream::document::v1::BaseTextItem::kListItem:
        base = &item.list_item().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFormula: base = &item.formula().base(); break;
      case pipestream::document::v1::BaseTextItem::kText: base = &item.text().base(); break;
      case pipestream::document::v1::BaseTextItem::kCode:
        // CodeItem carries its fields inline instead of a nested base.
        if (!text.empty()) text.push_back('\n');
        text.append(item.code().text());
        break;
      case pipestream::document::v1::BaseTextItem::kFieldHeading:
        base = &item.field_heading().base();
        break;
      case pipestream::document::v1::BaseTextItem::kFieldValue:
        base = &item.field_value().base();
        break;
      case pipestream::document::v1::BaseTextItem::ITEM_NOT_SET: break;
    }
    if (base == nullptr) continue;
    if (!text.empty()) text.push_back('\n');
    text.append(base->text());
  }
  return text;
}

// True when the response must carry this output format. An empty to_formats
// keeps the historical default of the plain-text export alone; every other
// format is opt-in by explicit request.
bool requested(const pipestream::parse::v1::ConvertDocumentOptions& options,
               pipestream::parse::v1::OutputFormat format) {
  if (options.to_formats().empty()) {
    return format == pipestream::parse::v1::OUTPUT_FORMAT_TEXT;
  }
  return std::ranges::find(options.to_formats(), format) !=
         options.to_formats().end();
}

// True for the output formats ConvertSource renders: every named value of
// the wire's OutputFormat enum. UNSPECIFIED and values outside the enum are
// rejected up front by validate_options, each by name.
bool renderable(pipestream::parse::v1::OutputFormat format) {
  switch (format) {
    case pipestream::parse::v1::OUTPUT_FORMAT_TEXT:
    case pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN:
    case pipestream::parse::v1::OUTPUT_FORMAT_HTML:
    case pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE:
    case pipestream::parse::v1::OUTPUT_FORMAT_JSON:
    case pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS:
    case pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG:
    case pipestream::parse::v1::OUTPUT_FORMAT_VTT:
    case pipestream::parse::v1::OUTPUT_FORMAT_YAML:
    case pipestream::parse::v1::OUTPUT_FORMAT_CANONICAL_JSON:
    case pipestream::parse::v1::OUTPUT_FORMAT_GDOCS_JSON:
      return true;
    default:
      return false;
  }
}

// Validation both surfaces share: the unary options message and the
// streaming chunk carry the same recognition fields with the same rules.
grpc::Status validate_ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                 bool has_render_scale, double render_scale) {
  if (has_render_scale && (render_scale < 1.0 || render_scale > 8.0)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "option 'render_scale' must be within [1.0, 8.0]");
  }
  if (has_do_ocr && !do_ocr && force_ocr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "option 'do_ocr' false contradicts option 'force_ocr' true");
  }
  return grpc::Status::OK;
}

// The scheduler tuning the validated recognition fields resolve to. The
// options steer only the in-process CV collector; remote collectors read
// their own inputs and never see them.
PageScheduler::OcrTuning ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                    bool has_render_scale, double render_scale) {
  PageScheduler::OcrTuning tuning;
  if (force_ocr) {
    tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
  } else if (has_do_ocr && !do_ocr) {
    tuning.mode = PageScheduler::OcrTuning::Mode::kOff;
  }
  if (has_render_scale) tuning.render_dpi = render_scale * 72.0;
  return tuning;
}

// `surface` names the RPC in the rejections so a caller learns which of the
// conversion surfaces turned its request down.
grpc::Status validate_options(const pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface = "ConvertSource") {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (field->name() != "to_formats" && field->name() != "collectors" &&
        field->name() != "ebcdic_layout_json" &&
        field->name() != "lol_html_options_json" && field->name() != "do_ocr" &&
        field->name() != "force_ocr" && field->name() != "render_scale") {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement option '" + std::string(field->name()) + "'");
    }
  }
  const grpc::Status tuning_status =
      validate_ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                          options.has_render_scale(), options.render_scale());
  if (!tuning_status.ok()) return tuning_status;
  for (const auto raw : options.to_formats()) {
    const auto format = static_cast<pipestream::parse::v1::OutputFormat>(raw);
    if (!renderable(format)) {
      // A value outside the enum has no name; the rejection still identifies
      // it by number.
      std::string name = pipestream::parse::v1::OutputFormat_Name(format);
      if (name.empty()) name = std::to_string(raw);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement output format '" + name + "'");
    }
  }
  return grpc::Status::OK;
}

grpc::Status status_from_exception(std::exception_ptr failure);

}  // namespace

const std::string& CollectorEndpoints::target(
    pipestream::parse::v1::Collector id) const {
  static const std::string kNone;
  switch (id) {
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return targets_.libreoffice;
    case pipestream::parse::v1::COLLECTOR_ASR: return targets_.asr;
    case pipestream::parse::v1::COLLECTOR_EMAIL: return targets_.email;
    case pipestream::parse::v1::COLLECTOR_XML: return targets_.xml;
    case pipestream::parse::v1::COLLECTOR_EBCDIC: return targets_.ebcdic;
    case pipestream::parse::v1::COLLECTOR_EPUB: return targets_.epub;
    case pipestream::parse::v1::COLLECTOR_MARKUP: return targets_.markup;
    case pipestream::parse::v1::COLLECTOR_LOL_HTML: return targets_.lol_html;
    case pipestream::parse::v1::COLLECTOR_FASTWARC: return targets_.fastwarc;
    case pipestream::parse::v1::COLLECTOR_PDF: return targets_.pdf;
    default: return kNone;
  }
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::channel(
    pipestream::parse::v1::Collector id) {
  const std::string& where = target(id);
  if (where.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& channel = channels_[id];
  if (channel == nullptr) {
    channel = grpc::CreateChannel(where, grpc::InsecureChannelCredentials());
  }
  return channel;
}

DocumentParserService::DocumentParserService(PageScheduler& scheduler,
                                             std::shared_ptr<CollectorEndpoints> endpoints,
                                             CallExecutor::Options executor_options)
    : scheduler_(scheduler),
      endpoints_(std::move(endpoints)),
      executor_(executor_options) {}

namespace {

// One parsed source: the merged document every conversion surface starts
// from, plus the offset side table when this parse produced a usable one.
struct SourceParse {
  fs::path filename;
  CoordinatorResult result;
  chunking::OffsetTable offsets;
};

// The parse every unary surface shares: decode the single FileSource, plan
// the collectors, run them, and merge. The response shaping (exports, chunks)
// belongs to the caller. Blocking throughout, so it runs on a CallExecutor
// worker and never on the thread that reacted to the call; `context` outlives
// that worker because the reactor finishes the call from it.
grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::string& surface, SourceParse* parsed) {
  const auto& sources = request.sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one FileSource containing base64_string");
  }
  const grpc::Status option_status = validate_options(request.options(), surface);
  if (!option_status.ok()) return option_status;
  // Every dialed leg inherits this call's own ceiling, so no collector is
  // waited on past the patience of the client that asked for the parse. A
  // call with no deadline yields time_point::max(), which leaves each leg on
  // its own static cap exactly as before.
  const CollectorDeadline inbound_deadline = context->deadline();
  try {
    const auto& source = sources.Get(0).file();
    auto bytes = std::make_shared<const std::string>(decode_base64(source.base64_string()));
    const fs::path requested_name = source.filename().empty() ? "document.pdf" : fs::path(source.filename()).filename();
    const bool pdf = is_pdf(*bytes, requested_name);

    // The base document carries identity; every collector's output merges
    // into it additively, in plan order. The schema name and version name
    // the wire schema minor this repo currently mirrors, and must match
    // what every other producer stamps on its documents.
    pipestream::document::v1::Document base;
    base.set_schema_name(kWireSchemaName);
    base.set_version(kUpstreamSchemaVersion);
    base.set_name(requested_name.filename().string());
    auto* origin = base.mutable_origin();
    origin->set_filename(requested_name.filename().string());
    origin->set_mimetype(pdf ? "application/pdf" : mimetype_for(requested_name));
    origin->set_binary_hash(content_hash(*bytes));
    base.mutable_body()->set_self_ref("#/body");
    base.mutable_body()->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
    base.mutable_furniture()->set_self_ref("#/furniture");
    base.mutable_furniture()->set_content_layer(
        pipestream::document::v1::CONTENT_LAYER_FURNITURE);

    // The in-process CV collector: the page scheduler's layout, OCR, and
    // model pipeline over rendered pages, assembled into a document
    // fragment. Never throws; failures become the outcome. The tuning rides
    // as a parameter because the pdf routing leg re-enters this path with
    // the inspector's OCR page set applied.
    const auto& request_options = request.options();
    const PageScheduler::OcrTuning tuning = ocr_tuning(
        request_options.has_do_ocr(), request_options.do_ocr(), request_options.force_ocr(),
        request_options.has_render_scale(), request_options.render_scale());
    // The CV path is the only collector that knows where its text lands in
    // the document's text stream. Its offset rows are kept here and only
    // published when that collector turns out to be the whole document.
    auto cv_offsets = std::make_shared<
        google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>();
    auto run_cv = [&](const PageScheduler::OcrTuning& cv_tuning) -> CollectorOutcome {
      CollectorOutcome outcome;
      try {
        struct UnaryResult {
          std::mutex mutex;
          std::condition_variable changed;
          std::map<int, std::shared_ptr<const OcrPage>> pages;
          std::exception_ptr failure;
          int total_pages = 0;
          bool finished = false;
        } state;

        const auto ticket = scheduler.submit(
            bytes, pdf, cv_tuning,
            PageScheduler::Callbacks{
                [&state](int total_pages) {
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.total_pages = total_pages;
                  state.changed.notify_all();
                },
                [&state, context](int page_number, std::shared_ptr<const OcrPage> page) {
                  if (context->IsCancelled()) return PageScheduler::DeliveryResult::kCancelled;
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.pages.emplace(page_number, std::move(page));
                  return PageScheduler::DeliveryResult::kAcceptedAndRelease;
                },
                [&state](std::exception_ptr failure) {
                  std::lock_guard<std::mutex> lock(state.mutex);
                  state.failure = std::move(failure);
                  state.finished = true;
                  state.changed.notify_all();
                }});

        std::unique_lock<std::mutex> lock(state.mutex);
        while (!state.finished) {
          state.changed.wait_for(lock, std::chrono::milliseconds(25));
          if (context->IsCancelled()) ticket.cancel();
        }
        if (context->IsCancelled()) {
          outcome.error = "request cancelled";
          outcome.code = grpc::StatusCode::CANCELLED;
          return outcome;
        }
        const grpc::Status scheduler_status = status_from_exception(state.failure);
        if (!scheduler_status.ok()) {
          outcome.error = scheduler_status.error_message();
          outcome.code = scheduler_status.error_code();
          return outcome;
        }
        if (state.total_pages <= 0 || state.pages.size() != static_cast<size_t>(state.total_pages)) {
          outcome.error = "scheduler completed before every page was available";
          return outcome;
        }
        std::string plain_text;
        AssemblyCursor assembly_cursor;
        google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset> offsets;
        for (int page_number = 1; page_number <= state.total_pages; ++page_number) {
          const auto page = state.pages.find(page_number);
          if (page == state.pages.end()) {
            outcome.error = "scheduler omitted a document page";
            return outcome;
          }
          append_page_to_document(*page->second, page_number, &assembly_cursor,
                                  &outcome.document, &plain_text, &offsets);
        }
        // Heading depth clusters over the whole document's heights, so it
        // can only run after every page is in.
        assign_section_header_levels(&outcome.document);
        *cv_offsets = std::move(offsets);
        outcome.success = true;
        return outcome;
      } catch (...) {
        const grpc::Status status = status_from_exception(std::current_exception());
        outcome.error = status.error_message();
        outcome.code = status.error_code();
        return outcome;
      }
    };

    auto endpoints = collectors;
    const auto ebcdic_layout_json =
        std::make_shared<const std::string>(request.options().ebcdic_layout_json());
    const auto lol_html_options_json = std::make_shared<const std::string>(
        request.options().lol_html_options_json());

    // The default PDF route becomes the pdf inspector when one is
    // configured: its classification decides between the collector's own
    // fast-path Document and a CV run restricted to the pages needing OCR.
    // Unconfigured, PDF stays on the CV path exactly as before.
    const auto selected_collectors =
        requested_collectors(request.options().collectors());
    auto routed = route_collector(requested_name.string(), std::string());
    if (selected_collectors.empty() && pdf &&
        routed == pipestream::parse::v1::COLLECTOR_GRPARSE_CV &&
        endpoints != nullptr && endpoints->has(pipestream::parse::v1::COLLECTOR_PDF)) {
      routed = pipestream::parse::v1::COLLECTOR_PDF;
    }
    const auto plan_ids = resolve_collectors(selected_collectors, routed);
    // Classification routing applies when the pdf collector is the whole
    // plan — by the swap above or by explicit sole selection. Shared with
    // other collectors it is a plain Document-emitting leg.
    const bool pdf_routing =
        pdf && plan_ids.size() == 1 && plan_ids[0] == pipestream::parse::v1::COLLECTOR_PDF &&
        endpoints != nullptr && endpoints->has(pipestream::parse::v1::COLLECTOR_PDF);

    std::vector<PlannedCollector> plan;
    for (const auto id : plan_ids) {
      PlannedCollector collector;
      collector.id = id;
      if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
        collector.run = [run_cv, tuning] { return run_cv(tuning); };
      } else if (local_collector(id)) {
        collector.run = [id, bytes] { return run_local_collector(id, *bytes); };
      } else if (pdf_routing) {
        collector.run = [run_cv, tuning, endpoints, bytes, inbound_deadline, context]() {
          // Same pre-dial cancellation check as the plain collector legs: a
          // call that died after the dequeue check must not dial the
          // inspector either.
          if (context->IsCancelled()) {
            CollectorOutcome outcome;
            outcome.error = "request cancelled";
            outcome.code = grpc::StatusCode::CANCELLED;
            return outcome;
          }
          const PdfParseResult parsed =
              collect_pdf(endpoints->channel(pipestream::parse::v1::COLLECTOR_PDF), *bytes,
                          inbound_deadline);
          const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
          if (parsed.outcome.success && route.fast_path) {
            return parsed.outcome;
          }
          CollectorOutcome outcome;
          if (!parsed.outcome.success) {
            // Degrade, don't sink: an unreachable inspector leaves the parse
            // on exactly the path it would have taken without the collector.
            outcome = run_cv(tuning);
            outcome.warnings.push_back("pdf collector failed (" + parsed.outcome.error +
                                       "); fell back to the in-process CV path");
            return outcome;
          }
          PageScheduler::OcrTuning routed_tuning = tuning;
          routed_tuning.ocr_pages.insert(route.ocr_pages.begin(), route.ocr_pages.end());
          const bool forced =
              route.force_ocr &&
              routed_tuning.mode == PageScheduler::OcrTuning::Mode::kSelective;
          if (forced) routed_tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
          outcome = run_cv(routed_tuning);
          outcome.warnings.push_back(
              "pdf inspector classified the document as " +
              std::string(pdf_class_name(parsed.classification.pdf_class)) +
              (parsed.classification.encoding_issues
                   ? " with encoding issues in the text layer, so its extraction was not taken"
                   : "") +
              (forced
                   ? "; recognition was forced on every page in place of the "
                     "untrustworthy embedded layer"
               : route.ocr_pages.empty()
                   ? "; the CV path's own per-page heuristic decided recognition"
                   : "; recognition restricted to the " +
                         std::to_string(route.ocr_pages.size()) + " page(s) needing OCR"));
          return outcome;
        };
      } else {
        collector.run = [id, endpoints, bytes, requested_name, ebcdic_layout_json,
                         lol_html_options_json, inbound_deadline, context]() {
          // The dequeue check answered "still listening" before this parse
          // started; a cancel can land any time after. Ask again before
          // dialing so a dead call costs no collector leg.
          if (context->IsCancelled()) {
            CollectorOutcome outcome;
            outcome.error = "request cancelled";
            outcome.code = grpc::StatusCode::CANCELLED;
            return outcome;
          }
          return run_remote_collector(id, endpoints, requested_name.string(),
                                      requested_name.string(), std::string(), *bytes,
                                      *ebcdic_layout_json, *lol_html_options_json,
                                      inbound_deadline);
        };
      }
      plan.push_back(std::move(collector));
    }

    CoordinatorResult result = run_collectors(std::move(plan), std::move(base));
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
    }
    if (result.succeeded == 0) {
      const auto& first = result.failures.front();
      std::string message;
      for (const auto& failure : result.failures) {
        if (!message.empty()) message += "; ";
        message += std::string(collector_name(failure.id)) + ": " + failure.error;
      }
      return grpc::Status(first.code, message);
    }

    // The offset table describes the CV collector's own text stream. It is
    // published only when that collector is the entire document: a merge
    // renumbers arena references, and a table that no longer names the items
    // it describes is worse than no table at all.
    if (result.succeeded == 1 && !cv_offsets->empty()) {
      chunking::add_offsets(*cv_offsets, &parsed->offsets);
    }
    // Collector warnings are not failures; they stay on the document, keyed
    // by collector, so nothing the collectors reported is dropped.
    for (const auto& [collector, text] : result.warnings) {
      auto& fields =
          *result.document.mutable_body()->mutable_meta()->mutable_custom_fields();
      *fields[std::string("collector_warnings:") + collector_name(collector)]
           .mutable_list_value()
           ->add_values()
           ->mutable_string_value() = text;
    }
    parsed->filename = requested_name;
    parsed->result = std::move(result);
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

// The collector failures of a parse, as the error list every conversion
// surface reports them under.
void report_failures(const std::vector<CollectorFailureInfo>& failures,
                     google::protobuf::RepeatedPtrField<pipestream::parse::v1::ErrorItem>* errors) {
  for (const auto& failure : failures) {
    auto* error = errors->Add();
    error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
    error->set_module_name(std::string("collector:") + collector_name(failure.id));
    error->set_error_message(failure.error);
  }
}

// Renders every output format the options asked for onto the response.
void render_exports(const pipestream::parse::v1::ConvertDocumentOptions& options,
                    const pipestream::document::v1::Document& document,
                    pipestream::parse::v1::DocumentExports* exports) {
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_TEXT)) {
    exports->set_text(document_plain_text(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_MARKDOWN)) {
    exports->set_md(render_markdown(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML)) {
    exports->set_html(render_html(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_JSON)) {
    exports->set_json(render_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_CANONICAL_JSON)) {
    exports->set_canonical_json(render_canonical_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_GDOCS_JSON)) {
    exports->set_gdocs_json(render_gdocs_json(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCTAGS)) {
    exports->set_doctags(render_doctags(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_DOCLANG)) {
    exports->set_doclang(render_doclang(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_VTT)) {
    exports->set_vtt(render_vtt(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_HTML_SPLIT_PAGE)) {
    exports->set_html_split_page(render_html_split_page(document));
  }
  if (requested(options, pipestream::parse::v1::OUTPUT_FORMAT_YAML)) {
    exports->set_yaml(render_yaml(document));
  }
}

// The reactor the blocking unary surfaces finish through. Construction hands
// `work` to the executor and returns immediately, so the event-manager thread
// that reacted to the call is free the moment the handler returns; the worker
// finishes the call. gRPC queues operations requested before the reactor is
// bound, so finishing from either thread is safe whichever wins the race.
// Cancellation needs no OnCancel here: the work polls the context, which is
// where it can act on the answer.
class ParseUnaryReactor final : public grpc::ServerUnaryReactor {
 public:
  ParseUnaryReactor(grpc::CallbackServerContext* context, CallExecutor& executor,
                    std::function<grpc::Status()> work) {
    const bool queued = executor.submit([this, context, work = std::move(work)] {
      // A call can wait in the queue behind every conversion ahead of it, so
      // the first thing a worker does is ask whether anyone is still
      // listening. The answer costs one atomic read and saves the whole
      // parse behind it: the base64 decode of up to a few hundred megabytes,
      // and every collector leg after that.
      if (context->IsCancelled()) {
        Finish(grpc::Status(grpc::StatusCode::CANCELLED,
                            "request cancelled before conversion started"));
        return;
      }
      grpc::Status status;
      try {
        status = work();
      } catch (...) {
        status = status_from_exception(std::current_exception());
      }
      // Last statement on purpose: OnDone can delete this reactor from
      // another thread the instant the call completes.
      Finish(std::move(status));
    });
    if (queued) return;
    // The reactor holds the call and is the only thing that can answer it, so
    // a refused submission is a status, not a dropped RPC.
    Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "conversion executor is saturated"));
  }

  void OnDone() override { delete this; }
};

// The trivial surfaces answer on the reaction thread through the context's own
// reactor, which allocates nothing.
grpc::ServerUnaryReactor* finish_inline(grpc::CallbackServerContext* context,
                                        grpc::Status status) {
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}

}  // namespace

grpc::ServerUnaryReactor* DocumentParserService::ConvertSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ConvertSourceRequest* request,
    pipestream::parse::v1::ConvertSourceResponse* response) {
  return new ParseUnaryReactor(context, executor_, [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    SourceParse parsed;
    const grpc::Status parse_status = parse_source(context, request->request(), scheduler_,
                                                   endpoints_, "ConvertSource", &parsed);
    if (!parse_status.ok()) return parse_status;
    auto& result = parsed.result;
    auto* converted = response->mutable_response();
    auto* document_response = converted->mutable_document();
    document_response->set_filename(parsed.filename.string());
    auto* document = document_response->mutable_doc();
    *document = std::move(result.document);
    report_failures(result.failures, converted->mutable_errors());
    // Every requested output format renders from the same merged document;
    // TEXT keeps its arena-order line export, the rest fold the body tree.
    const auto& options = request->request().options();
    render_exports(options, *document, document_response->mutable_exports());
    // The target delivers the same conversion somewhere else; the response
    // body above keeps everything it already carries either way. It runs
    // here on the conversion's own worker because it compresses and uploads,
    // which is not work a gRPC event thread may be handed.
    const auto& target = request->request().target();
    bool delivery_failed = false;
    if (targets::needs_delivery(target)) {
      const grpc::Status delivered =
          targets::deliver(target, *document, document_response->exports(),
                           converted->mutable_target_result());
      if (!delivered.ok()) {
        // Delivery is additive, never a replacement: the conversion the
        // response already carries survives a store that would not take it.
        // The failure lands as an error item and a partial status, except a
        // misconfigured target itself, which the caller must fix and gets
        // told about at the RPC layer.
        if (delivered.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
            delivered.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
          return delivered;
        }
        delivery_failed = true;
        auto* error = converted->add_errors();
        error->set_component_type(pipestream::parse::v1::COMPONENT_TYPE_PIPELINE);
        error->set_module_name("target-delivery");
        error->set_error_message(delivered.error_message());
      }
    }
    converted->set_status(result.failures.empty() && !delivery_failed
                              ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                              : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
    converted->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    (*converted->mutable_timings())["total"] = converted->processing_time();
    return grpc::Status::OK;
  });
}

namespace {

// The converted document a chunk response carries when the caller asked for
// it. The chunks themselves never depend on it.
void attach_converted_document(const pipestream::parse::v1::ConvertDocumentOptions& options,
                               const fs::path& filename, CoordinatorResult* result,
                               pipestream::parse::v1::ChunkDocumentResponse* response) {
  auto* entry = response->add_documents();
  entry->set_kind("document");
  auto* content = entry->mutable_content();
  content->set_filename(filename.string());
  *content->mutable_doc() = std::move(result->document);
  render_exports(options, content->doc(), content->mutable_exports());
  entry->set_status(result->failures.empty()
                        ? pipestream::parse::v1::CONVERSION_STATUS_SUCCESS
                        : pipestream::parse::v1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  report_failures(result->failures, entry->mutable_errors());
}

}  // namespace

grpc::ServerUnaryReactor* DocumentParserService::ChunkHierarchicalSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHierarchicalSourceRequest* request,
    pipestream::parse::v1::ChunkHierarchicalSourceResponse* response) {
  return new ParseUnaryReactor(context, executor_, [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    const auto& chunk_request = request->request();
    SourceParse parsed;
    pipestream::parse::v1::ConvertDocumentRequest convert;
    *convert.mutable_sources() = chunk_request.sources();
    *convert.mutable_options() = chunk_request.convert_options();
    const grpc::Status parse_status =
        parse_source(context, convert, scheduler_, endpoints_, "ChunkHierarchicalSource", &parsed);
    if (!parse_status.ok()) return parse_status;
    auto* chunked = response->mutable_response();
    const chunking::ChunkOptions options{chunk_request.chunking_options().use_markdown_tables(),
                                         chunk_request.chunking_options().include_raw_text()};
    for (auto& chunk : chunking::chunk_hierarchical(parsed.result.document, parsed.offsets,
                                                    options, parsed.filename.string())) {
      *chunked->add_chunks() = std::move(chunk);
    }
    if (chunk_request.include_converted_doc()) {
      attach_converted_document(chunk_request.convert_options(), parsed.filename,
                                &parsed.result, chunked);
    }
    chunked->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return grpc::Status::OK;
  });
}

grpc::ServerUnaryReactor* DocumentParserService::ChunkHybridSource(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::ChunkHybridSourceRequest* request,
    pipestream::parse::v1::ChunkHybridSourceResponse* response) {
  return new ParseUnaryReactor(context, executor_, [this, context, request, response] {
    const auto started = std::chrono::steady_clock::now();
    const auto& chunk_request = request->request();
    // The budget decides every boundary, so it is validated before any work
    // starts rather than defaulted to a number nobody asked for.
    const grpc::Status option_status =
        chunking::validate_hybrid_options(chunk_request.chunking_options());
    if (!option_status.ok()) return option_status;
    SourceParse parsed;
    pipestream::parse::v1::ConvertDocumentRequest convert;
    *convert.mutable_sources() = chunk_request.sources();
    *convert.mutable_options() = chunk_request.convert_options();
    const grpc::Status parse_status =
        parse_source(context, convert, scheduler_, endpoints_, "ChunkHybridSource", &parsed);
    if (!parse_status.ok()) return parse_status;
    auto* chunked = response->mutable_response();
    for (auto& chunk :
         chunking::chunk_hybrid(parsed.result.document, parsed.offsets,
                                chunk_request.chunking_options(), parsed.filename.string())) {
      *chunked->add_chunks() = std::move(chunk);
    }
    if (chunk_request.include_converted_doc()) {
      attach_converted_document(chunk_request.convert_options(), parsed.filename,
                                &parsed.result, chunked);
    }
    chunked->set_processing_time(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return grpc::Status::OK;
  });
}

grpc::ServerUnaryReactor* DocumentParserService::Health(
    grpc::CallbackServerContext* context, const pipestream::parse::v1::HealthRequest*,
    pipestream::parse::v1::HealthResponse* response) {
  response->set_status("ready");
  response->set_version(kServiceVersion);
  return finish_inline(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* DocumentParserService::GetServiceInfo(
    grpc::CallbackServerContext* context,
    const pipestream::parse::v1::GetServiceInfoRequest*,
    pipestream::parse::v1::GetServiceInfoResponse* response) {
  response->set_name("gRParse");
  response->set_version(kServiceVersion);
  auto* ui = response->mutable_ui();
  ui->set_title("gRParse");
  ui->set_path("/ui/grparse");
  ui->set_description("Diskless PDF/image to page-streamed protobuf with OCR and layout");
  return finish_inline(context, grpc::Status::OK);
}

namespace {

constexpr size_t kMaximumDocumentBytes = 500U * 1024U * 1024U;

grpc::Status status_from_exception(std::exception_ptr failure) {
  try {
    if (failure) std::rethrow_exception(failure);
  } catch (const InvalidDocument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const SchedulerSaturated& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::bad_alloc& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::invalid_argument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::exception& error) {
    return grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  }
  return grpc::Status::OK;
}

class ArenaEvent final {
 public:
  ArenaEvent() : message(google::protobuf::Arena::Create<pipestream::parse::v1::DocumentStreamEvent>(&arena)) {}

  google::protobuf::Arena arena;
  pipestream::parse::v1::DocumentStreamEvent* message;
};

class DocumentStreamReactor final
    : public grpc::ServerBidiReactor<pipestream::parse::v1::DocumentChunk,
                                     pipestream::parse::v1::DocumentStreamEvent> {
 public:
  DocumentStreamReactor(grpc::CallbackServerContext* context, PageScheduler& scheduler,
                        std::shared_ptr<CollectorEndpoints> endpoints)
      : context_(context),
        scheduler_(scheduler),
        endpoints_(std::move(endpoints)),
        // The scheduler never delivers more than one page window ahead of
        // the credits this reactor returns, so the configured window is the
        // exact buffer bound; any smaller cap would fail well-behaved
        // clients with RESOURCE_EXHAUSTED under a raised GRPARSE_PAGE_WINDOW.
        maximum_buffered_pages_(scheduler.page_window()),
        callback_gate_(std::make_shared<CallbackGate>()) {
    callback_gate_->reactor = this;
    StartRead(&incoming_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client_cancelled_) {
          request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
          return;
        }
        if (!complete_seen_ || bytes_.empty()) {
          request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                             "stream must end with a non-empty complete chunk"));
          return;
        }
      }
      begin_processing();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finish_requested_) return;
      if (complete_seen_) {
        request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "received data after complete chunk"));
        return;
      }
      if (document_id_.empty()) {
        document_id_ = incoming_.document_id();
        filename_ = fs::path(incoming_.filename()).filename();
        content_type_ = incoming_.content_type();
        if (document_id_.empty() || filename_.empty()) {
          request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                             "first chunk requires document_id and filename"));
          return;
        }
      } else if ((!incoming_.document_id().empty() && incoming_.document_id() != document_id_) ||
                 (!incoming_.filename().empty() && fs::path(incoming_.filename()).filename() != filename_)) {
        request_finish_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                           "all chunks must describe the same document"));
        return;
      }
      // The collector selection resolves like the identity fields: the
      // first chunk whose list is non-empty wins.
      if (requested_collectors_.empty() && !incoming_.collectors().empty()) {
        for (const int value : incoming_.collectors()) {
          requested_collectors_.push_back(static_cast<pipestream::parse::v1::Collector>(value));
        }
      }
      // Recognition tuning resolves under the same doctrine: the first
      // chunk that sets a field wins. Validation waits for the complete
      // stream so a contradiction split across chunks is still caught.
      if (!do_ocr_.has_value() && incoming_.has_do_ocr()) do_ocr_ = incoming_.do_ocr();
      if (!force_ocr_.has_value() && incoming_.has_force_ocr()) {
        force_ocr_ = incoming_.force_ocr();
      }
      if (!render_scale_.has_value() && incoming_.has_render_scale()) {
        render_scale_ = incoming_.render_scale();
      }
      if (incoming_.data().size() > kMaximumDocumentBytes - bytes_.size()) {
        request_finish_locked(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                           "document exceeds 500 MiB streaming limit"));
        return;
      }
      bytes_.append(incoming_.data());
      complete_seen_ = incoming_.complete();
      incoming_.Clear();
      StartRead(&incoming_);
    }
  }

  void OnWriteDone(bool ok) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::INTERNAL, "write completed without an event"));
      return;
    }
    const bool page_written = events_.front()->message->has_page();
    if (page_written && buffered_pages_ > 0) --buffered_pages_;
    events_.pop_front();
    write_in_flight_ = false;
    if (!ok) {
      client_cancelled_ = true;
      events_.clear();
      buffered_pages_ = 0;
      request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "client stopped reading"));
      return;
    }
    if (page_written) ticket_.release();
    pump_locked();
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    client_cancelled_ = true;
    request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
  }

  void OnDone() override {
    // gRPC's callback API transfers ownership of the reactor; OnDone must delete
    // it.  Scheduler threads are not gRPC reactions and hold no call reference,
    // so the gate mutex — not just the null check — is what keeps this delete
    // ordered after any in-progress on_page/on_scheduler_finish call.
    const auto gate = callback_gate_;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->reactor = nullptr;
    }
    delete this;
  }

 private:
  struct CallbackGate {
    std::mutex mutex;
    DocumentStreamReactor* reactor = nullptr;
  };

  void begin_processing() {
    std::shared_ptr<const std::string> bytes;
    bool pdf = false;
    bool want_cv = false;
    bool pdf_routing = false;
    PageScheduler::OcrTuning tuning;
    std::vector<pipestream::parse::v1::Collector> remotes;
    std::vector<pipestream::parse::v1::Collector> locals;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // The resolved recognition fields validate exactly like the unary
      // options; an invalid value fails the stream naming the offender.
      const grpc::Status tuning_status = validate_ocr_tuning(
          do_ocr_.has_value(), do_ocr_.value_or(true), force_ocr_.value_or(false),
          render_scale_.has_value(), render_scale_.value_or(0.0));
      if (!tuning_status.ok()) {
        request_finish_locked(tuning_status);
        return;
      }
      tuning = ocr_tuning(do_ocr_.has_value(), do_ocr_.value_or(true),
                          force_ocr_.value_or(false), render_scale_.has_value(),
                          render_scale_.value_or(0.0));
      bytes = std::make_shared<const std::string>(std::move(bytes_));
      pdf = content_type_ == "application/pdf" || is_pdf(*bytes, filename_);
      pdf_ = pdf;
      // Scatter-gather routing: the plan resolves from the request's
      // collector selection or the document's format, unwired collectors
      // fail immediately, and each wired collector is one pending part of
      // the stream. The parse degrades collector by collector instead of
      // failing while any part succeeds. The default PDF route becomes the
      // pdf inspector when one is configured, exactly like the unary path.
      auto routed = route_collector(filename_.string(), content_type_);
      if (requested_collectors_.empty() && pdf &&
          routed == pipestream::parse::v1::COLLECTOR_GRPARSE_CV && endpoints_ != nullptr &&
          endpoints_->has(pipestream::parse::v1::COLLECTOR_PDF)) {
        routed = pipestream::parse::v1::COLLECTOR_PDF;
      }
      const auto plan = resolve_collectors(requested_collectors_, routed);
      // The routing leg applies when the pdf collector is the whole plan;
      // shared with other collectors it runs as a plain Document leg.
      pdf_routing = pdf && plan.size() == 1 &&
                    plan[0] == pipestream::parse::v1::COLLECTOR_PDF && endpoints_ != nullptr &&
                    endpoints_->has(pipestream::parse::v1::COLLECTOR_PDF);
      for (const auto id : plan) {
        if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
          want_cv = true;
          ++pending_parts_;
        } else if (local_collector(id)) {
          locals.push_back(id);
          ++pending_parts_;
        } else if (remote_collector(id)) {
          remotes.push_back(id);
          ++pending_parts_;
        } else {
          record_part_failure_locked(
              id, grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                               std::string("collector '") + collector_name(id) +
                                   "' is not wired in yet"));
        }
      }
      if (pending_parts_ == 0) {
        request_finish_locked(first_failure_status_);
        return;
      }
    }
    // Hash the request once, here, rather than under the reactor lock at
    // completion: it is a linear pass over up to 500 MiB.
    const uint64_t hash = content_hash(*bytes);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      document_bytes_hash_ = hash;
    }

    for (const auto id : locals) {
      spawn_local_collector(id, bytes);
    }
    for (const auto id : remotes) {
      if (id == pipestream::parse::v1::COLLECTOR_PDF && pdf_routing) {
        spawn_pdf_router(bytes, pdf, tuning);
      } else {
        spawn_remote_collector(id, bytes);
      }
    }
    if (!want_cv) return;
    submit_cv(bytes, pdf, tuning);
  }

  // Submits the document to the in-process CV pipeline and wires its
  // callbacks through the gate. Shared by the plain CV plan leg and by the
  // pdf routing leg once the inspector's classification has named the pages
  // needing OCR; a submission failure degrades like any CV failure.
  void submit_cv(std::shared_ptr<const std::string> bytes, bool pdf,
                 PageScheduler::OcrTuning tuning) {
    try {
      const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
      auto ticket = scheduler_.submit(
          std::move(bytes), pdf, tuning,
          PageScheduler::Callbacks{
              [weak_gate](int total_pages) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) gate->reactor->on_document(total_pages);
                }
              },
              [weak_gate](int page_number, std::shared_ptr<const OcrPage> page) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) {
                    return gate->reactor->on_page(page_number, std::move(page));
                  }
                }
                return PageScheduler::DeliveryResult::kCancelled;
              },
              [weak_gate](std::exception_ptr failure) {
                if (const auto gate = weak_gate.lock()) {
                  std::lock_guard<std::mutex> lock(gate->mutex);
                  if (gate->reactor != nullptr) gate->reactor->on_scheduler_finish(std::move(failure));
                }
              }});
      std::lock_guard<std::mutex> lock(mutex_);
      ticket_ = std::move(ticket);
      if (finish_requested_) cancel_ticket_locked();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      record_part_failure_locked(pipestream::parse::v1::COLLECTOR_GRPARSE_CV,
                                 status_from_exception(std::current_exception()));
      part_done_locked();
      pump_locked();
    }
  }

  // The pdf routing leg: the inspector's classification decides whether the
  // parse is the collector's own fast-path Document (text-based) or the
  // in-process CV pipeline restricted to the pages the inspector named as
  // needing OCR. A failed classification degrades to the unrouted CV path,
  // never to a failed parse. Runs on its own thread for the same reason
  // spawn_remote_collector does: the collector call is a blocking client.
  void spawn_pdf_router(std::shared_ptr<const std::string> bytes, bool pdf,
                        PageScheduler::OcrTuning tuning) {
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    auto endpoints = endpoints_;
    const CollectorDeadline inbound_deadline = context_->deadline();
    std::thread([weak_gate, endpoints, bytes = std::move(bytes), pdf, inbound_deadline,
                 tuning = std::move(tuning)]() mutable {
      const PdfParseResult parsed = collect_pdf(
          endpoints == nullptr
              ? nullptr
              : endpoints->channel(pipestream::parse::v1::COLLECTOR_PDF),
          *bytes, inbound_deadline);
      const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
      if (parsed.outcome.success && route.fast_path) {
        if (const auto gate = weak_gate.lock()) {
          std::lock_guard<std::mutex> lock(gate->mutex);
          if (gate->reactor != nullptr) {
            gate->reactor->on_collector_done(pipestream::parse::v1::COLLECTOR_PDF,
                                             std::move(parsed.outcome));
          }
        }
        return;
      }
      if (parsed.outcome.success) {
        tuning.ocr_pages.insert(route.ocr_pages.begin(), route.ocr_pages.end());
        if (route.force_ocr && tuning.mode == PageScheduler::OcrTuning::Mode::kSelective) {
          tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
        }
      } else if (const auto gate = weak_gate.lock()) {
        // The degradation stays visible: the pdf collector's failure rides
        // the complete event as a failure entry while the in-process CV path
        // parses the document, same as any collector failing beside a
        // surviving one. The pending part itself is settled by the CV run.
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) gate->reactor->note_pdf_fallback(parsed.outcome);
      }
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->submit_cv(std::move(bytes), pdf, std::move(tuning));
        }
      }
    }).detach();
  }

  void note_pdf_fallback(const CollectorOutcome& outcome) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_part_failure_locked(
        pipestream::parse::v1::COLLECTOR_PDF,
        grpc::Status(outcome.code, outcome.error + "; fell back to the in-process CV path"));
  }

  // An in-process collector runs on its own thread for the same reason a
  // remote one does: the fold is straight-line work on the request bytes,
  // not a gRPC reaction, and the gate keeps its completion safe against
  // reactor teardown.
  void spawn_local_collector(pipestream::parse::v1::Collector id,
                             std::shared_ptr<const std::string> bytes) {
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    std::thread([weak_gate, id, bytes]() {
      CollectorOutcome outcome = run_local_collector(id, *bytes);
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->on_collector_done(id, std::move(outcome));
        }
      }
    }).detach();
  }

  // A remote collector runs on its own thread: it is a blocking client
  // stream, not a gRPC reaction. The gate keeps its completion safe against
  // reactor teardown exactly like the scheduler callbacks. A client cancel
  // abandons the result; the leg's deadline, the sooner of this call's own
  // and the collector's cap, bounds the orphaned call, so a client that
  // walked away is not waited on past the deadline it set. The streaming
  // wire carries no ebcdic layout and no lol-html rules, so selecting either
  // collector here degrades to that collector's own INVALID_ARGUMENT.
  void spawn_remote_collector(pipestream::parse::v1::Collector id,
                              std::shared_ptr<const std::string> bytes) {
    std::string document_id;
    std::string filename;
    std::string content_type;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      document_id = document_id_;
      filename = filename_.string();
      content_type = content_type_;
    }
    const std::weak_ptr<CallbackGate> weak_gate = callback_gate_;
    auto endpoints = endpoints_;
    const CollectorDeadline inbound_deadline = context_->deadline();
    std::thread([weak_gate, endpoints, id, bytes, document_id, filename, content_type,
                 inbound_deadline]() {
      CollectorOutcome outcome = run_remote_collector(
          id, endpoints, document_id, filename, content_type, *bytes,
          std::string(), std::string(), inbound_deadline);
      if (const auto gate = weak_gate.lock()) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->reactor != nullptr) {
          gate->reactor->on_collector_done(id, std::move(outcome));
        }
      }
    }).detach();
  }

  void on_document(int total_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_pages_ = total_pages;
  }

  PageScheduler::DeliveryResult on_page(int page_number, std::shared_ptr<const OcrPage> page) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return PageScheduler::DeliveryResult::kCancelled;
    if (buffered_pages_ >= maximum_buffered_pages_) {
      completed_pages_.clear();
      if (write_in_flight_ && !events_.empty()) {
        events_.erase(std::next(events_.begin()), events_.end());
        buffered_pages_ = events_.front()->message->has_page() ? 1 : 0;
      } else {
        events_.clear();
        buffered_pages_ = 0;
      }
      request_finish_locked(grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "client did not consume page events within the bounded stream buffer"));
      return PageScheduler::DeliveryResult::kCancelled;
    }
    ++buffered_pages_;
    completed_pages_.emplace(page_number, std::move(page));
    while (true) {
      auto page_it = completed_pages_.find(next_page_);
      if (page_it == completed_pages_.end()) break;
      auto event = std::make_unique<ArenaEvent>();
      event->message->set_document_id(document_id_);
      event->message->set_total_pages(total_pages_);
      append_page_data(*page_it->second, next_page_, &assembly_cursor_, event->message->mutable_page());
      // Heading depth needs every page's heights; the terminal event ships
      // the clustered result for the level-0 headers streamed here.
      collect_header_heights(event->message->page(), &header_heights_);
      completed_pages_.erase(page_it);
      events_.push_back(std::move(event));
      ++next_page_;
    }
    pump_locked();
    return PageScheduler::DeliveryResult::kAccepted;
  }

  void on_scheduler_finish(std::exception_ptr failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_ || context_->IsCancelled()) {
      request_finish_locked(grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled"));
      return;
    }
    grpc::Status status = status_from_exception(std::move(failure));
    if (status.ok() && (next_page_ != total_pages_ + 1 || !completed_pages_.empty())) {
      status = grpc::Status(grpc::StatusCode::INTERNAL,
                            "scheduler completed before every page was assembled");
    }
    if (status.ok()) {
      ++succeeded_parts_;
    } else {
      record_part_failure_locked(pipestream::parse::v1::COLLECTOR_GRPARSE_CV, status);
    }
    part_done_locked();
    pump_locked();
  }

  void on_collector_done(pipestream::parse::v1::Collector collector, CollectorOutcome outcome) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_cancelled_) return;
    if (outcome.success) {
      // A collector's finished document reaches the stream as page events
      // first, the same shape the CV pipeline emits page by page, so a
      // consumer that renders pages sees an inspector-routed text PDF
      // exactly as it sees a rasterized one. The whole document follows
      // as the collector-document event for consumers that want it intact.
      // The pdf collector's text is the document's own text layer; other
      // collectors' text has no OCR-or-digital story to tell.
      auto pages = project_page_data(
          outcome.document, collector == pipestream::parse::v1::COLLECTOR_PDF
                                ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                : pipestream::parse::v1::TEXT_SOURCE_UNSPECIFIED);
      if (!pages.empty()) {
        total_pages_ = std::max(total_pages_, static_cast<int>(pages.size()));
        for (auto& page : pages) {
          auto page_event = std::make_unique<ArenaEvent>();
          page_event->message->set_document_id(document_id_);
          page_event->message->set_total_pages(total_pages_);
          *page_event->message->mutable_page() = std::move(page);
          events_.push_back(std::move(page_event));
        }
      }
      auto event = std::make_unique<ArenaEvent>();
      event->message->set_document_id(document_id_);
      event->message->set_total_pages(total_pages_);
      auto* collector_document = event->message->mutable_collector_document();
      collector_document->set_collector(collector);
      *collector_document->mutable_document() = std::move(outcome.document);
      for (auto& warning : outcome.warnings) {
        collector_document->add_warnings(std::move(warning));
      }
      events_.push_back(std::move(event));
      ++succeeded_parts_;
    } else {
      record_part_failure_locked(collector,
                                 grpc::Status(outcome.code, outcome.error));
    }
    part_done_locked();
    pump_locked();
  }

  void record_part_failure_locked(pipestream::parse::v1::Collector collector,
                                  grpc::Status status) {
    auto* failure = collector_failures_.Add();
    failure->set_collector(collector);
    failure->set_error(status.error_message());
    if (first_failure_status_.ok()) first_failure_status_ = std::move(status);
  }

  // One collector part finished. When the last one lands, either every part
  // failed (the stream fails with the first failure's status) or the
  // terminal complete event carries the origin and the per-collector
  // failures of an otherwise successful parse.
  void part_done_locked() {
    if (pending_parts_ > 0) --pending_parts_;
    if (pending_parts_ > 0 || finish_requested_) return;
    if (succeeded_parts_ == 0) {
      request_finish_locked(first_failure_status_.ok()
                                ? grpc::Status(grpc::StatusCode::INTERNAL,
                                               "every collector failed")
                                : first_failure_status_);
      return;
    }
    auto event = std::make_unique<ArenaEvent>();
    event->message->set_document_id(document_id_);
    event->message->set_total_pages(total_pages_);
    auto* complete = event->message->mutable_complete();
    auto* origin = complete->mutable_origin();
    origin->set_filename(filename_.string());
    origin->set_mimetype(pdf_ ? "application/pdf" : mimetype_for(filename_));
    origin->set_binary_hash(document_bytes_hash_);
    *complete->mutable_collector_failures() = collector_failures_;
    if (!header_heights_.empty()) {
      auto levels = section_header_levels(std::move(header_heights_));
      complete->mutable_section_header_levels()->insert(levels.begin(), levels.end());
    }
    events_.push_back(std::move(event));
    request_finish_locked(grpc::Status::OK);
  }

  void request_finish_locked(grpc::Status status) {
    if (finish_started_) return;
    if (finish_requested_) {
      if (finish_status_.ok() && !status.ok()) {
        finish_status_ = std::move(status);
        cancel_ticket_locked();
      }
      pump_locked();
      return;
    }
    finish_requested_ = true;
    finish_status_ = std::move(status);
    if (!finish_status_.ok()) cancel_ticket_locked();
    pump_locked();
  }

  void cancel_ticket_locked() {
    if (ticket_cancelled_ || !ticket_.valid()) return;
    ticket_cancelled_ = true;
    ticket_.cancel();
  }

  void pump_locked() {
    if (!write_in_flight_ && !events_.empty()) {
      write_in_flight_ = true;
      StartWrite(events_.front()->message);
      return;
    }
    if (finish_requested_ && !write_in_flight_ && events_.empty() && !finish_started_) {
      finish_started_ = true;
      Finish(finish_status_);
    }
  }

  grpc::CallbackServerContext* context_;
  PageScheduler& scheduler_;
  std::shared_ptr<CollectorEndpoints> endpoints_;
  const size_t maximum_buffered_pages_;
  std::shared_ptr<CallbackGate> callback_gate_;
  std::mutex mutex_;
  pipestream::parse::v1::DocumentChunk incoming_;
  std::string document_id_;
  fs::path filename_;
  std::string content_type_;
  std::string bytes_;
  uint64_t document_bytes_hash_ = 0;
  PageScheduler::Ticket ticket_;
  std::map<int, std::shared_ptr<const OcrPage>> completed_pages_;
  std::deque<std::unique_ptr<ArenaEvent>> events_;
  AssemblyCursor assembly_cursor_;
  // Level-less section headers streamed so far, clustered into depths for
  // the terminal event.
  std::vector<HeaderHeight> header_heights_;
  grpc::Status finish_status_;
  std::vector<pipestream::parse::v1::Collector> requested_collectors_;
  // Recognition fields resolved from the first chunk that set each one;
  // empty means the chunk stream never set it.
  std::optional<bool> do_ocr_;
  std::optional<bool> force_ocr_;
  std::optional<double> render_scale_;
  google::protobuf::RepeatedPtrField<pipestream::parse::v1::CollectorFailure>
      collector_failures_;
  grpc::Status first_failure_status_ = grpc::Status::OK;
  int pending_parts_ = 0;
  int succeeded_parts_ = 0;
  int total_pages_ = 0;
  int next_page_ = 1;
  size_t buffered_pages_ = 0;
  bool pdf_ = false;
  bool complete_seen_ = false;
  bool write_in_flight_ = false;
  bool client_cancelled_ = false;
  bool ticket_cancelled_ = false;
  bool finish_requested_ = false;
  bool finish_started_ = false;
};

}  // namespace

DocumentStreamingService::DocumentStreamingService(PageScheduler& scheduler,
                                                   std::shared_ptr<CollectorEndpoints> endpoints)
    : scheduler_(scheduler), endpoints_(std::move(endpoints)) {}

grpc::ServerBidiReactor<pipestream::parse::v1::DocumentChunk, pipestream::parse::v1::DocumentStreamEvent>*
DocumentStreamingService::StreamProcessDocument(grpc::CallbackServerContext* context) {
  return new DocumentStreamReactor(context, scheduler_, endpoints_);
}

}  // namespace grparse
