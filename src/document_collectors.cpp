#include "grparse/document_collectors.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

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

// Writes the options request already staged in `request`, then the payload
// in kChunkBytes frames, then half-closes. `fill_chunk(frame, offset,
// length, last)` stages one payload frame in place; `always_send_chunk`
// forces one frame even for an empty payload, for wires whose final frame
// carries a completion marker. A failed Write ends the upload early; the
// reason surfaces through the stream's Finish status.
template <typename Stream, typename Request, typename FillChunk>
void upload_stream(Stream& stream, Request& request, const std::string& bytes,
                   bool always_send_chunk, FillChunk fill_chunk) {
  if (!stream.Write(request)) return;
  size_t offset = 0;
  bool chunk_sent = false;
  while (offset < bytes.size() || (always_send_chunk && !chunk_sent)) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    fill_chunk(request, offset, length, offset + length >= bytes.size());
    offset += length;
    chunk_sent = true;
    if (!stream.Write(request)) return;
  }
  stream.WritesDone();
}

// Drains the response stream into an outcome. The document event is the
// payload; the collector's typed events are dropped, because the fold
// already happened where the events were made. `on_status(event, warnings)`
// returns true when the event is the terminal status, appending any
// warnings that status carries.
template <typename Response, typename Stream, typename OnStatus>
CollectorOutcome drain_stream(const char* name, Stream& stream, OnStatus on_status) {
  CollectorOutcome outcome;
  bool trailer_seen = false;
  bool document_seen = false;
  Response event;
  while (stream.Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (on_status(event, outcome.warnings)) {
      trailer_seen = true;
    }
    event.Clear();
  }
  return finish_outcome(name, stream.Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

}  // namespace

CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& bytes) {
  auto stub = asrv1::AsrService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kAsrDeadline);
  auto stream = stub->Transcribe(&context);

  asrv1::TranscribeRequest request;
  request.mutable_options()->set_model(model);
  request.mutable_options()->set_emit_document(true);
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

CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes) {
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

CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes) {
  auto stub = xmlv1::XmlParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseXml(&context);

  xmlv1::ParseXmlRequest request;
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](xmlv1::ParseXmlRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  return drain_stream<xmlv1::ParseXmlResponse>(
      "xml", *stream,
      [](const xmlv1::ParseXmlResponse& event, std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          std::string text =
              xmlv1::WarningCode_Name(warning.code()) + ": " + warning.message();
          if (warning.count() > 1) {
            text += " (x" + std::to_string(warning.count()) + ")";
          }
          warnings.push_back(std::move(text));
        }
        return true;
      });
}

CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& layout_json,
                                         const std::string& bytes) {
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
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
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

CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes) {
  auto stub = epubv1::EpubParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseEpub(&context);

  epubv1::ParseEpubRequest request;
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](epubv1::ParseEpubRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  return drain_stream<epubv1::ParseEpubResponse>(
      "epub", *stream,
      [](const epubv1::ParseEpubResponse& event, std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          std::string text =
              epubv1::ParseWarningCode_Name(warning.code()) + ": " + warning.message();
          if (!warning.href().empty()) text += " (" + warning.href() + ")";
          warnings.push_back(std::move(text));
        }
        return true;
      });
}

}  // namespace grparse
