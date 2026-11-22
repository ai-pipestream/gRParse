#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/lightonocr/v1/lighton_ocr.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/vlm_convert.h"

namespace grparse {

// The LightOnOCR leg: grpc-lighton-ocr runs LightOnOCR-3 on its own engine
// over ParseDocument, one bidirectional stream per document. It produces
// the parse body of a PROCESSING_PIPELINE_VLM request that names the
// LightOnOCR model, in place of grpc-vlm-convert's LightOnOCR preset, when
// GRPARSE_LIGHTON_TARGET is set. Empty target means the leg does not exist.
struct LightonOcrOptions {
  std::string target;
  // Whole-leg ceiling (GRPARSE_LIGHTON_TIMEOUT_MS). The engine runs on the
  // CPU and a page takes seconds to minutes, so the default is five minutes.
  std::chrono::milliseconds timeout{300000};
  // The checkpoint the peer serves (GRPARSE_LIGHTON_MODEL, 0.8b or 4b). The
  // wire requires one, and a checkpoint other than the served one is the
  // peer's FAILED_PRECONDITION.
  ai::pipestream::lightonocr::v1::LightonModel model =
      ai::pipestream::lightonocr::v1::LIGHTON_MODEL_0_8B;

  bool enabled() const { return !target.empty(); }
};

// The claimant every item the LightOnOCR leg merges carries.
inline constexpr const char* kLightonCollectorName = "lighton-ocr";

// The engine for a VLM pipeline request whose selection resolved to
// `resolved` (apply_vlm_convert_options): grpc-lighton-ocr when the request
// names the LightOnOCR model (the LIGHTON_OCR preset) and the leg is
// configured, grpc-vlm-convert otherwise. Pure. Once LightOnOCR is chosen
// there is no fallback: its failure is the parse's.
VlmBodyEngine choose_vlm_body_engine(const VlmConvertOptions& resolved,
                                     bool lighton_configured);

// Rasterize `bytes` and run ParseDocument through `channel`: ParseOptions,
// then one PageImage per page of the span `vlm.page_range` names (every
// page when unset), rendered at `vlm.render_dpi` and sent one at a time on
// their own thread while the answers are read (ConcurrentUpload). The
// wire numbers the pages sent 1..n, so a span that starts past page 1 is
// mapped back to the document's page numbers before the merge. Each
// PageDocument merges into `document` under claimant "lighton-ocr", in page
// order whatever order the pages came back in. A PageFailure is a warning;
// with `vlm.abort_on_error` it fails the leg. A leg with no converted page,
// a peer error, a missing ParseComplete trailer, or a page event outside
// the pages sent fails the leg (report.success false, report.code and
// report.error set). Never throws. `cancelled` and `inbound_deadline` end
// the call (CANCELLED or DEADLINE_EXCEEDED) instead of rendering the rest.
VlmConvertReport convert_lighton_pages(const std::shared_ptr<grpc::Channel>& channel,
                                       const LightonOcrOptions& options,
                                       const VlmConvertOptions& vlm,
                                       std::shared_ptr<const std::string> bytes, bool pdf,
                                       ai::pipestream::document::v1::Document* document,
                                       CollectorDeadline inbound_deadline = kNoCollectorDeadline,
                                       const std::function<bool()>& cancelled = {});

}  // namespace grparse
