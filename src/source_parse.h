#pragma once

// The parse the unary surfaces share: one FileSource in, a merged Document
// out. ConvertSource renders exports from it and the two chunkers chunk it,
// so the parse itself lives here rather than in any one of them.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.pb.h"
#include "chunking/chunker.h"
#include "grparse/chart_extraction_policy.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_repair.h"
#include "grparse/page_scheduler.h"

namespace grparse {

// One parsed source: the merged document every conversion surface starts
// from, plus the offset side table when this parse produced a usable one.
struct SourceParse {
  std::filesystem::path filename;
  CoordinatorResult result;
  chunking::OffsetTable offsets;
  // The CV collector's page read quality, when it read any page.
  std::optional<ai::pipestream::parse::v1::ConfidenceScores> confidence;
  // The structural rule findings, when the request asked for a REPORT.
  google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::StructureFinding>
      structure_findings;
};

// The parse every unary surface shares: decode the single FileSource, plan
// the collectors, run them, and merge. The response shaping (exports, chunks)
// belongs to the caller. Blocking throughout, so it runs on a CallExecutor
// worker and never on the thread that reacted to the call; `context` outlives
// that worker because the reactor finishes the call from it.
//
// `surface` names the RPC in every rejection, so a caller learns which of
// the conversion surfaces turned its request down.
grpc::Status validate_options(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface);

// The chart-extraction preset a Convert request resolves to under the
// server's policy, mirroring docling-jobkit's _parse_chart_extraction_options:
// a chart_extraction_custom_config must be allowed by policy (and use an
// allowed engine); otherwise the named chart_extraction_preset, or "default"
// when none is named, must be in the policy's registry. nullopt when the
// request names nothing and sets do_chart_extraction false. INVALID_ARGUMENT
// for an unknown or disallowed preset, PERMISSION_DENIED for a disabled
// custom config or a disallowed engine. Call after validate_options.
std::expected<std::optional<ChartExtractionPreset>, grpc::Status> resolve_chart_extraction(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const ChartExtractionPolicy& policy, const std::string& surface);

grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& server_repair,
                          const std::string& surface, SourceParse* parsed);

}  // namespace grparse
