#include "source_parse.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/json_util.h>

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
#include "grparse/pdf_form_widgets.h"
#include "grparse/schema_version.h"
#include "grparse/vlm_convert.h"
#include "parse_support.h"
#include "structure_validation.h"

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
    case pipestream::parse::v1::OUTPUT_FORMAT_CHUNKS:
    case pipestream::parse::v1::OUTPUT_FORMAT_DCLX:
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
      "ebcdic_layout",
      "ebcdic_layout_json",
      "lol_html_options",
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
      "chunking_preset",
      "hierarchical_chunking",
      "hybrid_chunking",
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
      // dials grpc-vlm-convert when GRPARSE_VLM_CONVERT_TARGET is set. The
      // presets and maps no leg reads pass only at their Docling defaults
      // (validate_unread_options); the rest apply where a dial exists.
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
      "caption_placement",
      "chart_extraction_preset",
      "chart_extraction_custom_config",
      "table_structure_custom_config",
      "layout_custom_config",
      "ocr_custom_config",
      "picture_classification_custom_config",
      "doclang_include_namespace",
      "structure_validation",
      "structure_validation_rules",
      "structure_repairs",
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

// PDFs are read here by the PdfBackendService GRPARSE_PDF_BACKEND names, not
// by Docling's Python backends. Named PdfBackend values are accepted so
// clients that always set the field are not turned away; the deployment's
// configured backend reads the document either way.
grpc::Status validate_pdf_backend(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                  const std::string& surface) {
  if (!options.has_pdf_backend()) return grpc::Status::OK;
  if (pipestream::parse::v1::PdfBackend_IsValid(options.pdf_backend())) {
    return grpc::Status::OK;
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                      surface + " pdf_backend value is not a known PdfBackend");
}

// A ScalarValue as a number, when it holds one.
std::optional<double> scalar_number(const pipestream::parse::v1::ScalarValue& value) {
  if (value.has_double_value()) return value.double_value();
  if (value.has_int_value()) return static_cast<double>(value.int_value());
  if (value.has_uint_value()) return static_cast<double>(value.uint_value());
  return std::nullopt;
}

// The picture_description_api.params names grpc-enrich has a typed field
// for; max_completion_tokens is OpenAI's newer name for max_tokens.
constexpr std::string_view kTypedGenerationParams[] = {
    "max_completion_tokens", "max_tokens", "model", "seed", "temperature", "top_p"};

// Sets the typed generation param `name` from `value`, or returns why the
// value is refused (the text after "picture_description_api.").
std::optional<std::string> set_generation_param(
    const std::string& name, const pipestream::parse::v1::ScalarValue& value,
    ai::pipestream::enrich::v1::VlmGenerationParams* params) {
  const std::optional<double> number = scalar_number(value);
  constexpr auto kMaxTokens = std::numeric_limits<uint32_t>::max();
  if (name == "model") {
    if (!value.has_string_value() || value.string_value().empty()) {
      return "params.model must be a non-empty string";
    }
    params->set_model(value.string_value());
  } else if (name == "max_tokens" || name == "max_completion_tokens") {
    const bool whole =
        (value.has_int_value() && value.int_value() > 0 && value.int_value() <= kMaxTokens) ||
        (value.has_uint_value() && value.uint_value() > 0 && value.uint_value() <= kMaxTokens);
    if (!whole) return "params." + name + " must be a positive integer";
    params->set_max_tokens(static_cast<uint32_t>(*number));
  } else if (name == "temperature") {
    if (!number.has_value() || !std::isfinite(*number) || *number < 0.0) {
      return "params.temperature must be a finite number, not negative";
    }
    params->set_temperature(*number);
  } else if (name == "top_p") {
    if (!number.has_value() || !(*number >= 0.0 && *number <= 1.0)) {
      return "params.top_p must be a number between 0 and 1";
    }
    params->set_top_p(*number);
  } else if (value.has_int_value()) {
    params->set_seed(value.int_value());
  } else if (value.has_uint_value() &&
             value.uint_value() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    params->set_seed(static_cast<int64_t>(value.uint_value()));
  } else {
    return "params.seed must be an integer";
  }
  return std::nullopt;
}

// The params map is a free-form dict; only the names grpc-enrich has a
// typed field for travel, visited in name order so the refusal of an
// unknown one is deterministic. No params send no message.
std::expected<std::optional<ai::pipestream::enrich::v1::VlmGenerationParams>, grpc::Status>
generation_params(const pipestream::parse::v1::PictureDescriptionApi& api,
                  const std::string& surface) {
  const auto invalid = [&surface](const std::string& what) {
    return std::unexpected(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        surface + ": picture_description_api." + what));
  };
  std::vector<std::string> names;
  for (const auto& entry : api.params()) names.push_back(entry.first);
  if (names.empty()) return std::nullopt;
  std::ranges::sort(names);
  ai::pipestream::enrich::v1::VlmGenerationParams params;
  std::optional<std::string> token_cap_name;
  for (const std::string& name : names) {
    if (std::ranges::find(kTypedGenerationParams, name) == std::end(kTypedGenerationParams)) {
      return std::unexpected(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          surface + " does not implement option 'picture_description_api.params." + name + "'"));
    }
    if (name == "max_tokens" || name == "max_completion_tokens") {
      // The two names are one generation cap; a request naming both is
      // ambiguous.
      if (token_cap_name.has_value()) {
        return invalid("params." + *token_cap_name + " and params." + name +
                       " are the same generation cap; set one");
      }
      token_cap_name = name;
    }
    if (auto refused = set_generation_param(name, api.params().at(name), &params)) {
      return invalid(*refused);
    }
  }
  return params;
}

