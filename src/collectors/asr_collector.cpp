#include "grparse/document_collectors.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "collector_support.h"

namespace asrv1 = ai::pipestream::asr::v1;

namespace grparse {

CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& filename,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline) {
  auto stub = asrv1::AsrService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kAsrDeadline));
  auto stream = stub->Transcribe(&context);

  asrv1::TranscribeRequest request;
  request.mutable_options()->set_model(model);
  request.mutable_options()->set_emit_document(true);
  // The transcriber can only stamp the document's identity and register
  // speakers when asked; diarization stays on unless the deployment turns
  // it off (GRPARSE_ASR_DIARIZE=0).
  request.mutable_options()->set_filename(filename);
  static const bool diarize = [] {
    const char* value = std::getenv("GRPARSE_ASR_DIARIZE");
    return value == nullptr || std::string_view(value) != "0";
  }();
  request.mutable_options()->set_diarize(diarize);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](asrv1::TranscribeRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.mutable_chunk()->set_data(bytes.data() + offset, length);
                });
  return drain_stream<asrv1::TranscribeResponse>(
      "asr", *stream,
      [](const asrv1::TranscribeResponse& event, std::vector<std::string>&) {
        return event.has_complete();
      });
}
}  // namespace grparse
