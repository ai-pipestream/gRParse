#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>

#include "ai/pipestream/asr/v1/asr_service.grpc.pb.h"
#include "ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "ai/pipestream/markup/v1/markup_service.grpc.pb.h"
#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "fastwarc/v1/warc_service.grpc.pb.h"
#include "lolhtml/v1/lolhtml_service.grpc.pb.h"

namespace asrv1 = ai::pipestream::asr::v1;
namespace docv1 = ai::pipestream::document::v1;
namespace ebcdicv1 = ai::pipestream::ebcdic::v1;
namespace emailv1 = ai::pipestream::email::v1;
namespace epubv1 = ai::pipestream::epub::v1;
namespace lolv1 = lolhtml::v1;
namespace markupv1 = ai::pipestream::markup::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;
namespace warcv1 = fastwarc::v1;
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

CollectorOutcome collect_markup_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& filename,
                                         const std::string& content_type,
                                         const std::string& bytes) {
  auto stub = markupv1::MarkupParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseMarkup(&context);

  markupv1::ParseMarkupRequest request;
  // The hint spares the collector a sniff and resolves what a sniff cannot:
  // Markdown and AsciiDoc have no reliable signature, but the filename
  // does. An unresolved hint stays MARKUP_FORMAT_UNSPECIFIED, which is the
  // wire's "sniff it".
  request.mutable_options()->set_format(markup_format_for(filename, content_type));
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](markupv1::ParseMarkupRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  return drain_stream<markupv1::ParseMarkupResponse>(
      "markup", *stream,
      [](const markupv1::ParseMarkupResponse& event,
         std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          std::string text =
              markupv1::WarningCode_Name(warning.code()) + ": " + warning.message();
          if (warning.count() > 1) {
            text += " (x" + std::to_string(warning.count()) + ")";
          }
          warnings.push_back(std::move(text));
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

CollectorOutcome collect_lol_html_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& options_json,
                                           const std::string& bytes) {
  CollectorOutcome outcome;
  if (options_json.empty()) {
    // Nothing to dial: the collector extracts what its selector rules name,
    // and this client never invents rules.
    outcome.error = "lol-html collector: a parse requires lol_html_options_json";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  lolv1::ExtractOptions options;
  const auto parsed =
      google::protobuf::util::JsonStringToMessage(options_json, &options);
  if (!parsed.ok()) {
    outcome.error =
        "lol-html collector: lol_html_options_json does not parse as "
        "lolhtml.v1.ExtractOptions: " +
        std::string(parsed.message());
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }

  auto stub = lolv1::LolHtmlService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->Extract(&context);

  lolv1::ExtractRequest request;
  *request.mutable_options() = std::move(options);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](lolv1::ExtractRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  // The one collector wire with no document event: the match stream IS the
  // product, so the fold happens here. One group per rule, its matches and
  // text as source-tagged text items in arrival order. Instance nesting is
  // not reconstructed: matches are a transcript, not a tree; a caller who
  // wants document structure runs the markup collector instead.
  docv1::Document& document = outcome.document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  std::map<std::string, int> groups_by_rule;
  const auto group_ref = [&document, &groups_by_rule](const std::string& rule) {
    auto found = groups_by_rule.find(rule);
    if (found == groups_by_rule.end()) {
      const int index = document.groups_size();
      docv1::GroupItem* group = document.add_groups();
      group->set_self_ref("#/groups/" + std::to_string(index));
      group->mutable_parent()->set_ref("#/body");
      group->set_content_layer(docv1::CONTENT_LAYER_BODY);
      group->set_name(rule);
      group->set_label(docv1::GROUP_LABEL_SECTION);
      document.mutable_body()->add_children()->set_ref(group->self_ref());
      found = groups_by_rule.emplace(rule, index).first;
    }
    return found->second;
  };
  const auto add_text = [&document, &group_ref](const std::string& rule,
                                                std::string text) {
    const int group = group_ref(rule);
    auto* base = document.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(document.texts_size() - 1));
    base->mutable_parent()->set_ref("#/groups/" + std::to_string(group));
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(text);
    base->set_text(std::move(text));
    base->add_source()->mutable_collector()->set_collector("lol-html");
    document.mutable_groups(group)->add_children()->set_ref(base->self_ref());
  };

  bool finished_seen = false;
  bool error_seen = false;
  lolv1::ExtractResponse event;
  while (stream->Read(&event)) {
    switch (event.event_case()) {
      case lolv1::ExtractResponse::kElement: {
        const auto& element = event.element();
        std::string tag = element.tag_name().empty() ? element.tag_name_raw()
                                                     : element.tag_name();
        std::string text = "<" + (tag.empty() ? "match" : tag);
        for (const auto& attribute : element.attributes()) {
          text += " " + attribute.name() + "=\"" + attribute.value() + "\"";
        }
        text += ">";
        add_text(element.rule_id(), std::move(text));
        break;
      }
      case lolv1::ExtractResponse::kText:
        add_text(event.text().rule_id(), event.text().text());
        break;
      case lolv1::ExtractResponse::kComment:
        add_text(event.comment().rule_id(), "<!--" + event.comment().text() + "-->");
        break;
      case lolv1::ExtractResponse::kDoctype: {
        std::string text = "<!DOCTYPE";
        if (event.doctype().has_name()) text += " " + event.doctype().name();
        text += ">";
        add_text(event.doctype().rule_id(), std::move(text));
        break;
      }
      case lolv1::ExtractResponse::kFinished: {
        finished_seen = true;
        const auto& finished = event.finished();
        if (finished.bailed_out()) {
          outcome.warnings.push_back(
              "bailed out before the end of the document: " +
              finished.bail_out_reason());
        }
        // A rule that matched nothing is worth saying out loud: an empty
        // group and a mistyped selector look identical otherwise.
        for (const auto& [rule, count] : finished.matches_by_rule()) {
          if (count == 0) {
            outcome.warnings.push_back("rule '" + rule + "' matched nothing");
          }
        }
        break;
      }
      case lolv1::ExtractResponse::kError:
        // Terminal and in-band by contract; the RPC itself still ends OK.
        error_seen = true;
        outcome.error = "lol-html collector: " +
                        lolv1::ParseErrorCode_Name(event.error().code()) + ": " +
                        event.error().message();
        outcome.code =
            event.error().code() == lolv1::PARSE_ERROR_CODE_MEMORY_LIMIT_EXCEEDED
                ? grpc::StatusCode::RESOURCE_EXHAUSTED
                : grpc::StatusCode::INVALID_ARGUMENT;
        break;
      case lolv1::ExtractResponse::kStarted:
      case lolv1::ExtractResponse::kEndTag:
      default:
        break;
    }
    event.Clear();
  }

  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    outcome.error = std::string("lol-html collector: ") + status.error_message();
    outcome.code = map_code(status.error_code());
    return outcome;
  }
  if (error_seen) return outcome;
  if (!finished_seen) {
    outcome.error = "lol-html collector: stream ended without a terminal event";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    return outcome;
  }
  outcome.success = true;
  return outcome;
}

CollectorOutcome collect_fastwarc_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes) {
  auto stub = warcv1::WarcService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParseWarc(&context);

  warcv1::ParseWarcRequest request;
  warcv1::ParseWarcConfig* config = request.mutable_config();
  // Everything the fold needs is opt-in-able but defaults on; set it anyway
  // so the client does not silently change shape if a server default moves.
  // Compression detection stays unset, which enables magic-byte sniffing of
  // gzip/zstd/lz4 streams. Batching cuts the per-record message count on
  // record-dense archives; batches flush as they fill, so latency is kept.
  config->set_parse_http(true);
  config->set_include_payload(true);
  config->set_include_headers(true);
  config->set_response_batch_size(64);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](warcv1::ParseWarcRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  // Like lol-html, this wire carries no document event and no terminal
  // status: the record stream IS the product, so the fold happens here. One
  // group per record in stream order, the record's salient metadata as
  // source-tagged text items, plus the payload itself when it reads as text.
  CollectorOutcome outcome;
  docv1::Document& document = outcome.document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  // Payload text is capped: a WARC payload can be a whole video, and the
  // Document model is a text plane, not an archive. The cap matches the
  // server's default payload chunk, and record_end.payload_length still
  // reports the full size for the truncation note.
  constexpr size_t kPayloadTextBytes = 64U * 1024U;

  const auto add_text = [&document](int group, std::string text) {
    auto* base = document.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(document.texts_size() - 1));
    base->mutable_parent()->set_ref("#/groups/" + std::to_string(group));
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(text);
    base->set_text(std::move(text));
    base->add_source()->mutable_collector()->set_collector("fastwarc");
    document.mutable_groups(group)->add_children()->set_ref(base->self_ref());
  };

  // A record reads as text when its embedded HTTP Content-Type says so, or
  // when the record was not an HTTP message at all (warcinfo, metadata, and
  // DNS records are text by convention). Binary HTTP payloads stay out; the
  // group's length items still record them.
  const auto looks_textual = [](const warcv1::RecordMetadata& metadata) {
    if (!metadata.has_http_content_type()) return true;
    const std::string& type = metadata.http_content_type();
    return type.starts_with("text/") || type.contains("json") ||
           type.contains("xml") || type.contains("html");
  };

  const auto fold_record = [&document, &add_text, &looks_textual](
                               const warcv1::RecordMetadata& metadata,
                               const std::string& payload, uint64_t payload_length) {
    const int index = document.groups_size();
    docv1::GroupItem* group = document.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(index));
    group->mutable_parent()->set_ref("#/body");
    group->set_content_layer(docv1::CONTENT_LAYER_BODY);
    std::string type_name = warcv1::WarcRecordType_Name(metadata.record_type());
    const std::string prefix = "WARC_RECORD_TYPE_";
    if (type_name.starts_with(prefix)) type_name.erase(0, prefix.size());
    if (type_name.empty()) type_name = std::to_string(metadata.record_type());
    std::ranges::transform(type_name, type_name.begin(),
                           [](unsigned char letter) { return std::tolower(letter); });
    group->set_name(type_name + " @ " + std::to_string(metadata.stream_pos()));
    group->set_label(docv1::GROUP_LABEL_SECTION);
    document.mutable_body()->add_children()->set_ref(group->self_ref());

    add_text(index, "warc-type: " + type_name);
    add_text(index, "stream-pos: " + std::to_string(metadata.stream_pos()));
    if (metadata.has_record_id()) {
      add_text(index, "record-id: " + metadata.record_id());
    }
    if (metadata.has_record_date()) {
      add_text(index, "record-date: " +
                          google::protobuf::util::TimeUtil::ToString(metadata.record_date()));
    }
    if (metadata.has_http_content_type()) {
      add_text(index, "http-content-type: " + metadata.http_content_type());
    }
    add_text(index, "content-length: " + std::to_string(metadata.content_length()));
    add_text(index, "payload-length: " + std::to_string(payload_length));
    if (looks_textual(metadata) && payload_length > 0) {
      std::string text = payload;
      if (payload_length > payload.size()) {
        text += "\n[fastwarc: payload truncated to the first " +
                std::to_string(kPayloadTextBytes) + " of " +
                std::to_string(payload_length) + " bytes]";
      }
      add_text(index, std::move(text));
    }
  };

  // The fold's open-record state: a record_start opens it, payload chunks
  // accumulate (capped), record_end closes and folds it. Chunks arriving
  // outside a record are a server bug and dropped.
  warcv1::RecordMetadata current;
  bool record_open = false;
  std::string payload;
  const auto handle_event = [&](const warcv1::ParseWarcResponse& event) {
    switch (event.kind_case()) {
      case warcv1::ParseWarcResponse::kRecordStart:
        if (record_open) {
          // A start while a record is open means a record_end was lost;
          // fold what arrived rather than dropping it.
          fold_record(current, payload, current.content_length());
          outcome.warnings.push_back("record at stream offset " +
                                     std::to_string(current.stream_pos()) +
                                     " closed without a record_end");
        }
        current = event.record_start().metadata();
        record_open = true;
        payload.clear();
        break;
      case warcv1::ParseWarcResponse::kPayloadChunk:
        if (!record_open) break;
        if (payload.size() < kPayloadTextBytes) {
          const std::string& data = event.payload_chunk().data();
          payload.append(data, 0, std::min(data.size(), kPayloadTextBytes - payload.size()));
        }
        break;
      case warcv1::ParseWarcResponse::kRecordEnd:
        if (!record_open) break;
        fold_record(current, payload, event.record_end().payload_length());
        record_open = false;
        break;
      case warcv1::ParseWarcResponse::kRecordError: {
        const auto& error = event.record_error();
        if (error.recoverable()) {
          // A record whose HTTP headers did not parse: the stream carries
          // on, so the fold does too, with the failure noted.
          outcome.warnings.push_back("record at stream offset " +
                                     std::to_string(error.stream_pos()) +
                                     " skipped: " + error.message());
        } else {
          // Framing is lost; the server ends the stream after this. What
          // was already parsed is kept: a clipped archive is a partial
          // success, matching how the demo treats truncated captures.
          outcome.warnings.push_back("archive truncated by a framing error at stream offset " +
                                     std::to_string(error.stream_pos()) + ": " +
                                     error.message());
        }
        break;
      }
      default:
        break;
    }
  };

  warcv1::ParseWarcResponse event;
  while (stream->Read(&event)) {
    if (event.has_batch()) {
      // The client opted into batching, so it owes the wire a flatten.
      // Items are stream-ordered and never nest, by contract.
      for (const auto& item : event.batch().items()) handle_event(item);
    } else {
      handle_event(event);
    }
    event.Clear();
  }
  if (record_open) {
    // The stream closed inside a record sequence; fold the partial record
    // instead of discarding it.
    fold_record(current, payload, current.content_length());
    outcome.warnings.push_back("stream ended inside the record at stream offset " +
                               std::to_string(current.stream_pos()));
  }

  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    if (document.groups_size() == 0) {
      // Nothing parsed at all: the transport failure is the outcome.
      outcome.error = std::string("fastwarc collector: ") + status.error_message();
      outcome.code = map_code(status.error_code());
      return outcome;
    }
    // Records already folded survive a mid-stream transport failure, the
    // same partial-success reading the framing error above gets.
    outcome.warnings.push_back(std::string("stream failed after ") +
                               std::to_string(document.groups_size()) +
                               " records: " + status.error_message());
  }
  outcome.success = true;
  return outcome;
}

