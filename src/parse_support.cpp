#include "parse_support.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"
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
    case pipestream::parse::v1::COLLECTOR_CALAMINE: return "GRPARSE_CALAMINE_TARGET";
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
  // Readers accept the header anywhere in the first kilobyte.
  if (std::string_view(content).substr(0, 1024 + 4).find("%PDF-") != std::string_view::npos) {
    return true;
  }
  // Bytes that sniff as something else outrank the name: a PNG called
  // x.pdf is a PNG.
  if (!sniff_mimetype(content).empty()) return false;
  std::string extension = filename.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  return extension == ".pdf";
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
    const std::string& content_type, const std::string& bytes, const CollectorRules& rules,
    CollectorDeadline inbound_deadline, CollectorCancelled cancelled) {
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
                                     inbound_deadline, cancelled, rules.passwords);
    }
    case pipestream::parse::v1::COLLECTOR_ASR:
      if (endpoints->asr_model().empty() &&
          !(rules.asr_options &&
            ((!rules.asr_options->model().empty()) ||
             rules.asr_options->model_type() != pipestream::parse::v1::ASR_MODEL_TYPE_UNSPECIFIED ||
             !rules.asr_options->model_type_raw().empty() ||
             rules.asr_options->has_remote()))) {
        outcome.error = "asr collector has no model configured (GRPARSE_ASR_MODEL)";
        outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
        return outcome;
      }
      return collect_asr_document(endpoints->channel(id), endpoints->asr_model(), filename, bytes,
                                  rules.asr_options, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_EMAIL:
      // An HTML-only message's body folds through the markup collector
      // when one is configured, and the leg says so when not.
      return collect_email_document(
          endpoints->channel(id),
          endpoints->has(pipestream::parse::v1::COLLECTOR_MARKUP)
              ? endpoints->channel(pipestream::parse::v1::COLLECTOR_MARKUP)
              : nullptr,
          document_id, filename, content_type, bytes, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_XML:
      return collect_xml_document(endpoints->channel(id), bytes, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_EBCDIC:
      return collect_ebcdic_document(endpoints->channel(id), rules.ebcdic, bytes,
                                     inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_EPUB:
      // The book, not the skeleton: the chapters fold through the markup
      // collector when one is configured, and the leg says so when not.
      return collect_epub_book(
          endpoints->channel(id),
          endpoints->has(pipestream::parse::v1::COLLECTOR_MARKUP)
              ? endpoints->channel(pipestream::parse::v1::COLLECTOR_MARKUP)
              : nullptr,
          bytes, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_MARKUP: {
      CollectorOutcome outcome = collect_markup_document(
          endpoints->channel(id), filename, content_type, bytes, inbound_deadline, cancelled);
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
      return collect_lol_html_document(endpoints->channel(id), rules.lol_html, bytes,
                                       inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_FASTWARC:
      return collect_fastwarc_document(endpoints->channel(id), bytes, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_PDF:
      // The plain leg, reached when the pdf collector shares a selection
      // with other collectors: its Document is the contribution. The
      // classification-driven routing lives with the plan, not here.
      return collect_pdf_document(endpoints->channel(id), bytes, inbound_deadline, cancelled);
    case pipestream::parse::v1::COLLECTOR_CALAMINE:
      return collect_calamine_document(endpoints->channel(id), bytes, inbound_deadline, cancelled);
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

namespace {

const ai::pipestream::parse::v1::OcrOptionsBase* ocr_options_base(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options) {
  using C = ai::pipestream::parse::v1::ConvertDocumentOptions;
  switch (options.ocr_options_case()) {
    case C::kOcrAuto:
      return &options.ocr_auto().base();
    case C::kEasyocr:
      return &options.easyocr().base();
    case C::kTesseract:
      return &options.tesseract().base();
    case C::kTesseractCli:
      return &options.tesseract_cli().base();
    case C::kRapidocr:
      return &options.rapidocr().base();
    case C::kOcrmac:
      return &options.ocrmac().base();
    case C::kNemotronOcr:
      return &options.nemotron_ocr().base();
    case C::kKserveV2Ocr:
      return &options.kserve_v2_ocr().base();
    case C::OCR_OPTIONS_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

bool ocr_options_engine_unimplemented(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options) {
  using C = ai::pipestream::parse::v1::ConvertDocumentOptions;
  switch (options.ocr_options_case()) {
    case C::kOcrAuto:
    case C::kRapidocr:
    case C::OCR_OPTIONS_NOT_SET:
      return false;
    case C::kEasyocr:
    case C::kTesseract:
    case C::kTesseractCli:
    case C::kOcrmac:
    case C::kNemotronOcr:
    case C::kKserveV2Ocr:
      return true;
  }
  return false;
}

}  // namespace

grpc::Status resolve_replace_broken_text_layer(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    std::optional<bool>* out) {
  if (ocr_options_engine_unimplemented(options)) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "option 'ocr_options' names an OCR engine this server "
                        "does not run (only ocr_auto and rapidocr are served)");
  }
  // These messages also describe engines/configuration this implementation
  // cannot apply. Reject every explicitly present unsupported field, including
  // explicit defaults, rather than silently accepting a different request.
  if (options.has_rapidocr()) {
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    options.rapidocr().GetReflection()->ListFields(options.rapidocr(), &fields);
    for (const auto* field : fields) {
      if (field->name() != "base" && field->name() != "detect_orientation") {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            "option 'rapidocr." + std::string(field->name()) + "' is not implemented");
      }
    }
  }
  std::optional<bool> nested;
  if (const auto* base = ocr_options_base(options)) {
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    base->GetReflection()->ListFields(*base, &fields);
    for (const auto* field : fields) {
      if (field->name() != "replace_broken_text_layer") {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            "option 'ocr_options.base." + std::string(field->name()) + "' is not implemented");
      }
    }
    if (base->has_replace_broken_text_layer()) {
      nested = base->replace_broken_text_layer();
    }
  }
  if (options.has_force_ocr() && options.force_ocr() && nested && *nested) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "option 'force_ocr' true contradicts "
                        "ocr_options.*.base.replace_broken_text_layer true");
  }
  *out = nested;
  return grpc::Status::OK;
}

grpc::Status resolve_rapidocr_detect_orientation(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    std::optional<bool>* out) {
  if (ocr_options_engine_unimplemented(options)) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "option 'ocr_options' names an OCR engine this server "
                        "does not run (only ocr_auto and rapidocr are served)");
  }
  std::optional<bool> nested;
  if (options.has_rapidocr() && options.rapidocr().has_detect_orientation()) {
    nested = options.rapidocr().detect_orientation();
  }
  *out = nested;
  return grpc::Status::OK;
}



namespace {

grpc::StatusCode status_code_for(PdfBackendFailure reason) {
  switch (reason) {
    case PdfBackendFailure::kDeadlineExceeded:
      return grpc::StatusCode::DEADLINE_EXCEEDED;
    case PdfBackendFailure::kCancelled:
      return grpc::StatusCode::CANCELLED;
    case PdfBackendFailure::kResourceExhausted:
      return grpc::StatusCode::RESOURCE_EXHAUSTED;
    case PdfBackendFailure::kUnavailable:
      break;
  }
  return grpc::StatusCode::UNAVAILABLE;
}

}  // namespace

grpc::Status status_from_exception(std::exception_ptr failure) {
  try {
    if (failure) std::rethrow_exception(failure);
  } catch (const PdfBackendUnavailable& error) {
    return grpc::Status(status_code_for(error.reason()), error.what());
  } catch (const InvalidDocument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const PdfBackendNotConfigured& error) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error.what());
  } catch (const SchedulerSaturated& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const SchedulerShuttingDown& error) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, error.what());
  } catch (const std::bad_alloc& error) {
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::invalid_argument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  } catch (const std::exception& error) {
    return grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  } catch (...) {
    // Every caller sits in a catch block that must still finish its call,
    // so nothing may escape from here.
    return grpc::Status(grpc::StatusCode::UNKNOWN, "non-standard exception");
  }
  return grpc::Status::OK;
}

CollectorOutcome outcome_from_exception(std::exception_ptr failure) {
  const grpc::Status status = status_from_exception(std::move(failure));
  CollectorOutcome outcome;
  outcome.error = status.error_message();
  outcome.code = status.error_code();
  return outcome;
}

CollectorOutcome cancelled_outcome() {
  CollectorOutcome outcome;
  outcome.error = "request cancelled";
  outcome.code = grpc::StatusCode::CANCELLED;
  return outcome;
}

}  // namespace grparse