// Why a header cannot go to the endpoint as it is, naming the header and
// never its value (a header value is a credential); nothing when it can.
std::optional<std::string> header_refusal(const std::string& name, const std::string& lowered,
                                          const std::string& value) {
  const auto token_char = [](unsigned char c) {
    return std::isalnum(c) != 0 || std::string_view("!#$%&'*+-.^_`|~").contains(c);
  };
  if (name.empty() || !std::ranges::all_of(name, token_char)) {
    return "headers name '" + name + "' is not an HTTP header name";
  }
  static constexpr std::string_view kReserved[] = {
      "host",       "content-type", "content-length", "expect",           "connection",
      "keep-alive", "te",           "trailer",        "transfer-encoding", "upgrade"};
  if (std::ranges::find(kReserved, lowered) != std::end(kReserved) ||
      lowered.starts_with("proxy-")) {
    return "headers '" + name + "' is set by the HTTP client, not the request";
  }
  if (std::ranges::any_of(value, [](unsigned char c) {
        return (c < 0x20 && c != '\t') || c == 0x7f;
      })) {
    return "headers '" + name + "' value holds a control character";
  }
  return std::nullopt;
}

// The headers as VlmHeader entries in name order. HTTP names are case
// insensitive, so two map keys differing only in case would send the one
// header twice; that is refused naming both.
std::expected<std::vector<ai::pipestream::enrich::v1::VlmHeader>, grpc::Status> vlm_headers(
    const pipestream::parse::v1::PictureDescriptionApi& api, const std::string& surface) {
  const auto invalid = [&surface](const std::string& what) {
    return std::unexpected(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        surface + ": picture_description_api." + what));
  };
  std::vector<std::string> names;
  for (const auto& entry : api.headers()) names.push_back(entry.first);
  std::ranges::sort(names);
  std::vector<ai::pipestream::enrich::v1::VlmHeader> headers;
  std::map<std::string, std::string> seen;
  for (const std::string& name : names) {
    std::string lowered = name;
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string& value = api.headers().at(name);
    if (auto refused = header_refusal(name, lowered, value)) return invalid(*refused);
    const auto [earlier, inserted] = seen.try_emplace(lowered, name);
    if (!inserted) {
      return invalid("headers '" + earlier->second + "' and '" + name +
                     "' name the same header; set one");
    }
    ai::pipestream::enrich::v1::VlmHeader& header = headers.emplace_back();
    header.set_name(name);
    header.set_value(value);
  }
  return headers;
}

// Nested picture-description engines map onto enrich fields this binary can
// forward (repo_id / url / timeout / concurrency / classification_*, and the
// api's prompt, params and headers in the enrich service's typed form).
// Local and API are mutually exclusive. The local engine's prompt and
// generation_config are accepted for Docling clients (ScalarValue maps) even
// though the enrich dial does not forward them.
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
  }
  if (options.has_picture_description_api()) {
    const auto& api = options.picture_description_api();
    if (api.url().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.url is required");
    }
    if (api.has_timeout() && api.timeout() <= 0.0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.timeout must be positive");
    }
    if (api.has_concurrency() && api.concurrency() < 1) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": picture_description_api.concurrency must be >= 1");
    }
    if (auto call = picture_description_call(api, surface); !call.has_value()) {
      return call.error();
    }
  }
  return grpc::Status::OK;
}

// A request that names its own remote model endpoint has a peer call that
// address on the caller's behalf, so it is refused unless the operator set
// GRPARSE_ENABLE_REMOTE_SERVICES=on (docling-serve's
// DOCLING_SERVE_ENABLE_REMOTE_SERVICES). A vlm_pipeline_model_api.url that
// is not an http(s) URL is a preset name, never dialed, and stays allowed.
grpc::Status validate_remote_services(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                      const CollectorEndpoints* collectors,
                                      const std::string& surface) {
  if (collectors != nullptr && collectors->remote_services_enabled()) return grpc::Status::OK;
  const auto refused = [&surface](const std::string& field) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        surface + ": " + field +
                            " names a remote service, which this server does not call on a "
                            "request's behalf unless GRPARSE_ENABLE_REMOTE_SERVICES=on");
  };
  if (options.has_picture_description_api() && !options.picture_description_api().url().empty()) {
    return refused("picture_description_api.url");
  }
  if (options.has_vlm_pipeline_model_api()) {
    const std::string& url = options.vlm_pipeline_model_api().url();
    if (url.starts_with("http://") || url.starts_with("https://")) {
      return refused("vlm_pipeline_model_api.url");
    }
  }
  return grpc::Status::OK;
}

