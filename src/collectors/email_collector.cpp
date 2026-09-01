#include "grparse/document_collectors.h"

#include <string>
#include <vector>

#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "collector_support.h"

namespace emailv1 = ai::pipestream::email::v1;

namespace grparse {

CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes,
                                        CollectorDeadline inbound_deadline) {
  auto stub = emailv1::EmailParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseEmail(&context);

  emailv1::ParseEmailRequest request;
  emailv1::ParseEmailOptions* options = request.mutable_options();
  options->set_document_id(document_id);
  options->set_filename(filename);
  options->set_content_type(content_type);
  options->set_emit_document(true);
  // The email wire requires the final frame to declare itself: a half-close
  // without a complete-marked chunk is a truncated upload by contract, so
  // even an empty payload sends one frame.
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/true,
                [&bytes](emailv1::ParseEmailRequest& frame, size_t offset,
                         size_t length, bool last) {
                  emailv1::EmailChunk* chunk = frame.mutable_chunk();
                  chunk->set_data(bytes.data() + offset, length);
                  chunk->set_complete(last);
                });
  return drain_stream<emailv1::ParseEmailResponse>(
      "email", *stream,
      [](const emailv1::ParseEmailResponse& event,
         std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          warnings.push_back(warning);
        }
        return true;
      });
}
}  // namespace grparse
