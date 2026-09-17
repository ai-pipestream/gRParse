#include "source_parse.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>

#include "grparse/base64.h"
#include "grparse/chart_derender.h"
#include "grparse/confidence.h"
#include "grparse/content_sniff.h"
#include "grparse/data_totals.h"
#include "grparse/document_assembly.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"
#include "grparse/heading_hierarchy.h"
#include "grparse/input_format.h"
#include "grparse/page_previews.h"
#include "grparse/schema_version.h"
#include "grparse/vlm_convert.h"
#include "parse_support.h"

namespace fs = std::filesystem;
namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// Stamps the origin's mimetype and what it rests on: the request's own
// content type first, the bytes next, the name last (content_sniff.h).
void stamp_origin_mimetype(const std::string& declared_content_type,
                           const std::string& bytes, const fs::path& filename,
                           pipestream::document::v1::DocumentOrigin* origin) {
  const MimetypeResolution resolved = resolve_mimetype(declared_content_type, bytes, filename);
  origin->set_mimetype(resolved.mimetype);
  origin->set_mimetype_evidence(resolved.evidence);
  if (resolved.evidence == "magic") {
    data_counters().mimetypes_sniffed.fetch_add(1, std::memory_order_relaxed);
    data_log("origin " + filename.string() + " mimetype " + resolved.mimetype +
             " from the bytes");
  }
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

std::vector<pipestream::parse::v1::Collector> requested_collectors(
    const google::protobuf::RepeatedField<int>& raw) {
  std::vector<pipestream::parse::v1::Collector> collectors;
  collectors.reserve(raw.size());
  for (const int value : raw) {
    collectors.push_back(static_cast<pipestream::parse::v1::Collector>(value));
  }
  return collectors;
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
    case pipestream::parse::v1::OUTPUT_FORMAT_LATEX:
      return true;
    default:
      return false;
  }
}

// The options every conversion surface implements. Anything else populated
// on the request is rejected by name (below): an option this server would
// silently ignore is worse than one it turns down.
bool implemented_option(std::string_view name) {
  static constexpr std::string_view kImplemented[] = {
      "from_formats",
      "to_formats",
      "collectors",
      "ebcdic_layout_json",
      "lol_html_options_json",
      "do_ocr",
      "force_ocr",
      "render_scale",
      "pipeline",
      "include_page_images",
      "md_page_break_placeholder",
      "md_compact_tables",
      "do_pdf_heading_hierarchy",
      "pdf_heading_hierarchy_options",
      "document_timeout",
      "page_range",
      "include_images",
      "images_scale",
      "image_export_mode",
      "ocr_engine",
      "do_table_structure",
      "table_mode",
      "do_picture_classification",
      "ocr_lang",
      "pdf_backend",
      "table_cell_matching",
      "abort_on_error",
      "do_chart_extraction",
      "do_picture_description",
      "picture_description_area_threshold",
      "picture_description_preset",
      "picture_description_local",
      "picture_description_api",
      "do_code_enrichment",
      "do_formula_enrichment",
      "code_formula_preset",
      // Accepted for Docling clients that always populate them. PROCESSING_PIPELINE_VLM
      // dials grpc-vlm-convert when GRPARSE_VLM_CONVERT_TARGET is set. Typed
      // *_custom_config messages and open ScalarValue maps are accepted; values
      // are applied where a local dial exists (e.g. classification threshold).
      "vlm_pipeline_model",
      "vlm_pipeline_model_local",
      "vlm_pipeline_model_api",
      "vlm_pipeline_preset",
      "ocr_preset",
      "table_structure_preset",
      "layout_preset",
      "picture_classification_preset",
      "vlm_pipeline_custom_config",
      "picture_description_custom_config",
      "code_formula_custom_config",
      "table_structure_custom_config",
      "layout_custom_config",
      "ocr_custom_config",
      "picture_classification_custom_config",
  };
  return std::ranges::find(kImplemented, name) != std::end(kImplemented);
}

// The pipelines this server runs: STANDARD is the default path, NATIVE the
// model-free extraction the pdf collector's own Document provides, VLM the
// grpc-vlm-convert leg (availability checked at parse time against
// GRPARSE_VLM_CONVERT_TARGET). ASR and LEGACY name engines this server does
// not host.
grpc::Status validate_pipeline(const pipestream::parse::v1::ConvertDocumentOptions& options,
                               const std::string& surface) {
  if (!options.has_pipeline()) return grpc::Status::OK;
  switch (options.pipeline()) {
    case pipestream::parse::v1::PROCESSING_PIPELINE_UNSPECIFIED:
    case pipestream::parse::v1::PROCESSING_PIPELINE_STANDARD:
    case pipestream::parse::v1::PROCESSING_PIPELINE_NATIVE:
    case pipestream::parse::v1::PROCESSING_PIPELINE_VLM:
      return grpc::Status::OK;
    default: {
      std::string name = pipestream::parse::v1::ProcessingPipeline_Name(options.pipeline());
      if (name.empty()) name = std::to_string(static_cast<int>(options.pipeline()));
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement pipeline '" + name + "'");
    }
  }
}

// RapidOCR is the only recognizer this binary hosts. AUTO and UNSPECIFIED
// resolve to it; every other named engine is rejected so a caller asking for
// EasyOCR/Tesseract does not silently get a different stack.
grpc::Status validate_ocr_engine(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                 const std::string& surface) {
  if (!options.has_ocr_engine()) return grpc::Status::OK;
  switch (options.ocr_engine()) {
    case pipestream::parse::v1::OCR_ENGINE_UNSPECIFIED:
    case pipestream::parse::v1::OCR_ENGINE_AUTO:
    case pipestream::parse::v1::OCR_ENGINE_RAPIDOCR:
      return grpc::Status::OK;
    default: {
      std::string name = pipestream::parse::v1::OcrEngine_Name(options.ocr_engine());
      if (name.empty()) name = std::to_string(static_cast<int>(options.ocr_engine()));
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement ocr_engine '" + name + "'");
    }
  }
}

// One RapidTable model is installed; FAST and ACCURATE both use it. The
// field is accepted so Docling clients that always set table_mode are not
// turned away for a distinction this binary does not host.
grpc::Status validate_table_mode(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                 const std::string& surface) {
  if (!options.has_table_mode()) return grpc::Status::OK;
  if (pipestream::parse::v1::TableFormerMode_IsValid(options.table_mode())) {
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      surface + " table_mode value is not a known TableFormerMode");
}