// VLM selection fields are accepted so Docling clients that always set them
// are not turned away. They are mutually exclusive (preset / enum / local /
// api). PROCESSING_PIPELINE_VLM uses them when the convert peer is configured.
grpc::Status validate_vlm_selection(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                    const std::string& surface) {
  int engines = 0;
  if (options.has_vlm_pipeline_model() &&
      options.vlm_pipeline_model() != pipestream::parse::v1::VLM_MODEL_TYPE_UNSPECIFIED) {
    ++engines;
  }
  if (options.has_vlm_pipeline_model_local()) {
    ++engines;
  }
  if (options.has_vlm_pipeline_model_api()) {
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
  if (options.has_vlm_pipeline_model_local()) {
    const auto& local = options.vlm_pipeline_model_local();
    if (!local.has_repo_id() || local.repo_id().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": vlm_pipeline_model_local.repo_id is required");
    }
  }
  if (options.has_vlm_pipeline_model_api()) {
    const auto& api = options.vlm_pipeline_model_api();
    if (!api.has_url() || api.url().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": vlm_pipeline_model_api.url is required");
    }
    if (api.has_timeout() && api.timeout() <= 0.0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": vlm_pipeline_model_api.timeout must be positive");
    }
    if (api.has_concurrency() && api.concurrency() < 1) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": vlm_pipeline_model_api.concurrency must be >= 1");
    }
  }
  return grpc::Status::OK;
}

// Custom-config fields mirror Docling: typed VLM option messages plus open
// ScalarValue maps for dict[str, Any]. Known keys are soft-validated; empty
// maps are a no-op.
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

  // Open ScalarValue maps: soft-validate well-known keys when present
  // (validate_unread_options turns down the ocr_custom_config keys).
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

  const bool has_chart_preset = options.has_chart_extraction_preset();
  const bool has_chart_config = options.has_chart_extraction_custom_config();
  if (has_chart_preset && has_chart_config) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": chart_extraction_preset and "
                                  "chart_extraction_custom_config are mutually exclusive");
  }
  if (has_chart_preset && options.chart_extraction_preset().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": chart_extraction_preset is empty");
  }
  if (has_chart_config) {
    const auto& cfg = options.chart_extraction_custom_config();
    if (auto s = require_engine(cfg.engine_options().engine_type(),
                                "chart_extraction_custom_config");
        !s.ok()) {
      return s;
    }
    if (!pipestream::parse::v1::VlmEngineType_IsValid(cfg.engine_options().engine_type())) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": chart_extraction_custom_config.engine_options."
                                    "engine_type is not a known engine");
    }
    if (auto s = require_model_spec(cfg.model_spec(), "chart_extraction_custom_config");
        !s.ok()) {
      return s;
    }
    // Docling's ChartExtractionVlmEngineOptions validator: the three
    // outputs are independent, but at least one must be on. All three, and
    // the natural-language prompt switch, travel to grpc-enrich.
    const bool chart2csv = !cfg.has_chart2csv() || cfg.chart2csv();
    const bool chart2summary = cfg.has_chart2summary() && cfg.chart2summary();
    const bool chart2code = cfg.has_chart2code() && cfg.chart2code();
    if (!chart2csv && !chart2summary && !chart2code) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": chart_extraction_custom_config: at least one of "
                                    "chart2csv, chart2summary, or chart2code must be true");
    }
    if (cfg.has_output_format()) {
      const auto format = cfg.output_format();
      if (format == pipestream::parse::v1::CHART_EXTRACTION_OUTPUT_FORMAT_UNSPECIFIED &&
          cfg.output_format_raw().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + ": chart_extraction_custom_config.output_format "
                                      "is unspecified");
      }
      if (format != pipestream::parse::v1::CHART_EXTRACTION_OUTPUT_FORMAT_UNSPECIFIED &&
          format != pipestream::parse::v1::
                        CHART_EXTRACTION_OUTPUT_FORMAT_GRANITE_VISION_CHARTS) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + ": chart_extraction_custom_config.output_format "
                                      "is not a known format");
      }
      if (!cfg.output_format_raw().empty() &&
          cfg.output_format_raw() != "granite_vision_charts") {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + ": chart_extraction_custom_config.output_format and "
                                      "output_format_raw disagree");
      }
    } else if (!cfg.output_format_raw().empty() &&
               cfg.output_format_raw() != "granite_vision_charts") {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": chart_extraction_custom_config.output_format_raw "
                                    "is not a known format");
    }
  }
  if (options.has_caption_placement()) {
    switch (options.caption_placement()) {
      case pipestream::parse::v1::CAPTION_PLACEMENT_STANDARD:
      case pipestream::parse::v1::CAPTION_PLACEMENT_LAYOUT:
        break;
      default:
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            surface + ": caption_placement is unspecified or unknown");
    }
  }
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

// The per-collector rules in each collector's typed form. The typed fields
// are the contract; the deprecated JSON fields still work alone (the ebcdic
// layout JSON is the collector's own layout_json input, forwarded as is;
// the lol-html JSON is read here into the typed message), and a request
// that sets both forms of one is refused rather than guessed at.
grpc::Status resolve_collector_rules(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                     const std::string& surface, CollectorRules* rules) {
  if (options.has_ebcdic_layout() && options.has_ebcdic_layout_json()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": ebcdic_layout and the deprecated ebcdic_layout_json are "
                                  "mutually exclusive");
  }
  if (options.has_lol_html_options() && options.has_lol_html_options_json()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": lol_html_options and the deprecated lol_html_options_json "
                                  "are mutually exclusive");
  }
  if (options.has_ebcdic_layout()) {
    *rules->ebcdic.mutable_layout() = options.ebcdic_layout();
  } else if (!options.ebcdic_layout_json().empty()) {
    rules->ebcdic.set_layout_json(options.ebcdic_layout_json());
  }
  if (options.has_lol_html_options()) {
    rules->lol_html = options.lol_html_options();
  } else if (!options.lol_html_options_json().empty()) {
    lolhtml::v1::ExtractOptions parsed;
    const auto status =
        google::protobuf::util::JsonStringToMessage(options.lol_html_options_json(), &parsed);
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": lol_html_options_json does not parse as "
                                    "lolhtml.v1.ExtractOptions: " +
                              std::string(status.message()));
    }
    rules->lol_html = std::move(parsed);
  }
  return grpc::Status::OK;
}

