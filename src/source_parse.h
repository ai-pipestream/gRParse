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
#include "grparse/chart_derender.h"
#include "grparse/chart_extraction_policy.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_render.h"
#include "grparse/document_repair.h"
#include "grparse/page_scheduler.h"

namespace grparse {

// One parsed source: the merged document every conversion surface starts
// from, plus the offset table of its text stream.
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

// The bytes a request's sources decode to while the parse holds them: each
// FileSource's base64 payload decoded (three bytes per four characters).
// The unary surfaces charge it to the in-flight budget beside the wire size,
// since the decoded copy lives as long as the parse does.
uint64_t decoded_source_bytes(
    const google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::Source>& sources);

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

// The DocLang export options a request carries: image_export_mode (unset or
// UNSPECIFIED leaves each DocLang format at its own docling default) and
// doclang_include_namespace. validate_options has already turned down the
// combinations the renderers reject.
DoclangOptions doclang_options(const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

// Docling's picture_description_api prompt, params and headers in the
// enrich service's typed form. The params map's names that have a typed
// field travel as one (model; max_tokens or its alias max_completion_tokens;
// temperature; top_p; seed), each checked for its type and range; any other
// name is refused by name. Headers become VlmHeader entries in name order;
// a name HTTP does not allow, one the HTTP client sets itself, two names
// differing only in case, or a value holding a control character is
// refused naming the header, never its value. INVALID_ARGUMENT on any
// refusal.
std::expected<PictureDescriptionCall, grpc::Status> picture_description_call(
    const ai::pipestream::parse::v1::PictureDescriptionApi& api, const std::string& surface);

// The request's picture-description call, resolved once per parse (empty
// without picture_description_api). grpc-enrich sends the headers to every
// per-request endpoint of the job, so headers alongside a chart-extraction
// preset that names a different endpoint, with the chart leg on, are
// INVALID_ARGUMENT rather than handed to that endpoint.
std::expected<PictureDescriptionCall, grpc::Status> request_picture_description_call(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const std::optional<ChartExtractionPreset>& chart_extraction, const std::string& surface);

grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& server_repair,
                          const std::string& surface, SourceParse* parsed);

}  // namespace grparse