PdfRouteDecision route_pdf_by_classification(const PdfClassification& classification) {
  PdfRouteDecision decision;
  // Document-wide encoding issues make the embedded layer untrustworthy for
  // every class, so the CV run recognizes all pages rather than reading it.
  decision.force_ocr = classification.encoding_issues;
  switch (classification.pdf_class) {
    case PdfClass::kTextBased:
      // The whole text layer is usable: the collector's own Document is the
      // parse result and the CV pipeline never runs for this document. A
      // text-based document that still names OCR pages is not the fast
      // path; its named pages route to recognition like any other
      // classification's. Neither is one whose trailer flagged encoding
      // issues: the wire contract says that text layer is untrustworthy
      // however confident the classification, and when no pages are named
      // the CV path's own heuristic decides recognition (custom-encoded
      // vector fonts routinely classify TEXT_BASED at full confidence while
      // extracting mojibake or nothing).
      decision.fast_path =
          classification.pages_needing_ocr.empty() && !classification.encoding_issues;
      decision.ocr_pages = classification.pages_needing_ocr;
      break;
    case PdfClass::kScanned:
    case PdfClass::kImageBased:
    case PdfClass::kMixed:
      decision.ocr_pages = classification.pages_needing_ocr;
      break;
    case PdfClass::kUnknown:
      // No routing answer (a failed or pre-info stream): leave the CV
      // path's own heuristic in charge.
      break;
  }
  return decision;
}