// PDF rasterization here is poppler (or an optional remote pdf backend), not
// Docling's Python backends. Named PdfBackend values are accepted so clients
// that always set the field are not turned away; the engine stays poppler.
grpc::Status validate_pdf_backend(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                  const std::string& surface) {
  if (!options.has_pdf_backend()) return grpc::Status::OK;
  if (pipestream::parse::v1::PdfBackend_IsValid(options.pdf_backend())) {
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      surface + " pdf_backend value is not a known PdfBackend");
}

// Nested picture-description engines map onto enrich fields this binary can
// forward (repo_id / url / timeout / concurrency). Local and API are
// mutually exclusive. Prompt, headers, params, and generation_config have no
// enrich wire and are rejected when set.
grpc::Status validate_picture_description_engines(
    const pipestream::parse::v1::ConvertDocumentOptions& options, const std::string& surface) {
  if (options.has_picture_description_local() && options.has_picture_description_api()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": picture_description_local and "
                                  "picture_description_api are mutually exclusive");
  }
  if (options.has_picture_description_local()) {
    const auto& local = options.picture_description_local();
    if (local.repo_id().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_local.repo_id is required");
    }
    if (local.has_prompt()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement picture_description_local.prompt");
    }
    if (!local.generation_config().empty()) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          surface + " does not implement picture_description_local.generation_config");
    }
  }
  if (options.has_picture_description_api()) {
    const auto& api = options.picture_description_api();
    if (api.url().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.url is required");
    }
    if (!api.headers().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement picture_description_api.headers");
    }
    if (!api.params().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement picture_description_api.params");
    }
    if (api.has_prompt()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement picture_description_api.prompt");
    }
    if (api.has_timeout() && api.timeout() <= 0.0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.timeout must be positive");
    }
    if (api.has_concurrency() && api.concurrency() < 1) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.concurrency must be >= 1");
    }
  }
  return grpc::Status::OK;
}

// VLM selection fields are accepted so Docling clients that always set them
// are not turned away. They are mutually exclusive (preset / enum / local /
// api string). PROCESSING_PIPELINE_VLM uses them when the convert peer is
// configured.
grpc::Status validate_vlm_selection(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                    const std::string& surface) {
  int engines = 0;
  if (options.has_vlm_pipeline_model() &&
      options.vlm_pipeline_model() != pipestream::parse::v1::VLM_MODEL_TYPE_UNSPECIFIED) {
    ++engines;
  }
  if (options.has_vlm_pipeline_model_local() && !options.vlm_pipeline_model_local().empty()) {
    ++engines;
  }
  if (options.has_vlm_pipeline_model_api() && !options.vlm_pipeline_model_api().empty()) {
    ++engines;
  }
  if (options.has_vlm_pipeline_preset() && !options.vlm_pipeline_preset().empty()) {
    ++engines;
  }
  if (engines > 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": vlm_pipeline_model, vlm_pipeline_model_local, "
                                  "vlm_pipeline_model_api, and vlm_pipeline_preset "
                                  "are mutually exclusive");
  }
  if (options.has_vlm_pipeline_model() &&
      !pipestream::parse::v1::VlmModelType_IsValid(options.vlm_pipeline_model())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " vlm_pipeline_model value is not a known VlmModelType");
  }
  return grpc::Status::OK;
}

// Custom-config Structs mirror Docling's open dict bags. Known keys are
// applied; any other key is rejected by name (never silently dropped). Empty
// Structs are accepted.
grpc::Status validate_custom_configs(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                     const std::string& surface) {
  const auto scalar_number =
      [](const pipestream::parse::v1::ScalarValue& value) -> std::optional<double> {
    if (value.has_double_value()) return value.double_value();
    if (value.has_int_value()) return static_cast<double>(value.int_value());
    if (value.has_uint_value()) return static_cast<double>(value.uint_value());
    return std::nullopt;
  };
  const auto require_engine = [&](pipestream::parse::v1::VlmEngineType engine_type,
                                  const char* name) -> grpc::Status {
    if (engine_type == pipestream::parse::v1::VLM_ENGINE_TYPE_UNSPECIFIED) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": " + std::string(name) +
                              ".engine_options.engine_type is required");
    }
    return grpc::Status::OK;
  };
  const auto require_model_spec = [&](const pipestream::parse::v1::VlmModelSpec& spec,
                                      const char* name) -> grpc::Status {
    if (spec.name().empty() || spec.default_repo_id().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": " + std::string(name) +
                              ".model_spec requires name and default_repo_id");
    }
    return grpc::Status::OK;
  };

  if (options.has_vlm_pipeline_custom_config()) {
    const auto& cfg = options.vlm_pipeline_custom_config();
    if (auto s = require_engine(cfg.engine_options().engine_type(), "vlm_pipeline_custom_config");
        !s.ok()) {
      return s;
    }
    if (auto s = require_model_spec(cfg.model_spec(), "vlm_pipeline_custom_config"); !s.ok()) {
      return s;
    }
  }
  if (options.has_picture_description_custom_config()) {
    const auto& cfg = options.picture_description_custom_config();
    if (auto s =
            require_engine(cfg.engine_options().engine_type(), "picture_description_custom_config");
        !s.ok()) {
      return s;
    }
    if (auto s = require_model_spec(cfg.model_spec(), "picture_description_custom_config");
        !s.ok()) {
      return s;
    }
    if (cfg.has_classification_min_confidence() &&
        (cfg.classification_min_confidence() < 0.0 || cfg.classification_min_confidence() > 1.0)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          surface +
              ": picture_description_custom_config.classification_min_confidence must be in [0, 1]");
    }
  }
  if (options.has_code_formula_custom_config()) {
    const auto& cfg = options.code_formula_custom_config();
    if (auto s = require_engine(cfg.engine_options().engine_type(), "code_formula_custom_config");
        !s.ok()) {
      return s;
    }
    if (auto s = require_model_spec(cfg.model_spec(), "code_formula_custom_config"); !s.ok()) {
      return s;
    }
  }

  // Open ScalarValue maps: any key is accepted (Docling dict[str, Any] parity).
  // Soft-validate well-known keys when present.
  if (auto it = options.ocr_custom_config().find("lang"); it != options.ocr_custom_config().end()) {
    if (!it->second.has_string_value() || it->second.string_value().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": ocr_custom_config.lang must be a non-empty string");
    }
  }
  if (auto it = options.picture_classification_custom_config().find("threshold");
      it != options.picture_classification_custom_config().end()) {
    const auto number = scalar_number(it->second);
    if (!number.has_value() || *number < 0.0 || *number > 1.0) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          surface + ": picture_classification_custom_config.threshold must be in [0, 1]");
    }
  }
  (void)options.table_structure_custom_config();
  (void)options.layout_custom_config();
  return grpc::Status::OK;
}

