#include "parse_support.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "grparse/confluence_storage.h"
#include "grparse/document_collectors.h"
#include "grparse/in_memory_document.h"
#include "grparse/data_totals.h"
#include "grparse/office_collector.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// True when the office document is a spreadsheet by name or declared type.
// Sheet renders are cell grids the layout detector reads as one figure, so
// the CV enrichment leg is kept off them: every picture a sheet holds
// reaches the document typed, through the office collector's own events.
bool spreadsheet_format(const fs::path& filename, const std::string& content_type) {
  std::string extension = filename.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  for (const char* sheet : {".xls", ".xlsx", ".xlsm", ".xlsb", ".ods", ".ots", ".fods", ".csv"}) {
    if (extension == sheet) return true;
  }
  std::string type = content_type;
  std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
  return type.contains("spreadsheet") || type.contains("ms-excel") || type == "text/csv";
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

}  // namespace

uint64_t content_hash(const std::string& document) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : document) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool is_pdf(const std::string& content, const fs::path& filename) {
  return filename.extension() == ".pdf" || content.starts_with("%PDF-");
}

bool remote_collector(pipestream::parse::v1::Collector id) {
  return *collector_target_env(id) != '\0';
}

bool local_collector(pipestream::parse::v1::Collector id) {
  return id == pipestream::parse::v1::COLLECTOR_CONFLUENCE;
}

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
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: {
      const bool spreadsheet = spreadsheet_format(filename, content_type);
      if (spreadsheet && endpoints->cv_enrichment().detector != nullptr) {
        data_counters().cv_enrichment_skipped.fetch_add(1, std::memory_order_relaxed);
        data_log("office " + filename + ": spreadsheet, CV enrichment of sheet renders skipped");
      }
      return collect_office_document(endpoints->channel(id), document_id,
                                     filename, content_type, bytes,
                                     spreadsheet ? OfficeCvEnrichment{}
                                                 : endpoints->cv_enrichment(),
                                     inbound_deadline);
    }
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
      // The book, not the skeleton: the chapters fold through the markup
      // collector when one is configured, and the leg says so when not.
      return collect_epub_book(
          endpoints->channel(id),
          endpoints->has(pipestream::parse::v1::COLLECTOR_MARKUP)
              ? endpoints->channel(pipestream::parse::v1::COLLECTOR_MARKUP)
              : nullptr,
          bytes, inbound_deadline);
    case pipestream::parse::v1::COLLECTOR_MARKUP: {
      CollectorOutcome outcome = collect_markup_document(
          endpoints->channel(id), filename, content_type, bytes, inbound_deadline);
      // An HTML page's <title> is the document's title; the collector
      // records it as metadata only. Whole pages only: an epub chapter's
      // title is the chapter's, and the book folds those on its own.
      if (outcome.success &&
          markup_format_for(filename, content_type) == pipestream::markup::v1::MARKUP_FORMAT_HTML) {
        promote_source_title(&outcome.document);
      }
      return outcome;
    }
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

CollectorOutcome cancelled_outcome() {
  CollectorOutcome outcome;
  outcome.error = "request cancelled";
  outcome.code = grpc::StatusCode::CANCELLED;
  return outcome;
}

}  // namespace grparse
