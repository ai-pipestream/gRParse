#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "ai/pipestream/vlm/v1/vlm_convert.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/warning.h"

namespace grparse {

// The VLM convert leg: rasterize PDF/raster bytes to full-page PNGs and dial
// grpc-vlm-convert ConvertPages. Produces the parse body (unlike enrich, which
// only annotates after merge). Empty target means the leg does not exist.
struct VlmConvertOptions {
  std::string target;
  // Whole-leg ceiling (GRPARSE_VLM_CONVERT_TIMEOUT_MS). VLM pages are slow;
  // default is two minutes.
  std::chrono::milliseconds timeout{120000};
  // Optional per-request VLM endpoint override (GRPARSE_VLM_CONVERT_ENDPOINT
  // or Convert picture/api URL); empty leaves the peer on its configured default.
  std::string endpoint;
  // Mapped from ConvertDocumentOptions VLM selection fields.
  ai::pipestream::vlm::v1::VlmPreset preset =
      ai::pipestream::vlm::v1::VLM_PRESET_UNSPECIFIED;
  std::string preset_raw;
  ai::pipestream::vlm::v1::ResponseFormat response_format =
      ai::pipestream::vlm::v1::RESPONSE_FORMAT_UNSPECIFIED;
  // docling's string value for response_format ("dots_json"), forwarded
  // as ConvertOptions.response_format_raw; empty when no format is named.
  std::string response_format_raw;
  std::string prompt;
  // Nemotron Parse 3.5 passes (docling's NemotronParseOptions), forwarded
  // as ConvertOptions.nemotron_parse; unset keeps the single page pass.
  std::optional<ai::pipestream::vlm::v1::NemotronParseOptions> nemotron_parse;
  // Rasterization DPI for PDF pages; rasters ignore it. Zero keeps 200 DPI.
  double render_dpi = 0.0;
  // Optional ConvertOptions.scale hint forwarded to the peer.
  double scale = 0.0;
  uint32_t concurrency = 0;
  bool abort_on_error = false;
  // Inclusive 1-indexed page span; unset means every page.
  std::optional<std::pair<int, int>> page_range;