// Options Docling clients populate that no leg here reads. Their Docling
// defaults are what this server does anyway, so those pass; any other value
// asks for behaviour this server would silently not deliver, and is turned
// down by name. table_mode and pdf_backend stay accepted (see their
// validators): one model and the deployment's own backend serve every value.
grpc::Status validate_unread_options(const pipestream::parse::v1::ConvertDocumentOptions& options,
                                     const std::string& surface) {
  const auto rejected = [&surface](const std::string& what) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " does not implement option '" + what + "'");
  };
  // The installed PP-OCRv3 "ch" models read Chinese and English and nothing
  // else, which is also RapidOCR's own default language list.
  static constexpr std::string_view kReadLanguages[] = {"ch", "chinese", "zh", "en", "english"};
  for (const std::string& language : options.ocr_lang()) {
    if (std::ranges::find(kReadLanguages, language) == std::end(kReadLanguages)) {
      return rejected("ocr_lang' value '" + language);
    }
  }
  if (options.has_table_cell_matching() && !options.table_cell_matching()) {
    return rejected("table_cell_matching");
  }
  // Only the VLM pipeline stops at the first failed page; the standard path
  // always degrades to a partial result.
  const bool vlm_pipeline =
      (options.has_pipeline() &&
       options.pipeline() == pipestream::parse::v1::PROCESSING_PIPELINE_VLM) ||
      (options.collectors_size() == 1 &&
       options.collectors(0) == pipestream::parse::v1::COLLECTOR_VLM);
  if (options.has_abort_on_error() && options.abort_on_error() && !vlm_pipeline) {
    return rejected("abort_on_error");
  }
  const auto default_preset = [](bool has, const std::string& value) {
    return !has || value.empty() || value == "default";
  };
  // RapidOCR is the one recognizer here, so its own preset name is honoured.
  if (!default_preset(options.has_ocr_preset(), options.ocr_preset()) &&
      options.ocr_preset() != "rapidocr") {
    return rejected("ocr_preset");
  }
  if (!default_preset(options.has_table_structure_preset(), options.table_structure_preset())) {
    return rejected("table_structure_preset");
  }
  if (!default_preset(options.has_layout_preset(), options.layout_preset())) {
    return rejected("layout_preset");
  }
  if (!default_preset(options.has_picture_classification_preset(),
                      options.picture_classification_preset())) {
    return rejected("picture_classification_preset");
  }
  // There is no chunking preset catalog: the only preset this server can
  // honour is the hierarchical defaults it falls back to.
  if (!default_preset(options.has_chunking_preset(), options.chunking_preset()) &&
      options.chunking_preset() != "hierarchical") {
    return rejected("chunking_preset");
  }
  if (!options.table_structure_custom_config().empty()) {
    return rejected("table_structure_custom_config");
  }
  if (!options.layout_custom_config().empty()) return rejected("layout_custom_config");
  // The VLM convert dial carries the endpoint and scale: a keyed API's auth
  // headers and request params would be dropped the same way.
  if (options.has_vlm_pipeline_model_api()) {
    const auto& api = options.vlm_pipeline_model_api();
    if (!api.headers().empty()) return rejected("vlm_pipeline_model_api.headers");
    if (!api.params().empty()) return rejected("vlm_pipeline_model_api.params");
  }
  // No OCR engine reads an option map: RapidOCR runs the installed models
  // as they are. A lang naming a language they read is what happens anyway
  // (as with ocr_lang); any other key, or any other lang, is turned down,
  // first key in name order.
  std::vector<std::string> ocr_keys;
  for (const auto& entry : options.ocr_custom_config()) ocr_keys.push_back(entry.first);
  std::ranges::sort(ocr_keys);
  for (const std::string& key : ocr_keys) {
    const auto& value = options.ocr_custom_config().at(key);
    if (key == "lang" && value.has_string_value() &&
        std::ranges::find(kReadLanguages, value.string_value()) != std::end(kReadLanguages)) {
      continue;
    }
    return rejected("ocr_custom_config." + key);
  }
  return grpc::Status::OK;
}

}  // namespace

