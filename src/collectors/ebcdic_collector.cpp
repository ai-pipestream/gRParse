#include "grparse/document_collectors.h"

#include <string>
#include <vector>

#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "collector_support.h"

namespace ebcdicv1 = ai::pipestream::ebcdic::v1;

namespace grparse {

CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& layout_json,
                                         const std::string& bytes,
                                         CollectorDeadline inbound_deadline) {
  if (layout_json.empty()) {
    // Nothing to dial: the collector cannot decode a byte without a layout,
    // and this client never invents one.
    CollectorOutcome outcome;
    outcome.error = "ebcdic collector: a parse requires ebcdic_layout_json";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  auto stub = ebcdicv1::EbcdicParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseEbcdic(&context);

  ebcdicv1::ParseEbcdicRequest request;
  request.mutable_options()->set_layout_json(layout_json);
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](ebcdicv1::ParseEbcdicRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  return drain_stream<ebcdicv1::ParseEbcdicResponse>(
      "ebcdic", *stream,
      [](const ebcdicv1::ParseEbcdicResponse& event,
         std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          warnings.push_back(ebcdicv1::WarningCode_Name(warning.code()) + ": " +
                             warning.message());
        }
        return true;
      });
}
}  // namespace grparse