// The heading pass's two switches must agree when both are given, and the
// numeric tunables must be in range; the rest of the message is accepted as
// documented on HeadingHierarchyOptions.
grpc::Status validate_heading_options(
    const pipestream::parse::v1::ConvertDocumentOptions& options, const std::string& surface) {
  if (!options.has_pdf_heading_hierarchy_options()) return grpc::Status::OK;
  const auto& heading = options.pdf_heading_hierarchy_options();
  if (options.has_do_pdf_heading_hierarchy() && heading.has_enabled() &&
      options.do_pdf_heading_hierarchy() != heading.enabled()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": do_pdf_heading_hierarchy and "
                                  "pdf_heading_hierarchy_options.enabled disagree");
  }
  if (heading.has_max_level() && (heading.max_level() < 1 || heading.max_level() > 6)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": pdf_heading_hierarchy_options.max_level must be in [1, 6]");
  }
  if (heading.has_style_size_tolerance() &&
      (heading.style_size_tolerance() < 0.0 || heading.style_size_tolerance() >= 1.0)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": pdf_heading_hierarchy_options.style_size_tolerance must "
                                  "be in [0, 1)");
  }
  return grpc::Status::OK;
}

// `surface` names the RPC in the rejections so a caller learns which of the
// conversion surfaces turned its request down.
grpc::Status validate_options(const pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface) {
  std::vector<const google::protobuf::FieldDescriptor*> populated;
  options.GetReflection()->ListFields(options, &populated);
  for (const auto* field : populated) {
    if (!implemented_option(field->name())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " does not implement option '" + std::string(field->name()) + "'");
    }
  }
  const grpc::Status tuning_status =
      validate_ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                          options.has_render_scale(), options.render_scale());
  if (!tuning_status.ok()) return tuning_status;
  const grpc::Status pipeline_status = validate_pipeline(options, surface);
  if (!pipeline_status.ok()) return pipeline_status;
  const grpc::Status ocr_engine_status = validate_ocr_engine(options, surface);
  if (!ocr_engine_status.ok()) return ocr_engine_status;
  const grpc::Status table_mode_status = validate_table_mode(options, surface);
  if (!table_mode_status.ok()) return table_mode_status;
  const grpc::Status pdf_backend_status = validate_pdf_backend(options, surface);
  if (!pdf_backend_status.ok()) return pdf_backend_status;
  const grpc::Status picture_engine_status =
      validate_picture_description_engines(options, surface);
  if (!picture_engine_status.ok()) return picture_engine_status;
  const grpc::Status vlm_selection_status = validate_vlm_selection(options, surface);
  if (!vlm_selection_status.ok()) return vlm_selection_status;
  const grpc::Status custom_config_status = validate_custom_configs(options, surface);
  if (!custom_config_status.ok()) return custom_config_status;
  const grpc::Status heading_status = validate_heading_options(options, surface);
  if (!heading_status.ok()) return heading_status;
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
  for (const auto raw : options.from_formats()) {
    const auto format = static_cast<pipestream::parse::v1::InputFormat>(raw);
    if (format == pipestream::parse::v1::INPUT_FORMAT_UNSPECIFIED ||
        !pipestream::parse::v1::InputFormat_IsValid(raw)) {
      std::string name = pipestream::parse::v1::InputFormat_Name(format);
      if (name.empty()) name = std::to_string(raw);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " from_formats contains invalid value '" + name + "'");
    }
  }
  // Docling page_range is a (start, end) tuple; on this wire that is exactly
  // two 1-indexed ints. Empty means the whole document.
  if (options.page_range_size() != 0 && options.page_range_size() != 2) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " page_range must be empty or [start, end]");
  }
  if (options.page_range_size() == 2) {
    const int start = options.page_range(0);
    const int end = options.page_range(1);
    if (start < 1 || end < start) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " page_range must be a 1-indexed inclusive span");
    }
  }
  if (options.has_images_scale() && !(options.images_scale() > 0.0)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " images_scale must be positive");
  }
  if (options.has_image_export_mode()) {
    switch (options.image_export_mode()) {
      case pipestream::parse::v1::IMAGE_REF_MODE_UNSPECIFIED:
      case pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED:
      case pipestream::parse::v1::IMAGE_REF_MODE_PLACEHOLDER:
      case pipestream::parse::v1::IMAGE_REF_MODE_REFERENCED:
        break;
      default: {
        std::string name =
            pipestream::parse::v1::ImageRefMode_Name(options.image_export_mode());
        if (name.empty()) name = std::to_string(static_cast<int>(options.image_export_mode()));
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + " does not implement image_export_mode '" + name + "'");
      }
    }
  }
  return validate_document_timeout(options.has_document_timeout(), options.document_timeout(),
                                   surface);
}

// The document every collector's output merges into, additively and in plan
// order. It carries identity and nothing else: the schema name and version
// name the wire schema minor this repo currently mirrors, and must match
// what every other producer stamps on its documents.
pipestream::document::v1::Document base_document(const std::string& bytes,
                                                 const fs::path& requested_name) {
  pipestream::document::v1::Document base;
  base.set_schema_name(kWireSchemaName);
  base.set_version(kUpstreamSchemaVersion);
  base.set_name(requested_name.filename().string());
  auto* origin = base.mutable_origin();
  origin->set_filename(requested_name.filename().string());
  // A FileSource declares no content type; the bytes speak before the name.
  // The resolved type is the origin's and, below, the routing's and every
  // dialed collector's: what the bytes are is decided once.
  stamp_origin_mimetype(std::string(), bytes, requested_name, origin);
  origin->set_binary_hash(content_hash(bytes));
  // The stamp is the service's own claim: attributed like any other, and
  // ranked above every collector's, so no collector's idea of the
  // filename or the hash displaces what the request said.
  pipestream::document::v1::CollectorSource stamp;
  stamp.set_collector("grparse");
  claim_fields(origin, stamp);
  auto* stamp_claim = base.add_claims();
  *stamp_claim->mutable_source() = stamp;
  *stamp_claim->mutable_origin() = *origin;
  stamp_claim->mutable_origin()->clear_field_sources();
  base.mutable_body()->set_self_ref("#/body");
  base.mutable_body()->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
  base.mutable_furniture()->set_self_ref("#/furniture");
  base.mutable_furniture()->set_content_layer(
      pipestream::document::v1::CONTENT_LAYER_FURNITURE);
  return base;
}