uint64_t decoded_source_bytes(
    const google::protobuf::RepeatedPtrField<pipestream::parse::v1::Source>& sources) {
  uint64_t total = 0;
  for (const auto& source : sources) {
    if (source.has_file()) total += source.file().base64_string().size() / 4 * 3 + 3;
  }
  return total;
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
  const grpc::Status unread_status = validate_unread_options(options, surface);
  if (!unread_status.ok()) return unread_status;
  CollectorRules rules;
  const grpc::Status rules_status = resolve_collector_rules(options, surface, &rules);
  if (!rules_status.ok()) return rules_status;
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
  const grpc::Status structure_status =
      validate_structure_request(StructureRequest::from(options), surface);
  if (!structure_status.ok()) return structure_status;
  // COLLECTOR_VLM selects the VLM convert pipeline rather than a fan-out leg.
  // By itself it runs grpc-vlm-convert; mixed with other collectors is rejected.
  bool wants_vlm_collector = false;
  for (const int raw : options.collectors()) {
    if (!pipestream::parse::v1::Collector_IsValid(raw) ||
        raw == pipestream::parse::v1::COLLECTOR_UNSPECIFIED) {
      std::string name = pipestream::parse::v1::Collector_Name(
          static_cast<pipestream::parse::v1::Collector>(raw));
      if (name.empty()) name = std::to_string(raw);
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " collectors contains invalid value '" + name + "'");
    }
    if (raw == pipestream::parse::v1::COLLECTOR_VLM) wants_vlm_collector = true;
  }
  if (wants_vlm_collector && options.collectors_size() != 1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": COLLECTOR_VLM must be the only collector "
                                  "(it maps to PROCESSING_PIPELINE_VLM)");
  }
  const bool has_chunking_preset =
      options.has_chunking_preset() && !options.chunking_preset().empty();
  const bool has_chunking_options = options.chunking_options_case() !=
                                    pipestream::parse::v1::ConvertDocumentOptions::CHUNKING_OPTIONS_NOT_SET;
  if (has_chunking_preset && has_chunking_options) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": chunking_preset and chunking_options are mutually exclusive");
  }
  bool wants_chunks = false;
  for (const auto raw : options.to_formats()) {
    if (static_cast<pipestream::parse::v1::OutputFormat>(raw) ==
        pipestream::parse::v1::OUTPUT_FORMAT_CHUNKS) {
      wants_chunks = true;
      break;
    }
  }
  if ((has_chunking_preset || has_chunking_options) && !wants_chunks) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": chunking_preset/chunking_options require "
                                  "OUTPUT_FORMAT_CHUNKS in to_formats");
  }
  // The hybrid budget decides every boundary, so ConvertSource checks it
  // exactly like ChunkHybridSource does, before any parse starts.
  if (options.chunking_options_case() ==
      pipestream::parse::v1::ConvertDocumentOptions::kHybridChunking) {
    const grpc::Status hybrid_status = chunking::validate_hybrid_options(options.hybrid_chunking());
    if (!hybrid_status.ok()) {
      return grpc::Status(hybrid_status.error_code(),
                          surface + ": " + hybrid_status.error_message());
    }
  }
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
  // Docling page_range is an inclusive (start, end) pair; on the wire that is
  // IntSpan (same shape as serve). Unset means the whole document.
  if (options.has_page_range()) {
    const int start = options.page_range().start();
    const int end = options.page_range().end();
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
    // docling-core refuses EMBEDDED for the DocLang archive, which keeps
    // its images outside the markup; so does this server, before parsing.
    if (options.image_export_mode() == pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED &&
        std::find(options.to_formats().begin(), options.to_formats().end(),
                  static_cast<int>(pipestream::parse::v1::OUTPUT_FORMAT_DCLX)) !=
            options.to_formats().end()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + ": image_export_mode IMAGE_REF_MODE_EMBEDDED is not supported "
                                    "for OUTPUT_FORMAT_DCLX (the archive keeps images outside "
                                    "the markup); use REFERENCED or PLACEHOLDER");
    }
  }
  return validate_document_timeout(options.has_document_timeout(), options.document_timeout(),
                                   surface);
}

std::expected<std::optional<ChartExtractionPreset>, grpc::Status> resolve_chart_extraction(
    const pipestream::parse::v1::ConvertDocumentOptions& options,
    const ChartExtractionPolicy& policy, const std::string& surface) {
  const auto rejected = [&surface](const grpc::Status& status) {
    return std::unexpected(
        grpc::Status(status.error_code(), surface + ": " + status.error_message()));
  };
  if (options.has_chart_extraction_custom_config()) {
    const auto& config = options.chart_extraction_custom_config();
    const grpc::Status allowed =
        policy.check_custom_config(vlm_engine_name(config.engine_options().engine_type()));
    if (!allowed.ok()) return rejected(allowed);
    return std::optional<ChartExtractionPreset>(chart_preset_from_custom_config(config));
  }
  // An unset do_chart_extraction keeps the environment opt-in (the leg runs
  // when GRPARSE_ENRICH_TARGET is set), so it resolves the default preset
  // too; only an explicit false with no preset named turns charts off.
  if (!options.has_chart_extraction_preset() && options.has_do_chart_extraction() &&
      !options.do_chart_extraction()) {
    return std::optional<ChartExtractionPreset>();
  }
  auto preset = policy.resolve(options.chart_extraction_preset());
  if (!preset.has_value()) return rejected(preset.error());
  return std::optional<ChartExtractionPreset>(std::move(*preset));
}