PdfParseResult collect_pdf(const std::shared_ptr<grpc::Channel>& channel,
                           const std::string& bytes) {
  PdfParseResult result;
  if (channel == nullptr) {
    result.outcome.error = "pdf collector is not configured (GRPARSE_PDF_TARGET)";
    result.outcome.code = grpc::StatusCode::FAILED_PRECONDITION;
    return result;
  }
  auto stub = pdfv1::PdfParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + kDeadline);
  auto stream = stub->ParsePdf(&context);

  pdfv1::ParsePdfRequest request;
  // Mode stays absent, which the wire defines as FULL: the routing decision
  // needs only the info event, but a text-based document's fast path needs
  // the fold, and the fold is built from the page stream.
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](pdfv1::ParsePdfRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  bool trailer_seen = false;
  bool document_seen = false;
  pdfv1::ParsePdfResponse event;
  while (stream->Read(&event)) {
    if (event.has_info()) {
      const pdfv1::PdfInfo& info = event.info();
      switch (info.pdf_type()) {
        case pdfv1::PDF_TYPE_TEXT_BASED:
          result.classification.pdf_class = PdfClass::kTextBased;
          break;
        case pdfv1::PDF_TYPE_SCANNED:
          result.classification.pdf_class = PdfClass::kScanned;
          break;
        case pdfv1::PDF_TYPE_IMAGE_BASED:
          result.classification.pdf_class = PdfClass::kImageBased;
          break;
        case pdfv1::PDF_TYPE_MIXED:
          result.classification.pdf_class = PdfClass::kMixed;
          break;
        default:
          break;
      }
      // Page 0 is never a page (the wire rejects it in requests); drop it
      // defensively so a buggy server cannot inject it into the scheduler.
      for (const uint32_t page : info.pages_needing_ocr()) {
        if (page >= 1 && page <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
          result.classification.pages_needing_ocr.push_back(static_cast<int>(page));
        }
      }
    } else if (event.has_document()) {
      result.outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_status()) {
      for (const auto& warning : event.status().warnings()) {
        result.outcome.warnings.push_back(
            pdfv1::ParseWarningCode_Name(warning.code()) + ": " + warning.message());
      }
      if (event.status().has_encoding_issues()) {
        // The text layer decoded to mojibake somewhere; whatever the
        // classification said, the folded text is not fully trustworthy.
        // The flag rides the classification too so the routing can refuse
        // the fast path for it.
        result.classification.encoding_issues = true;
        result.outcome.warnings.push_back(
            "encoding issues detected in the text layer; extracted text may be untrustworthy");
      }
      trailer_seen = true;
    }
    event.Clear();
  }
  result.outcome = finish_outcome("pdf", stream->Finish(), trailer_seen, document_seen,
                                  std::move(result.outcome));
  return result;
}

CollectorOutcome collect_pdf_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes) {
  return collect_pdf(channel, bytes).outcome;
}

}  // namespace grparse