// The CV path is the only collector that knows where its text lands in the
// document's text stream. Its offset rows are kept here and only published
// when that collector turns out to be the whole document.
using CvOffsets =
    std::shared_ptr<google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>;
// Likewise the only collector that measured its pages: its read quality is
// kept here and published on the response whenever it read any page.
using CvConfidence = std::shared_ptr<std::optional<pipestream::parse::v1::ConfidenceScores>>;

// The in-process CV collector: the page scheduler's layout, OCR, and model
// pipeline over rendered pages, assembled into a document fragment. Never
// throws; failures become the outcome. The tuning rides as a call parameter
// because the pdf routing leg re-enters this path with the inspector's OCR
// page set applied.
class CvCollector {
 public:
  CvCollector(grpc::CallbackServerContext* context, PageScheduler& scheduler,
              std::shared_ptr<const std::string> bytes, bool pdf, CvOffsets offsets,
              CvConfidence confidence, HeadingOptions heading_options)
      : context_(context),
        scheduler_(scheduler),
        bytes_(std::move(bytes)),
        pdf_(pdf),
        offsets_(std::move(offsets)),
        confidence_(std::move(confidence)),
        heading_options_(std::move(heading_options)) {}

  CollectorOutcome operator()(const PageScheduler::OcrTuning& tuning) const {
    try {
      PageSet pages;
      if (std::optional<CollectorOutcome> failure = collect_pages(tuning, &pages)) {
        return std::move(*failure);
      }
      return assemble(pages);
    } catch (...) {
      CollectorOutcome outcome;
      const grpc::Status status = status_from_exception(std::current_exception());
      outcome.error = status.error_message();
      outcome.code = status.error_code();
      return outcome;
    }
  }

 private:
  // Every page one submission produced, in page order.
  struct PageSet {
    std::map<int, std::shared_ptr<const OcrPage>> pages;
    int total_pages = 0;
  };

  // The scheduler's own state while a submission runs: the pages as they
  // land, the failure that ended it, and whether it has ended at all.
  struct Run {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<int, std::shared_ptr<const OcrPage>> pages;
    std::exception_ptr failure;
    int total_pages = 0;
    bool finished = false;
  };

  // Submits the document and waits for the scheduler to finish with it,
  // cancelling the ticket as soon as the call goes away. Returns the outcome
  // that ended the run, or nothing when every page arrived.
  std::optional<CollectorOutcome> collect_pages(const PageScheduler::OcrTuning& tuning,
                                                PageSet* collected) const {
    Run state;
    auto* context = context_;
    const auto ticket = scheduler_.submit(
        bytes_, pdf_, tuning,
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
      if (context_->IsCancelled()) ticket.cancel();
    }
    if (context_->IsCancelled()) return cancelled_outcome();
    const grpc::Status scheduler_status = status_from_exception(state.failure);
    if (!scheduler_status.ok()) {
      CollectorOutcome outcome;
      outcome.error = scheduler_status.error_message();
      outcome.code = scheduler_status.error_code();
      return outcome;
    }
    if (state.total_pages <= 0 || state.pages.size() != static_cast<size_t>(state.total_pages)) {
      CollectorOutcome outcome;
      outcome.error = "scheduler completed before every page was available";
      return outcome;
    }
    collected->pages = std::move(state.pages);
    collected->total_pages = state.total_pages;
    return std::nullopt;
  }

  // Folds the collected pages into one document fragment and keeps the
  // offset rows its text stream produced.
  CollectorOutcome assemble(const PageSet& collected) const {
    CollectorOutcome outcome;
    std::string plain_text;
    AssemblyCursor assembly_cursor;
    google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset> offsets;
    std::vector<const OcrPage*> assembled_pages;
    assembled_pages.reserve(collected.pages.size());
    for (const auto& [page_number, page] : collected.pages) {
      append_page_to_document(*page, page_number, &assembly_cursor,
                              &outcome.document, &plain_text, &offsets,
                              &outcome.warnings);
      assembled_pages.push_back(page.get());
    }
    // A PDF read through several backends claims its vote: one aggregate
    // "protomolt" claim for the document, no-op when nothing voted.
    append_consensus_claim(assembled_pages, &outcome.document);
    // Heading depth clusters over the whole document's heights, so it can
    // only run after every page is in.
    assign_section_header_levels(&outcome.document, heading_options_);
    std::vector<PageConfidence> page_scores;
    page_scores.reserve(assembled_pages.size());
    for (const OcrPage* page : assembled_pages) page_scores.push_back(page_confidence(*page));
    *confidence_ = document_confidence(page_scores);
    *offsets_ = std::move(offsets);
    outcome.success = true;
    return outcome;
  }

  grpc::CallbackServerContext* context_;
  PageScheduler& scheduler_;
  std::shared_ptr<const std::string> bytes_;
  bool pdf_;
  CvOffsets offsets_;
  CvConfidence confidence_;
  HeadingOptions heading_options_;
};

// Everything one parse's collector legs read: the request's bytes and
// identity, the endpoints they dial, and the ceilings they run under. Copied
// into each leg, which is why every member is a value or a shared handle.
struct ParseInputs {
  grpc::CallbackServerContext* context = nullptr;
  std::shared_ptr<CollectorEndpoints> endpoints;
  std::shared_ptr<const std::string> bytes;
  std::shared_ptr<const std::string> ebcdic_layout_json;
  std::shared_ptr<const std::string> lol_html_options_json;
  fs::path filename;
  std::string content_type;
  PageScheduler::OcrTuning tuning;
  CollectorDeadline inbound_deadline = kNoCollectorDeadline;
  bool previews = false;
  // PROCESSING_PIPELINE_NATIVE: the pdf collector's own model-free Document
  // is the answer whatever its classification said.
  bool native_pipeline = false;
  // PROCESSING_PIPELINE_VLM: grpc-vlm-convert produces the body.
  bool vlm_pipeline = false;
  HeadingOptions heading;
  // Docling enrichment switches for the post-parse enrich dial. Unset chart
  // extraction keeps the env opt-in (run when GRPARSE_ENRICH_TARGET is set);
  // false skips; true runs when configured. Picture/code/formula default off
  // unless the request sets them true.
  std::optional<bool> do_chart_extraction;
  bool do_picture_description = false;
  bool do_code_enrichment = false;
  bool do_formula_enrichment = false;
  double picture_description_area_threshold = 0.0;
  std::string picture_description_preset;
  std::string code_formula_preset;
  // From picture_description_api.url / local.repo_id when set; empty means keep
  // the enrich service (or env) default.
  std::string picture_description_vlm_endpoint;
  std::optional<uint32_t> enrich_concurrency;
  std::optional<std::chrono::milliseconds> enrich_timeout;
  std::vector<std::string> picture_description_allow;
  std::vector<std::string> picture_description_deny;
  double picture_description_min_confidence = 0.0;
};