DoclangOptions doclang_options(const pipestream::parse::v1::ConvertDocumentOptions& options) {
  DoclangOptions doclang;
  if (options.has_image_export_mode()) {
    switch (options.image_export_mode()) {
      case pipestream::parse::v1::IMAGE_REF_MODE_EMBEDDED:
        doclang.image_mode = DoclangOptions::ImageMode::kEmbedded;
        break;
      case pipestream::parse::v1::IMAGE_REF_MODE_REFERENCED:
        doclang.image_mode = DoclangOptions::ImageMode::kReferenced;
        break;
      case pipestream::parse::v1::IMAGE_REF_MODE_PLACEHOLDER:
        doclang.image_mode = DoclangOptions::ImageMode::kPlaceholder;
        break;
      default:
        // UNSPECIFIED: each DocLang format keeps its own default.
        break;
    }
  }
  if (options.has_doclang_include_namespace()) {
    doclang.include_namespace = options.doclang_include_namespace();
  }
  return doclang;
}

std::expected<PictureDescriptionCall, grpc::Status> picture_description_call(
    const pipestream::parse::v1::PictureDescriptionApi& api, const std::string& surface) {
  PictureDescriptionCall call;
  if (api.has_prompt()) call.prompt = api.prompt();
  auto params = generation_params(api, surface);
  if (!params.has_value()) return std::unexpected(params.error());
  call.params = std::move(*params);
  auto headers = vlm_headers(api, surface);
  if (!headers.has_value()) return std::unexpected(headers.error());
  call.headers = std::move(*headers);
  return call;
}

std::expected<PictureDescriptionCall, grpc::Status> request_picture_description_call(
    const pipestream::parse::v1::ConvertDocumentOptions& options,
    const std::optional<ChartExtractionPreset>& chart_extraction, const std::string& surface) {
  if (!options.has_picture_description_api()) return PictureDescriptionCall{};
  const auto& api = options.picture_description_api();
  auto call = picture_description_call(api, surface);
  if (!call.has_value() || call->headers.empty()) return call;
  // grpc-enrich sends vlm_headers on every call to a per-request endpoint
  // of the job, chart_extraction.vlm_endpoint included; the caller's
  // headers are for its own endpoint only. The chart leg runs unless the
  // request turned it off (see derender_charts_if_configured).
  const bool charts_run = !options.has_do_chart_extraction() || options.do_chart_extraction();
  if (charts_run && chart_extraction.has_value() && !chart_extraction->vlm_endpoint.empty() &&
      chart_extraction->vlm_endpoint != api.url()) {
    return std::unexpected(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT,
        surface + ": picture_description_api.headers would also go to the endpoint of chart "
                  "extraction preset '" + chart_extraction->id +
            "'; drop the headers or set do_chart_extraction to false"));
  }
  return call;
}

