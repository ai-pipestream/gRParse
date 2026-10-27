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
  std::string prompt;
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
// a format the collector's mapper cannot read (everything past PLAINTEXT).
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

struct VlmConvertReport {
  int pages_sent = 0;
  int pages_ok = 0;
  int pages_failed = 0;
  std::vector<std::string> warnings;
  // True when the dial produced at least one PageDocument (or the peer
  // answered complete with pages_ok > 0).
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

// Rasterize `bytes` and run ConvertPages through `channel`. Merges every
// PageDocument into `document` under claimant "vlm-convert". Never throws;
// dial / peer failures set report.success false and report.error. Pages are
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
