#pragma once

// The parse the unary surfaces share: one FileSource in, a merged Document
// out. ConvertSource renders exports from it and the two chunkers chunk it,
// so the parse itself lives here rather than in any one of them.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.pb.h"
#include "chunking/chunker.h"
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
};

// The parse every unary surface shares: decode the single FileSource, plan
// the collectors, run them, and merge. The response shaping (exports, chunks)
// belongs to the caller. Blocking throughout, so it runs on a CallExecutor
// worker and never on the thread that reacted to the call; `context` outlives
// that worker because the reactor finishes the call from it.
//
// `surface` names the RPC in every rejection, so a caller learns which of
// the conversion surfaces turned its request down.
grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& repair,
                          const std::string& surface, SourceParse* parsed);

}  // namespace grparse