// Docling PictureClassificationLabel → figure-classifier class_name strings.
std::string classification_class_name(pipestream::parse::v1::PictureClassificationLabel label) {
  switch (label) {
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_BAR_CHART: return "bar_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_BOX_PLOT: return "box_plot";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_FLOW_CHART: return "flow_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_LINE_CHART: return "line_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_PIE_CHART: return "pie_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SCATTER_PLOT: return "scatter_plot";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_TABLE: return "table";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_OTHER_CHART: return "other_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_FULL_PAGE_IMAGE:
      return "full_page_image";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_PAGE_THUMBNAIL:
      return "page_thumbnail";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_PHOTOGRAPH: return "photograph";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_CHEMISTRY_STRUCTURE:
      return "chemistry_structure";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_BAR_CODE: return "bar_code";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_ICON: return "icon";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_LOGO: return "logo";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_QR_CODE: return "qr_code";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SIGNATURE: return "signature";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_STAMP: return "stamp";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_ENGINEERING_DRAWING:
      return "engineering_drawing";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SCREENSHOT_FROM_COMPUTER:
      return "screenshot_from_computer";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SCREENSHOT_FROM_MANUAL:
      return "screenshot_from_manual";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_GEOGRAPHICAL_MAP:
      return "geographical_map";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_TOPOGRAPHICAL_MAP:
      return "topographical_map";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_CALENDAR: return "calendar";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_CROSSWORD_PUZZLE:
      return "crossword_puzzle";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_MUSIC: return "music";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_OTHER: return "other";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_CAD_DRAWING: return "cad_drawing";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_ELECTRICAL_DIAGRAM:
      return "electrical_diagram";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_GEOGRAPHIC_MAP: return "map";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_HEATMAP: return "heatmap";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_MARKUSH_STRUCTURE:
      return "chemistry_markush_structure";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_MOLECULAR_STRUCTURE:
      return "chemistry_molecular_structure";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_NATURAL_IMAGE: return "natural_image";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_PICTURE_GROUP: return "picture_group";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_REMOTE_SENSING: return "remote_sensing";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SCATTER_CHART: return "scatter_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_SCREENSHOT: return "screenshot";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_STACKED_BAR_CHART:
      return "stacked_bar_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_STRATIGRAPHIC_CHART:
      return "stratigraphic_chart";
    case pipestream::parse::v1::PICTURE_CLASSIFICATION_LABEL_UNSPECIFIED:
    default:
      return std::string();
  }
}

std::vector<std::string> classification_class_names(
    const google::protobuf::RepeatedField<int>& labels) {
  std::vector<std::string> names;
  for (const int raw : labels) {
    const auto label = static_cast<pipestream::parse::v1::PictureClassificationLabel>(raw);
    std::string name = classification_class_name(label);
    if (!name.empty()) names.push_back(std::move(name));
  }
  return names;
}

// The heading pass's tuning as the request states it; every unset field
// keeps the pass's own default (HeadingOptions).
HeadingOptions heading_options_from(const pipestream::parse::v1::ConvertDocumentOptions& options) {
  HeadingOptions heading;
  if (options.has_do_pdf_heading_hierarchy()) heading.enabled = options.do_pdf_heading_hierarchy();
  if (!options.has_pdf_heading_hierarchy_options()) return heading;
  const auto& requested = options.pdf_heading_hierarchy_options();
  if (requested.has_enabled()) heading.enabled = requested.enabled();
  if (requested.has_use_numbering()) heading.use_numbering = requested.use_numbering();
  if (requested.has_use_style()) heading.use_style = requested.use_style();
  if (requested.has_max_level()) heading.max_level = requested.max_level();
  if (requested.has_style_size_tolerance()) {
    heading.style_size_tolerance = requested.style_size_tolerance();
  }
  return heading;
}