namespace {

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
              CvConfidence confidence, HeadingOptions heading_options,
              CollectorDeadline deadline)
      : context_(context),
        scheduler_(scheduler),
        bytes_(std::move(bytes)),
        pdf_(pdf),
        offsets_(std::move(offsets)),
        confidence_(std::move(confidence)),
        heading_options_(std::move(heading_options)),
        deadline_(deadline) {}

  CollectorOutcome operator()(const PageScheduler::OcrTuning& tuning) const {
    try {
      PageSet pages;
      if (std::optional<CollectorOutcome> failure = collect_pages(tuning, &pages)) {
        return std::move(*failure);
      }
      return assemble(pages);
    } catch (...) {
      return outcome_from_exception(std::current_exception());
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
  // cancelling the ticket as soon as the call goes away or the parse's
  // deadline (document_timeout, or the call's own) passes. Returns the
  // outcome that ended the run, or nothing when every page arrived.
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
    bool expired = false;
    while (!state.finished) {
      state.changed.wait_for(lock, std::chrono::milliseconds(25));
      if (!expired && std::chrono::system_clock::now() >= deadline_) expired = true;
      if (context_->IsCancelled() || expired) ticket.cancel();
    }
    if (context_->IsCancelled()) return cancelled_outcome();
    if (expired) {
      CollectorOutcome outcome;
      outcome.error = "document deadline exceeded before every page was read";
      outcome.code = grpc::StatusCode::DEADLINE_EXCEEDED;
      return outcome;
    }
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
    // A list is one structure: consecutive list items join a LIST group.
    group_list_items(&outcome.document);
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
  CollectorDeadline deadline_;
};

// Everything one parse's collector legs read: the request's bytes and
// identity, the endpoints they dial, and the ceilings they run under. Copied
// into each leg, which is why every member is a value or a shared handle.
struct ParseInputs {
  grpc::CallbackServerContext* context = nullptr;
  std::shared_ptr<CollectorEndpoints> endpoints;
  std::shared_ptr<const std::string> bytes;
  std::shared_ptr<const CollectorRules> collector_rules;
  fs::path filename;
  // What the dialed collectors log and correlate this parse by: the name
  // plus a per-call sequence, so two concurrent uploads of one filename
  // stay apart.
  std::string document_id;
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
  // The chart-extraction preset the server policy resolved (outputs,
  // prompts, model, endpoint); nullopt when the request switched charts off
  // without naming a preset.
  std::optional<ChartExtractionPreset> chart_extraction;
  // From picture_description_api.url / local.repo_id when set; empty means keep
  // the enrich service (or env) default.
  std::string picture_description_vlm_endpoint;
  // picture_description_api's prompt, params and headers, typed for enrich.
  PictureDescriptionCall picture_description_call;
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
                         const fs::path& requested_name, std::string content_type,
                         std::optional<ChartExtractionPreset> chart_extraction,
                         PictureDescriptionCall picture_description_call) {
  const auto& options = request.options();
  ParseInputs inputs;
  inputs.context = context;
  inputs.endpoints = collectors;
  inputs.bytes = std::move(bytes);
  // validate_options already refused rules that do not resolve.
  auto rules = std::make_shared<CollectorRules>();
  static_cast<void>(resolve_collector_rules(options, std::string(), rules.get()));
  inputs.collector_rules = std::move(rules);
  inputs.filename = requested_name;
  static std::atomic<uint64_t> call_sequence{0};
  inputs.document_id = requested_name.string() + "#" +
                       std::to_string(call_sequence.fetch_add(1, std::memory_order_relaxed) + 1);
  inputs.content_type = std::move(content_type);
  inputs.tuning = ocr_tuning(options.has_do_ocr(), options.do_ocr(), options.force_ocr(),
                             options.has_render_scale(), options.render_scale());
  if (options.has_page_range()) {
    inputs.tuning.page_range =
        std::make_pair(options.page_range().start(), options.page_range().end());
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
  inputs.chart_extraction = std::move(chart_extraction);
  inputs.picture_description_call = std::move(picture_description_call);
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
  // Neither ocr_lang nor ocr_custom_config.lang selects a model: the one
  // installed RapidOCR set reads Chinese and English, and
  // validate_unread_options turns down any other ocr_lang value by name.
  // Every dialed leg inherits this call's own ceiling, so no collector is
  // waited on past the patience of the client that asked for the parse. A
  // call with no deadline yields time_point::max(), which leaves each leg on
  // its own static cap exactly as before. document_timeout (seconds) further
  // caps that ceiling when set — parity with Docling Convert options.
  inputs.inbound_deadline = deadline_with_document_timeout(
      context->deadline(), options.has_document_timeout(), options.document_timeout());
  inputs.tuning.deadline = inputs.inbound_deadline;
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
  const bool vlm_collector =
      options.collectors_size() == 1 &&
      options.collectors(0) == pipestream::parse::v1::COLLECTOR_VLM;
  inputs.vlm_pipeline =
      (options.has_pipeline() &&
       options.pipeline() == pipestream::parse::v1::PROCESSING_PIPELINE_VLM) ||
      vlm_collector;
  inputs.heading = heading_options_from(options);
  return inputs;
}

// The CV path reads pixels and the text layer, never the file's own
// dictionaries; what the inspector read from them (title, authors, dates,
// the catalog /Lang) is the same file's account, so the CV document takes
// it unless it already has its own.
void lend_source_meta(const CollectorOutcome& inspector, CollectorOutcome* cv) {
  if (cv->success && inspector.document.has_source_meta() && !cv->document.has_source_meta()) {
    *cv->document.mutable_source_meta() = inspector.document.source_meta();
  }
}

// Why the routed PDF went to the CV path instead of the inspector's
// extraction, and how recognition was scoped there.
std::string cv_route_warning(const PdfClassification& classification,
                             const PdfRouteDecision& route, bool forced) {
  return "pdf inspector classified the document as " +
         std::string(pdf_class_name(classification.pdf_class)) +
         (classification.encoding_issues
              ? " with encoding issues in the text layer, so its extraction was not taken"
              : "") +
         (classification.empty_body
              ? "; its extraction carried no body text, so it was not taken"
              : "") +
         (classification.ocr_recommended
              ? "; it recommended OCR over the text layer, so its extraction was not taken"
              : "") +
         (forced ? "; recognition was forced on every page in place of the embedded layer"
          : route.ocr_pages.empty()
              ? "; the CV path's own per-page heuristic decided recognition"
              : "; recognition restricted to the " + std::to_string(route.ocr_pages.size()) +
                    " page(s) needing OCR");
}

// The inspector's own extraction as the leg's result: the fast path, or
// pipeline NATIVE taking it whatever the classification said.
CollectorOutcome take_inspector_extraction(const ParseInputs& inputs,
                                           const PdfParseResult& parsed,
                                           const PdfRouteDecision& route) {
  PdfParseResult fast = parsed;
  if (inputs.previews) {
    attach_page_previews(inputs.bytes, &fast.outcome.document, inputs.tuning.page_range,
                         [&inputs] {
                           return inputs.context->IsCancelled() ||
                                  std::chrono::system_clock::now() >= inputs.inbound_deadline;
                         },
                         inputs.inbound_deadline);
  }
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
        (parsed.classification.empty_body ? " and its extraction carried no body text" : "") +
        (parsed.classification.ocr_recommended ? " and recommended OCR for it" : "") +
        "; no layout, OCR, or table-structure model ran");
  }
  return fast.outcome;
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
                  *inputs.bytes, inputs.inbound_deadline, inputs.tuning.page_range,
                  [context = inputs.context] { return context->IsCancelled(); });
  const PdfRouteDecision route = route_pdf_by_classification(parsed.classification);
  if (parsed.outcome.success && (route.fast_path || inputs.native_pipeline)) {
    return take_inspector_extraction(inputs, parsed, route);
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
  lend_source_meta(parsed.outcome, &outcome);
  outcome.warnings.push_back(cv_route_warning(parsed.classification, route, forced));
  return outcome;
}

