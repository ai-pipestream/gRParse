#include "grparse/office_collector.h"

#include <utility>

#include "ai/pipestream/office/v1/office_service.grpc.pb.h"
#include "collectors/collector_support.h"
#include "grparse/docling_map.h"

namespace officev1 = ai::pipestream::office::v1;

namespace grparse {

// The office worker has its own per-document timeout well inside the shared
// kDeadline ceiling; the upload, the status mapping and the deadline are the
// same ones every collector client uses (collectors/collector_support.h).
CollectorOutcome collect_office_document(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& document_id,
    const std::string& filename, const std::string& content_type,
    const std::string& bytes, const OfficeCvEnrichment& enrichment,
    CollectorDeadline inbound_deadline, CollectorCancelled cancelled) {
  CollectorOutcome outcome;
  auto stub = officev1::OfficeRenderService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->StreamPages(&context);

  // The identity rides the first frame, which the worker reads it from; the
  // payload follows in chunks, the last one marked complete.
  officev1::StreamPagesRequest request;
  officev1::DocumentChunk* identity = request.mutable_chunk();
  identity->set_document_id(document_id);
  identity->set_filename(filename);
  identity->set_content_type(content_type);
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/true,
      [&bytes](officev1::StreamPagesRequest& frame, size_t offset, size_t length, bool last) {
        officev1::DocumentChunk* chunk = frame.mutable_chunk();
        chunk->set_data(bytes.data() + offset, length);
        chunk->set_complete(last);
      });

  DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  while (stream->Read(&event)) {
    mapper.consume(event);
    event.Clear();
  }
  upload.join();
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    outcome.error = "libreoffice collector: " + status.error_message();
    outcome.code = map_code(status.error_code());
    return outcome;
  }
  if (!mapper.finished()) {
    outcome.error = "libreoffice collector: stream ended without a terminal status";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    return outcome;
  }
  outcome.warnings = mapper.warnings();
  outcome.document = mapper.take();
  // The worker echoes the request's document_id on document_info, and the
  // fold names the document after it. That id is a per-call correlation
  // key (gRParse sends "<name>#<sequence>"), not a file name: the same file
  // parsed twice would report two different origins. The upload's filename
  // is what the origin and the name describe.
  if (!filename.empty()) {
    if (outcome.document.name() == document_id) outcome.document.set_name(filename);
    if (outcome.document.origin().filename() == document_id) {
      outcome.document.mutable_origin()->set_filename(filename);
    }
  }
  // The hybrid leg: native office text and tables are exact, so the CV
  // engines add only what the office core cannot see on its own renders —
  // figure regions, their classes, and barcode payloads.
  enrich_office_document(enrichment, &outcome.document);
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