// One parse's inputs, read off the request once. The mimetype is the
// origin's own resolved type, so the routing and every dialed collector see
// what the bytes were decided to be.
ParseInputs parse_inputs(grpc::CallbackServerContext* context,
                         const pipestream::parse::v1::ConvertDocumentRequest& request,
                         const PageScheduler& scheduler,
                         const std::shared_ptr<CollectorEndpoints>& collectors,
                         std::shared_ptr<const std::string> bytes,
                         const fs::path& requested_name, std::string content_type) {
  const auto& options = request.options();
  ParseInputs inputs;
  inputs.context = context;
  inputs.endpoints = collectors;
  inputs.bytes = std::move(bytes);
  inputs.ebcdic_layout_json = std::make_shared<const std::string>(options.ebcdic_layout_json());
  inputs.lol_html_options_json =
      std::make_shared<const std::string>(options.lol_html_options_json());
  inputs.filename = requested_name;
  inputs.content_type = std::move(content_type);
  inputs.tuning = ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                             options.has_render_scale(), options.render_scale());
  if (options.page_range_size() == 2) {
    inputs.tuning.page_range = std::make_pair(options.page_range(0), options.page_range(1));
  }
  if (options.has_include_images()) {
    inputs.tuning.capture_picture_images = options.include_images();
  }
  if (options.has_images_scale()) {
    inputs.tuning.images_scale = options.images_scale();
  }
  if (options.has_do_table_structure()) {
    inputs.tuning.do_table_structure = options.do_table_structure();
  }
  if (options.has_do_picture_classification()) {
    inputs.tuning.do_picture_classification = options.do_picture_classification();
  }
  if (options.has_do_chart_extraction()) {
    inputs.do_chart_extraction = options.do_chart_extraction();
  }
  if (options.has_do_picture_description()) {
    inputs.do_picture_description = options.do_picture_description();
  }
  if (options.has_do_code_enrichment()) {
    inputs.do_code_enrichment = options.do_code_enrichment();
  }
  if (options.has_do_formula_enrichment()) {
    inputs.do_formula_enrichment = options.do_formula_enrichment();
  }
  if (options.has_picture_description_area_threshold()) {
    inputs.picture_description_area_threshold = options.picture_description_area_threshold();
  }
  if (options.has_picture_description_preset()) {
    inputs.picture_description_preset = options.picture_description_preset();
  }
  if (options.has_code_formula_preset()) {
    inputs.code_formula_preset = options.code_formula_preset();
  }
  if (options.has_picture_description_local()) {
    // repo_id is the enrich raw preset name; presence of local does not force
    // do_picture_description — the Convert bool still gates the job.
    const auto& local = options.picture_description_local();
    inputs.picture_description_preset = local.repo_id();
    inputs.picture_description_allow = classification_class_names(local.classification_allow());
    inputs.picture_description_deny = classification_class_names(local.classification_deny());
    if (local.has_classification_min_confidence()) {
      inputs.picture_description_min_confidence = local.classification_min_confidence();
    }
  }
  if (options.has_picture_description_api()) {
    const auto& api = options.picture_description_api();
    inputs.picture_description_vlm_endpoint = api.url();
    if (api.has_concurrency()) {
      inputs.enrich_concurrency = static_cast<uint32_t>(api.concurrency());
    }
    if (api.has_timeout()) {
      inputs.enrich_timeout = std::chrono::milliseconds(
          static_cast<int64_t>(std::ceil(api.timeout() * 1000.0)));
    }
    inputs.picture_description_allow = classification_class_names(api.classification_allow());
    inputs.picture_description_deny = classification_class_names(api.classification_deny());
    if (api.has_classification_min_confidence()) {
      inputs.picture_description_min_confidence = api.classification_min_confidence();
    }
  }
  if (auto it = options.picture_classification_custom_config().find("threshold");
      it != options.picture_classification_custom_config().end() &&
      inputs.picture_description_min_confidence <= 0.0) {
    if (it->second.has_double_value()) {
      inputs.picture_description_min_confidence = it->second.double_value();
    } else if (it->second.has_int_value()) {
      inputs.picture_description_min_confidence = static_cast<double>(it->second.int_value());
    }
  }
  if (options.has_picture_description_custom_config()) {
    const auto& cfg = options.picture_description_custom_config();
    if (inputs.picture_description_allow.empty()) {
      inputs.picture_description_allow = classification_class_names(cfg.classification_allow());
    }
    if (inputs.picture_description_deny.empty()) {
      inputs.picture_description_deny = classification_class_names(cfg.classification_deny());
    }
    if (cfg.has_classification_min_confidence() &&
        inputs.picture_description_min_confidence <= 0.0) {
      inputs.picture_description_min_confidence = cfg.classification_min_confidence();
    }
  }
  // ocr_custom_config.lang is accepted (validated) for Docling clients that
  // put languages there; RapidOCR here already accepts ocr_lang and does not
  // need a second path.
  // Every dialed leg inherits this call's own ceiling, so no collector is
  // waited on past the patience of the client that asked for the parse. A
  // call with no deadline yields time_point::max(), which leaves each leg on
  // its own static cap exactly as before. document_timeout (seconds) further
  // caps that ceiling when set — parity with Docling Convert options.
  inputs.inbound_deadline = deadline_with_document_timeout(
      context->deadline(), options.has_document_timeout(), options.document_timeout());
  // A collector-folded PDF never rasterized; when previews are on, it gets
  // them rendered so the shell has a page to paint the boxes on. The request
  // decides when it says; the server setting otherwise.
  if (options.has_include_page_images()) {
    inputs.tuning.capture_page_images = options.include_page_images();
    inputs.previews = options.include_page_images();
  } else {
    inputs.previews = scheduler.captures_page_images();
  }
  inputs.native_pipeline =
      options.has_pipeline() &&
      options.pipeline() == pipestream::parse::v1::PROCESSING_PIPELINE_NATIVE;
  inputs.vlm_pipeline =
      options.has_pipeline() &&
      options.pipeline() == pipestream::parse::v1::PROCESSING_PIPELINE_VLM;
  inputs.heading = heading_options_from(options);
  return inputs;
}

// The pdf routing leg: the inspector's classification decides between the
// collector's own fast-path Document and a CV run restricted to the pages it
// named as needing OCR. A failed classification degrades to the unrouted CV
// path, never to a failed parse.
CollectorOutcome route_pdf_leg(const ParseInputs& inputs, const CvCollector& run_cv) {
  // Same pre-dial cancellation check as the plain collector legs: a call
  // that died after the dequeue check must not dial the inspector either.
  if (inputs.context->IsCancelled()) return cancelled_outcome();
  const PdfParseResult parsed =
      collect_pdf(inputs.endpoints->channel(pipestream::parse::v1::COLLECTOR_PDF),
                  *inputs.bytes, inputs.inbound_deadline, inputs.tuning.page_range);
  const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
  if (parsed.outcome.success && (route.fast_path || inputs.native_pipeline)) {
    PdfParseResult fast = parsed;
    if (inputs.previews) attach_page_previews(inputs.bytes, &fast.outcome.document);
    if (!route.fast_path) {
      // NATIVE asked for the text layer as it is; say what the models would
      // have been run for, so a caller can tell a thin result from a thin
      // document.
      fast.outcome.warnings.push_back(
          "pipeline NATIVE took the pdf collector's extraction although the inspector "
          "classified the document as " +
          std::string(pdf_class_name(parsed.classification.pdf_class)) +
          (parsed.classification.encoding_issues ? " with encoding issues in the text layer"
                                                 : "") +
          "; no layout, OCR, or table-structure model ran");
    }
    return fast.outcome;
  }
  if (!parsed.outcome.success && inputs.native_pipeline) {
    // Degrading to the CV path would run the models NATIVE excludes.
    CollectorOutcome outcome;
    outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    outcome.error = "pipeline NATIVE needs the pdf collector's extraction and it failed: " +
                    parsed.outcome.error;
    return outcome;
  }
  if (!parsed.outcome.success) {
    // Degrade, don't sink: an unreachable inspector leaves the parse on
    // exactly the path it would have taken without the collector.
    CollectorOutcome outcome = run_cv(inputs.tuning);
    outcome.warnings.push_back("pdf collector failed (" + parsed.outcome.error +
                               "); fell back to the in-process CV path");
    return outcome;
  }
  PageScheduler::OcrTuning routed_tuning = inputs.tuning;
  routed_tuning.ocr_pages.insert(route.ocr_pages.begin(), route.ocr_pages.end());
  const bool forced =
      route.force_ocr && routed_tuning.mode == PageScheduler::OcrTuning::Mode::kSelective;
  if (forced) routed_tuning.mode = PageScheduler::OcrTuning::Mode::kForce;
  CollectorOutcome outcome = run_cv(routed_tuning);
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
}