// The collectors one request runs, and whether the pdf collector owns the
// plan whole, which is what turns classification routing on.
struct RoutedPlan {
  std::vector<pipestream::parse::v1::Collector> ids;
  bool pdf_routing = false;
  // True when the routed (not explicitly selected) plan fanned out to the
  // secondary office collector; the leg append_office_fanout adds is always
  // calamine, never the routed primary itself.
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
  // A routed office default fans out to calamine for workbooks when its
  // endpoint is configured. An explicit selection stays verbatim. Word
  // processing and presentation formats have libreoffice alone, so its
  // failure is the parse's failure (all_failed_status), never an empty body.
  if (selected.empty() && inputs.endpoints != nullptr) {
    plan.office_fanout = true;
    append_office_fanout(&plan.ids, inputs.filename.string(), inputs.content_type,
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
    // The fan-out leg reads the same bytes as the routed libreoffice
    // default: beside a live primary its body reading drops and only its
    // claims merge. An explicit selection stays verbatim, readings and all.
    collector.office_fanout =
        office_fanout && id == pipestream::parse::v1::COLLECTOR_CALAMINE;
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
        // The leg's own call ends with this one: a client that cancels
        // mid-leg leaves no collector working for nobody.
        return run_remote_collector(id, inputs.endpoints, inputs.document_id,
                                    inputs.filename.string(), inputs.content_type,
                                    *inputs.bytes, *inputs.collector_rules,
                                    inputs.inbound_deadline,
                                    [context = inputs.context] { return context->IsCancelled(); });
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
  enrich.chart_extraction = inputs.chart_extraction;
  enrich.picture_description_call = inputs.picture_description_call;
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
      derender_charts(collectors->enrich_channel(), enrich, &result->document, inbound_deadline,
                      [context] { return context->IsCancelled(); });
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
                          const std::optional<RepairOptions>& server_repair,
                          const std::string& surface, SourceParse* parsed) {
  const auto& sources = request.sources();
  if (sources.size() != 1 || !sources.Get(0).has_file()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " currently accepts exactly one FileSource containing base64_string");
  }
  const grpc::Status option_status = validate_options(request.options(), surface);
  if (!option_status.ok()) return option_status;
  if (auto remote = validate_remote_services(request.options(), collectors.get(), surface);
      !remote.ok()) {
    return remote;
  }
  static const ChartExtractionPolicy kDefaultChartPolicy = default_chart_extraction_policy();
  auto chart_extraction = resolve_chart_extraction(
      request.options(), collectors != nullptr ? collectors->chart_policy() : kDefaultChartPolicy,
      surface);
  if (!chart_extraction.has_value()) return chart_extraction.error();
  auto picture_description =
      request_picture_description_call(request.options(), *chart_extraction, surface);
  if (!picture_description.has_value()) return picture_description.error();
  // The structural repairs a request opts into join the server's repair
  // pass; the validation runs once the document is final.
  const StructureRequest structure = StructureRequest::from(request.options());
  const std::optional<RepairOptions> repair = repair_for_request(server_repair, structure);
  try {
    const auto& source = sources.Get(0).file();
    auto bytes = std::make_shared<const std::string>(decode_base64(source.base64_string()));
    // A nameless upload gets a name that declares nothing, so the bytes
    // decide its type and route rather than a made-up extension.
    const fs::path requested_name = source.filename().empty() ? "document" : fs::path(source.filename()).filename();
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
                     base.origin().mimetype(), std::move(*chart_extraction),
                     std::move(*picture_description));

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
                            inputs.inbound_deadline, [context] { return context->IsCancelled(); });
      for (const std::string& warning : report.warnings) {
        result.warnings.emplace_back(pipestream::parse::v1::COLLECTOR_VLM, warning);
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
      const grpc::Status structure_status = check_structure(
          result.document, structure, surface, &parsed->structure_findings);
      if (!structure_status.ok()) return structure_status;
      stamp_collector_warnings(&result);
      parsed->filename = requested_name;
      parsed->result = std::move(result);
      return grpc::Status::OK;
    }
    const CvCollector run_cv(context, scheduler, bytes, pdf, cv_offsets, cv_confidence,
                             inputs.heading, inputs.inbound_deadline);

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
                                    "(GRPARSE_PDF_TARGET) for PDF input and does not "
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
    // AcroForm widgets join the finished body as field items, whichever
    // path produced it; the backend that owns the PDF layer supplies them
    // (pdf_form_widgets.h). Appending to the arenas leaves the offset table
    // below describing the same items.
    if (pdf && !context->IsCancelled()) {
      if (auto warning = fold_pdf_form_widgets_from_backend(*bytes, inputs.inbound_deadline,
                                                            &result.document)) {
        result.warnings.emplace_back(pipestream::parse::v1::COLLECTOR_GRPARSE_CV,
                                     std::move(*warning));
      }
    }
    // The document is final here: every surface renders, chunks or delivers
    // exactly this, so the structural rules check this.
    const grpc::Status structure_status =
        check_structure(result.document, structure, surface, &parsed->structure_findings);
    if (!structure_status.ok()) return structure_status;
    // The offset table comes from the final document, so every path has
    // one and it always names the items the response carries. The CV
    // collector's own rows add how each item was read (digital or OCR), but
    // only when that collector is the entire document and the repair pass
    // left its text and arena alone: a merge renumbers arena references and
    // a repair moves text, and a label on the wrong item is worse than none.
    parsed->offsets = chunking::derive_offsets(result.document);
    if (result.succeeded == 1 && !cv_offsets->empty() && !repaired_text) {
      chunking::overlay_sources(*cv_offsets, &parsed->offsets);
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