  bool enabled() const { return !target.empty(); }
};

// docling's string value for a ResponseFormat ("nemotron_parse_v3_5");
// nullopt for UNSPECIFIED and for a number outside the enum.
std::optional<std::string_view> response_format_name(
    ai::pipestream::parse::v1::ResponseFormat format);

// The ResponseFormat docling's string value names; nullopt for a name
// docling does not define.
std::optional<ai::pipestream::parse::v1::ResponseFormat> response_format_from_name(
    std::string_view name);

// The VLM convert collector's format for a parse ResponseFormat; nullopt for
// a format the collector's mapper cannot read (MINERU2, UNSPECIFIED, and a
// number outside the enum).
std::optional<ai::pipestream::vlm::v1::ResponseFormat> vlm_collector_response_format(
    ai::pipestream::parse::v1::ResponseFormat format);

// The format a VlmModelLocal, VlmModelApi or VlmModelSpec names: the typed
// field when it is set to a member, otherwise the docling name in
// response_format_raw. nullopt when neither names one. validate_options has
// already refused unknown values and a typed field and raw name that
// disagree.
template <typename Message>
std::optional<ai::pipestream::parse::v1::ResponseFormat> named_response_format(
    const Message& message) {
  if (message.has_response_format() &&
      message.response_format() != ai::pipestream::parse::v1::RESPONSE_FORMAT_UNSPECIFIED) {
    return message.response_format();
  }
  if (message.has_response_format_raw()) {
    return response_format_from_name(message.response_format_raw());
  }
  return std::nullopt;
}

// Fill preset / preset_raw / endpoint / scale / response_format from
// ConvertDocumentOptions VLM selection fields. The response format comes
// from vlm_pipeline_model_local or vlm_pipeline_model_api, else from
// vlm_pipeline_custom_config.model_spec; validate_options has refused one
// the collector cannot map. Pure.
void apply_vlm_convert_options(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& convert,
    VlmConvertOptions* options);

// The engine a PROCESSING_PIPELINE_VLM body came from: grpc-vlm-convert
// with the resolved preset, or grpc-lighton-ocr (lighton_ocr.h). Recorded on
// the leg's report, and named ("vlm-convert" or "lighton-ocr") as the
// claimant its pages merge under and the source of its document warnings.
enum class VlmBodyEngine {
  kVlmConvert,
  kLightonOcr,
};

// What became of one page the VLM convert leg sent.
enum class VlmPageStatus {
  // A PageDocument arrived for it and merged.
  kOk,
  // The peer answered PageRaw: the model call failed, or its output could
  // not be mapped into items.
  kFailed,
  // The stream ended with no answer for it.
  kUnanswered,
};

// One sent page's outcome on the grpc-vlm-convert leg.
struct VlmPageOutcome {
  // 1-based, as Document.pages keys are.
  int page_no = 0;
  VlmPageStatus status = VlmPageStatus::kUnanswered;
  // PageRaw.error, the peer's own words; empty unless the page failed.
  std::string error;
  // PageRaw.text: the model's output for a page the peer could not map,
  // byte for byte. The same text is kept on the Document as the page's
  // generation_traces entry, which is how it reaches the caller.
  std::string raw_text;
};

struct VlmConvertReport {
  // Which engine this report is from; set by the leg that ran.
  VlmBodyEngine engine = VlmBodyEngine::kVlmConvert;
  int pages_sent = 0;
  int pages_ok = 0;
  int pages_failed = 0;
  // One entry per page sent, in the order sent, once the stream has ended.
  // Filled by the grpc-vlm-convert leg; a page that was never sent (its
  // render failed) has a warning and no entry.
  std::vector<VlmPageOutcome> pages;
  std::vector<Warning> warnings;
  // True when the peer finished the stream with its trailer and at least
  // one PageDocument merged. A page that failed or went unanswered is a
  // warning beside the success, unless abort_on_error makes it the leg's
  // failure.
  bool success = false;
  grpc::StatusCode code = grpc::StatusCode::OK;
  std::string error;
};

// Classifies a provider's verbatim finish_reason as a VlmStopReason the way
// docling's picture-description engine does: docling's own value names,
// OpenAI "stop" as END_OF_SEQUENCE. Anything else, including an empty value,
// is UNSPECIFIED; gRParse does not guess (upstream's API image path calls
// every unknown value END_OF_SEQUENCE).
ai::pipestream::document::v1::VlmStopReason vlm_stop_reason(std::string_view finish_reason);

// Sets GenerationSource.stop_reason from finish_reason on every generation in
// `document` whose producer left stop_reason UNSPECIFIED. A producer's own
// stop_reason is kept as it is, and finish_reason is never rewritten. Map
// fields are not walked: no map value in the document schema carries a
// GenerationSource.
void type_generation_stop_reasons(ai::pipestream::document::v1::Document* document);

// The formats whose collector mappers keep the model's raw heading depth
// per page (docling's _RAW_HEADING_DEPTH_FORMATS): Chandra HTML,
// DeepSeek-OCR and Unlimited-OCR markdown, dots.ocr JSON, Nemotron Parse
// 2.0 and 3.5.
bool raw_heading_depth_format(ai::pipestream::vlm::v1::ResponseFormat format);

// docling's normalize_section_header_levels, once per joined document: when
// no section header sits at level 1 (a document with none counts), every
// section header moves up one level; otherwise nothing changes.
// convert_vlm_pages applies it after the last page merges when the leg's
// format is a raw-heading-depth one.
void normalize_section_header_levels(ai::pipestream::document::v1::Document* document);

// Rasterize `bytes` and run ConvertPages through `channel`. Merges every
// PageDocument into `document` under claimant "vlm-convert", and keeps a
// PageRaw's model output as a Document.generation_traces entry for its page.
// Never throws; dial / peer failures set report.success false and
// report.error. The stream counts as finished only with its ConvertComplete
// trailer: an OK stream without one, an answer for a page that was not sent
// and a second answer for one page are INTERNAL. A sent page with no answer
// is a PAGE_UNANSWERED warning (INTERNAL under abort_on_error), and a
// trailer whose counts differ from what was sent and arrived is a
// COLLECTOR_COUNT_MISMATCH warning. Pages are
// rendered and streamed one at a time; `cancelled` and `inbound_deadline` are
// checked before each page, and either ends the dial (CANCELLED or
// DEADLINE_EXCEEDED) instead of rasterizing the rest of the document.
VlmConvertReport convert_vlm_pages(const std::shared_ptr<grpc::Channel>& channel,
                                   const VlmConvertOptions& options,
                                   std::shared_ptr<const std::string> bytes, bool pdf,
                                   ai::pipestream::document::v1::Document* document,
                                   CollectorDeadline inbound_deadline = kNoCollectorDeadline,
                                   const std::function<bool()>& cancelled = {});

}  // namespace grparse