// The collectors one request runs, and whether the pdf collector owns the
// plan whole, which is what turns classification routing on.
struct RoutedPlan {
  std::vector<pipestream::parse::v1::Collector> ids;
  bool pdf_routing = false;
  // True when the routed (not explicitly selected) plan fanned out to the
  // secondary office collectors; the legs append_office_fanout adds are
  // always poi or calamine, never the routed primary itself.
  bool office_fanout = false;
};

// The default PDF route becomes the pdf inspector when one is configured:
// its classification decides between the collector's own fast-path Document
// and a CV run restricted to the pages needing OCR. Unconfigured, PDF stays
// on the CV path exactly as before.
RoutedPlan route_plan(const google::protobuf::RepeatedField<int>& requested, bool pdf,
                      const ParseInputs& inputs) {
  const auto selected = requested_collectors(requested);
  auto routed = route_document(inputs.filename.string(), std::string(), *inputs.bytes);
  const bool inspector =
      inputs.endpoints != nullptr && inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_PDF);
  if (selected.empty() && pdf && routed == pipestream::parse::v1::COLLECTOR_GRPARSE_CV &&
      inspector) {
    routed = pipestream::parse::v1::COLLECTOR_PDF;
  }
  RoutedPlan plan;
  plan.ids = resolve_collectors(selected, routed);
  // A routed office default fans out to the secondary office collectors when
  // their endpoints are configured: a poi leg for the OOXML/OLE2 formats, a
  // calamine leg for workbooks. An explicit selection stays verbatim.
  if (selected.empty() && inputs.endpoints != nullptr) {
    plan.office_fanout = true;
    append_office_fanout(&plan.ids, inputs.filename.string(), inputs.content_type,
                         inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_POI),
                         inputs.endpoints->has(pipestream::parse::v1::COLLECTOR_CALAMINE));
  }
  // Classification routing applies when the pdf collector is the whole plan:
  // by the swap above or by explicit sole selection. Shared with other
  // collectors it is a plain Document-emitting leg.
  plan.pdf_routing = pdf && plan.ids.size() == 1 &&
                     plan.ids[0] == pipestream::parse::v1::COLLECTOR_PDF && inspector;
  return plan;
}

// One planned leg per collector, in plan order: the CV path, an in-process
// fold, the classification-routed pdf leg, or a dialed collector.
std::vector<PlannedCollector> build_plan(
    const std::vector<pipestream::parse::v1::Collector>& plan_ids, bool pdf_routing,
    bool office_fanout, const ParseInputs& inputs, const CvCollector& run_cv) {
  std::vector<PlannedCollector> plan;
  for (const auto id : plan_ids) {
    PlannedCollector collector;
    collector.id = id;
    // The fan-out legs read the same bytes as the routed libreoffice
    // default: beside a live primary their body readings drop and only
    // their claims merge. An explicit selection stays verbatim, readings
    // and all.
    collector.office_fanout =
        office_fanout && (id == pipestream::parse::v1::COLLECTOR_POI ||
                          id == pipestream::parse::v1::COLLECTOR_CALAMINE);
    if (id == pipestream::parse::v1::COLLECTOR_GRPARSE_CV) {
      collector.run = [run_cv, tuning = inputs.tuning] { return run_cv(tuning); };
    } else if (local_collector(id)) {
      collector.run = [id, bytes = inputs.bytes] { return run_local_collector(id, *bytes); };
    } else if (pdf_routing) {
      collector.run = [inputs, run_cv] { return route_pdf_leg(inputs, run_cv); };
    } else {
      collector.run = [id, inputs] {
        // The dequeue check answered "still listening" before this parse
        // started; a cancel can land any time after. Ask again before
        // dialing so a dead call costs no collector leg.
        if (inputs.context->IsCancelled()) return cancelled_outcome();
        return run_remote_collector(id, inputs.endpoints, inputs.filename.string(),
                                    inputs.filename.string(), inputs.content_type,
                                    *inputs.bytes, *inputs.ebcdic_layout_json,
                                    *inputs.lol_html_options_json, inputs.inbound_deadline);
      };
    }
    plan.push_back(std::move(collector));
  }
  return plan;
}

// The status a parse whose every collector failed reports: the first
// failure's code, and every failure's message keyed by its collector.
grpc::Status all_failed_status(const CoordinatorResult& result) {
  const auto& first = result.failures.front();
  std::string message;
  for (const auto& failure : result.failures) {
    if (!message.empty()) message += "; ";
    message += std::string(collector_name(failure.id)) + ": " + failure.error;
  }
  return grpc::Status(first.code, message);
}

// The chart derender leg runs on the finished document, after the merge and
// the repair pass, so it sees every raster chart the CV path classified and
// none of the office charts (those carry their typed table already).
// Advisory and bounded: it edits picture annotations only, never the text or
// the arenas, and its failures are warnings.
void derender_charts_if_configured(const std::shared_ptr<CollectorEndpoints>& collectors,
                                   grpc::CallbackServerContext* context,
                                   CollectorDeadline inbound_deadline,
                                   const ParseInputs& inputs, CoordinatorResult* result) {
  if (collectors == nullptr || !collectors->has_derender() || context->IsCancelled()) return;
  ChartDerenderOptions enrich = collectors->derender();
  // Explicit false on the request turns chart extraction off even when enrich
  // is wired. Unset keeps the historical env opt-in (do_chart_extraction true
  // on ChartDerenderOptions).
  if (inputs.do_chart_extraction.has_value()) {
    enrich.do_chart_extraction = *inputs.do_chart_extraction;
  }
  enrich.do_picture_description = inputs.do_picture_description;
  enrich.do_code_enrichment = inputs.do_code_enrichment;
  enrich.do_formula_enrichment = inputs.do_formula_enrichment;
  enrich.picture_description_area_threshold = inputs.picture_description_area_threshold;
  enrich.picture_description_preset_raw = inputs.picture_description_preset;
  enrich.code_formula_preset_raw = inputs.code_formula_preset;
  if (!inputs.picture_description_vlm_endpoint.empty()) {
    enrich.vlm_endpoint = inputs.picture_description_vlm_endpoint;
  }
  if (inputs.enrich_concurrency.has_value()) {
    enrich.concurrency = *inputs.enrich_concurrency;
  }
  if (inputs.enrich_timeout.has_value()) {
    enrich.timeout = *inputs.enrich_timeout;
  }
  enrich.picture_description_allow = inputs.picture_description_allow;
  enrich.picture_description_deny = inputs.picture_description_deny;
  enrich.picture_description_min_confidence = inputs.picture_description_min_confidence;
  if (!enrich.any_job()) return;
  const ChartDerenderReport derendered =
      derender_charts(collectors->enrich_channel(), enrich, &result->document, inbound_deadline);
  for (const std::string& warning : derendered.warnings) {
    result->warnings.emplace_back(pipestream::parse::v1::COLLECTOR_GRPARSE_CV, warning);
  }
}

