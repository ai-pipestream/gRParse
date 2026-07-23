#include "grparse/office_collector.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "ai/pipestream/office/v1/office_service.grpc.pb.h"
#include "grparse/docling_map.h"

namespace officev1 = ai::pipestream::office::v1;

namespace grparse {
namespace {

// Upload chunk size. Small enough to interleave with response reads, large
// enough that a 50 MiB document stays under a few hundred frames.
constexpr size_t kChunkBytes = 256U * 1024U;

// A hung collector must not pin the parse forever; the office worker has
// its own per-document timeout well inside this ceiling.
constexpr std::chrono::minutes kDeadline{5};

grpc::StatusCode map_code(grpc::StatusCode code) {
  // The collector's own status classes survive where they are meaningful to
  // the caller; transport-level failures collapse to UNAVAILABLE.
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return code;
    default:
      return grpc::StatusCode::UNAVAILABLE;
  }
}

}  // namespace

CollectorOutcome collect_office_document(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& document_id,
    const std::string& filename, const std::string& content_type,
    const std::string& bytes) {
  CollectorOutcome outcome;
  auto stub = officev1::OfficeRenderService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->StreamPages(&context);

  bool write_failed = false;
  size_t offset = 0;
  do {
    officev1::StreamPagesRequest request;
    officev1::DocumentChunk* chunk = request.mutable_chunk();
    if (offset == 0) {
      chunk->set_document_id(document_id);
      chunk->set_filename(filename);
      chunk->set_content_type(content_type);
    }
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    chunk->set_data(bytes.substr(offset, length));
    offset += length;
    chunk->set_complete(offset >= bytes.size());
    if (!stream->Write(request)) {
      // The server closed early; the failure reason arrives with Finish
      // after the reads drain.
      write_failed = true;
      break;
    }
  } while (offset < bytes.size());
  if (!write_failed) stream->WritesDone();

  DoclingMapper mapper;
  officev1::StreamPagesResponse event;
  while (stream->Read(&event)) {
    mapper.consume(event);
    event.Clear();
  }
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
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
