#include "grparse/document_collectors.h"

#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "collector_support.h"

namespace ebcdicv1 = ai::pipestream::ebcdic::v1;

namespace grparse {

CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const ebcdicv1::ParseOptions& options,
                                         const std::string& bytes,
                                         CollectorDeadline inbound_deadline,
                                         CollectorCancelled cancelled) {
  if (options.layout_source_case() == ebcdicv1::ParseOptions::LAYOUT_SOURCE_NOT_SET) {
    // Nothing to dial: the collector cannot decode a byte without a layout,
    // and this client never invents one.
    CollectorOutcome outcome;
    outcome.error = "ebcdic collector: a parse requires ebcdic_layout";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  auto stub = ebcdicv1::EbcdicParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->ParseEbcdic(&context);

  ebcdicv1::ParseEbcdicRequest request;
  *request.mutable_options() = options;
  request.mutable_options()->set_emit_document(true);
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](ebcdicv1::ParseEbcdicRequest& frame, size_t offset, size_t length, bool /*last*/) {
        frame.set_chunk(bytes.data() + offset, length);
      });
  return drain_stream<ebcdicv1::ParseEbcdicResponse>(
      "ebcdic", *stream, upload,
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