// Collector warnings are not failures; they stay on the document, keyed by
// collector, so nothing the collectors reported is dropped.
void stamp_collector_warnings(CoordinatorResult* result) {
  for (const auto& [collector, text] : result->warnings) {
    auto& fields =
        *result->document.mutable_body()->mutable_meta()->mutable_custom_fields();
    *fields[std::string("collector_warnings:") + collector_name(collector)]
         .mutable_list_value()
         ->add_values()
         ->mutable_string_value() = text;
  }
}

}  // namespace

grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& repair,
                          const std::string& surface, SourceParse* parsed) {
  const auto& sources = request.sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one FileSource containing base64_string");
  }
  const grpc::Status option_status = validate_options(request.options(), surface);
  if (!option_status.ok()) return option_status;
  try {
    const auto& source = sources.Get(0).file();
    auto bytes = std::make_shared<const std::string>(decode_base64(source.base64_string()));
    const fs::path requested_name = source.filename().empty() ? "document.pdf" : fs::path(source.filename()).filename();
    pipestream::document::v1::Document base = base_document(*bytes, requested_name);

    if (!request.options().from_formats().empty()) {
      const auto detected =
          input_format_for(base.origin().mimetype(), requested_name);
      if (!detected.has_value()) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            surface + ": from_formats is set but the input type '" +
                base.origin().mimetype() + "' does not map to a known InputFormat");
      }
      if (!from_formats_allows(request.options().from_formats(), *detected)) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            surface + ": from_formats does not allow " +
                pipestream::parse::v1::InputFormat_Name(*detected));
      }
    }

    const ParseInputs inputs =
        parse_inputs(context, request, scheduler, collectors, bytes, requested_name,
                     base.origin().mimetype());

    const auto cv_offsets = std::make_shared<
        google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>>();
    const auto cv_confidence =
        std::make_shared<std::optional<pipestream::parse::v1::ConfidenceScores>>();
    const bool pdf = is_pdf(*bytes, requested_name);
    if (inputs.vlm_pipeline) {
      if (collectors == nullptr || !collectors->has_vlm()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            surface + ": pipeline VLM needs grpc-vlm-convert "
                                      "(GRPARSE_VLM_CONVERT_TARGET)");
      }
      VlmConvertOptions vlm = collectors->vlm();
      apply_vlm_convert_options(request.options(), &vlm);
      if (inputs.tuning.render_dpi > 0.0 && vlm.render_dpi <= 0.0) {
        vlm.render_dpi = inputs.tuning.render_dpi;
      }
      if (inputs.tuning.page_range.has_value() && !vlm.page_range.has_value()) {
        vlm.page_range = inputs.tuning.page_range;
      }
      CoordinatorResult result;
      result.document = std::move(base);
      const VlmConvertReport report =
          convert_vlm_pages(collectors->vlm_channel(), vlm, bytes, pdf, &result.document,
                            inputs.inbound_deadline);
      for (const std::string& warning : report.warnings) {
        result.warnings.emplace_back(pipestream::parse::v1::COLLECTOR_GRPARSE_CV, warning);
      }
      if (!report.success) {
        return grpc::Status(report.code, report.error.empty()
                                             ? surface + ": pipeline VLM failed"
                                             : report.error);
      }
      result.succeeded = 1;
      if (context->IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
      }
      const bool repaired_text =
          repair.has_value() &&
          run_repair_pass(&result.document, *repair).changed_text_or_arenas();
      (void)repaired_text;
      derender_charts_if_configured(collectors, context, inputs.inbound_deadline, inputs,
                                    &result);
      stamp_collector_warnings(&result);
      parsed->filename = requested_name;
      parsed->result = std::move(result);
      return grpc::Status::OK;
    }
    const CvCollector run_cv(context, scheduler, bytes, pdf, cv_offsets, cv_confidence,
                             inputs.heading);

    const RoutedPlan routed = route_plan(request.options().collectors(), pdf, inputs);
    // NATIVE is model-free by definition. A plan that would put the bytes
    // through the CV models (a PDF with no inspector to read its text layer,
    // a raster image) cannot honour it, and the caller must know rather
    // than get a modelled document under a model-free label.
    if (inputs.native_pipeline &&
        std::ranges::find(routed.ids, pipestream::parse::v1::COLLECTOR_GRPARSE_CV) !=
            routed.ids.end()) {
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          surface + ": pipeline NATIVE needs the pdf collector "
                                    "(GRPARSE_COLLECTOR_PDF) for PDF input and does not "
                                    "apply to raster input");
    }
    CoordinatorResult result = run_collectors(
        build_plan(routed.ids, routed.pdf_routing, routed.office_fanout, inputs, run_cv),
        std::move(base));
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
    }
    if (result.succeeded == 0) return all_failed_status(result);

    // The merged Document is complete here, and this is the one place every
    // unary surface renders from, so the post-merge repair pass runs here:
    // running headers and footers demoted to furniture, line-break
    // hyphenation rejoined, paragraphs a page break split merged.
    const bool repaired_text =
        repair.has_value() &&
        run_repair_pass(&result.document, *repair).changed_text_or_arenas();
    derender_charts_if_configured(collectors, context, inputs.inbound_deadline, inputs, &result);
    // The offset table describes the CV collector's own text stream. It is
    // published only when that collector is the entire document and the
    // repair pass left its text and arena alone: a merge renumbers arena
    // references, a repair that retires items or rewrites text moves them,
    // and a table that no longer names the items it describes is worse than
    // no table at all.
    if (result.succeeded == 1 && !cv_offsets->empty() && !repaired_text) {
      chunking::add_offsets(*cv_offsets, &parsed->offsets);
    }
    stamp_collector_warnings(&result);
    parsed->filename = requested_name;
    parsed->result = std::move(result);
    parsed->confidence = std::move(*cv_confidence);
    return grpc::Status::OK;
  } catch (...) {
    return status_from_exception(std::current_exception());
  }
}

}  // namespace grparse
