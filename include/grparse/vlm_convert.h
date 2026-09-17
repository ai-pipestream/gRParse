#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
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

// Fill preset / preset_raw / endpoint / scale from ConvertDocumentOptions VLM
// selection fields. Pure.
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

// Rasterize `bytes` and run ConvertPages through `channel`. Merges every
// PageDocument into `document` under claimant "vlm-convert". Never throws;
// dial / peer failures set report.success false and report.error.
VlmConvertReport convert_vlm_pages(const std::shared_ptr<grpc::Channel>& channel,
                                   const VlmConvertOptions& options,
                                   std::shared_ptr<const std::string> bytes, bool pdf,
                                   ai::pipestream::document::v1::Document* document,
                                   CollectorDeadline inbound_deadline = kNoCollectorDeadline);

}  // namespace grparse
