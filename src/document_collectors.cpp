#include "grparse/document_collectors.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace ebcdicv1 = ai::pipestream::ebcdic::v1;
namespace emailv1 = ai::pipestream::email::v1;
namespace epubv1 = ai::pipestream::epub::v1;
namespace xmlv1 = ai::pipestream::xml::v1;

namespace grparse {
namespace {

// Upload chunk size, matching the office collector: small enough to
// interleave with response reads, large enough to keep the frame count low.
constexpr size_t kChunkBytes = 256U * 1024U;

// A hung collector must not pin the parse forever. ASR gets a longer leash:
// transcription runs at a fraction of media real time, not of byte count.
constexpr std::chrono::minutes kDeadline{5};
constexpr std::chrono::minutes kAsrDeadline{30};

grpc::StatusCode map_code(grpc::StatusCode code) {
  // The collector's own status classes survive where they are meaningful to
  // the caller; transport-level failures collapse to UNAVAILABLE.
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::UNIMPLEMENTED:
      return code;
    default:
      return grpc::StatusCode::UNAVAILABLE;
  }
}

// The shared tail of every client: turn stream results into an outcome.
// `document_seen` distinguishes "collector too old to know emit_document"
// from a healthy stream; both trailer and document are hard requirements.
CollectorOutcome finish_outcome(const char* name, const grpc::Status& status,
                                bool trailer_seen, bool document_seen,
                                CollectorOutcome outcome) {
  if (!status.ok()) {
    outcome.error = std::string(name) + " collector: " + status.error_message();
    outcome.code = map_code(status.error_code());
    outcome.success = false;
    return outcome;
  }
  if (!trailer_seen) {
    outcome.error = std::string(name) + " collector: stream ended without a terminal status";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    outcome.success = false;
    return outcome;
  }
  if (!document_seen) {
    outcome.error = std::string(name) +
                    " collector: stream ended without a document event; the "
                    "collector predates emit_document";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    outcome.success = false;
    return outcome;
  }
  outcome.success = true;
  return outcome;
}

}  // namespace

CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& bytes) {
  CollectorOutcome outcome;
  auto stub = asrv1::AsrService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kAsrDeadline);
  auto stream = stub->Transcribe(&context);

  asrv1::TranscribeRequest request;
  request.mutable_options()->set_model(model);
  request.mutable_options()->set_emit_document(true);
  bool write_failed = !stream->Write(request);
  size_t offset = 0;
  while (!write_failed && offset < bytes.size()) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    request.mutable_chunk()->set_data(bytes.substr(offset, length));
    offset += length;
    write_failed = !stream->Write(request);
  }
  if (!write_failed) stream->WritesDone();

  bool trailer_seen = false;
  bool document_seen = false;
  asrv1::TranscribeResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_complete()) {
      trailer_seen = true;
    }
    event.Clear();
  }
  return finish_outcome("asr", stream->Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes) {
  CollectorOutcome outcome;
  auto stub = emailv1::EmailParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseEmail(&context);

  emailv1::ParseEmailRequest request;
  emailv1::ParseEmailOptions* options = request.mutable_options();
  options->set_document_id(document_id);
  options->set_filename(filename);
  options->set_content_type(content_type);
  options->set_emit_document(true);
  bool write_failed = !stream->Write(request);
  size_t offset = 0;
  do {
    if (write_failed) break;
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    emailv1::EmailChunk* chunk = request.mutable_chunk();
    chunk->set_data(bytes.substr(offset, length));
    offset += length;
    // The email wire requires the last chunk to say so: a half-close
    // without one is a truncated upload by contract.
    chunk->set_complete(offset >= bytes.size());
    write_failed = !stream->Write(request);
  } while (offset < bytes.size());
  if (!write_failed) stream->WritesDone();

  bool trailer_seen = false;
  bool document_seen = false;
  emailv1::ParseEmailResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      trailer_seen = true;
      for (const auto& warning : event.status().warnings()) {
        outcome.warnings.push_back(warning);
      }
    }
    event.Clear();
  }
  return finish_outcome("email", stream->Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes) {
  CollectorOutcome outcome;
  auto stub = xmlv1::XmlParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseXml(&context);

  xmlv1::ParseXmlRequest request;
  request.mutable_options()->set_emit_document(true);
  bool write_failed = !stream->Write(request);
  size_t offset = 0;
  while (!write_failed && offset < bytes.size()) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    request.set_chunk(bytes.substr(offset, length));
    offset += length;
    write_failed = !stream->Write(request);
  }
  if (!write_failed) stream->WritesDone();

  bool trailer_seen = false;
  bool document_seen = false;
  xmlv1::ParseXmlResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      trailer_seen = true;
      for (const auto& warning : event.status().warnings()) {
        std::string text =
            xmlv1::WarningCode_Name(warning.code()) + ": " + warning.message();
        if (warning.count() > 1) {
          text += " (x" + std::to_string(warning.count()) + ")";
        }
        outcome.warnings.push_back(std::move(text));
      }
    }
    event.Clear();
  }
  return finish_outcome("xml", stream->Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& layout_json,
                                         const std::string& bytes) {
  CollectorOutcome outcome;
  if (layout_json.empty()) {
    // Nothing to dial: the collector cannot decode a byte without a layout,
    // and this client never invents one.
    outcome.error = "ebcdic collector: a parse requires ebcdic_layout_json";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  auto stub = ebcdicv1::EbcdicParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseEbcdic(&context);

  ebcdicv1::ParseEbcdicRequest request;
  request.mutable_options()->set_layout_json(layout_json);
  request.mutable_options()->set_emit_document(true);
  bool write_failed = !stream->Write(request);
  size_t offset = 0;
  while (!write_failed && offset < bytes.size()) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    request.set_chunk(bytes.substr(offset, length));
    offset += length;
    write_failed = !stream->Write(request);
  }
  if (!write_failed) stream->WritesDone();

  bool trailer_seen = false;
  bool document_seen = false;
  ebcdicv1::ParseEbcdicResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      trailer_seen = true;
      for (const auto& warning : event.status().warnings()) {
        outcome.warnings.push_back(ebcdicv1::WarningCode_Name(warning.code()) +
                                   ": " + warning.message());
      }
    }
    event.Clear();
  }
  return finish_outcome("ebcdic", stream->Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes) {
  CollectorOutcome outcome;
  auto stub = epubv1::EpubParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseEpub(&context);

  epubv1::ParseEpubRequest request;
  request.mutable_options()->set_emit_document(true);
  bool write_failed = !stream->Write(request);
  size_t offset = 0;
  while (!write_failed && offset < bytes.size()) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    request.set_chunk(bytes.substr(offset, length));
    offset += length;
    write_failed = !stream->Write(request);
  }
  if (!write_failed) stream->WritesDone();

  bool trailer_seen = false;
  bool document_seen = false;
  epubv1::ParseEpubResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      trailer_seen = true;
      for (const auto& warning : event.status().warnings()) {
        std::string text =
            epubv1::ParseWarningCode_Name(warning.code()) + ": " + warning.message();
        if (!warning.href().empty()) text += " (" + warning.href() + ")";
        outcome.warnings.push_back(std::move(text));
      }
    }
    event.Clear();
  }
  return finish_outcome("epub", stream->Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

}  // namespace grparse
