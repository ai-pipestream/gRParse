#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/time_util.h>

#include "collector_support.h"
#include "fastwarc/v1/warc_service.grpc.pb.h"

namespace docv1 = ai::pipestream::document::v1;
namespace warcv1 = fastwarc::v1;

namespace grparse {
namespace {

// Payload text is capped: a WARC payload can be a whole video, and the
// Document model is a text plane, not an archive. The cap matches the
// server's default payload chunk, and record_end.payload_length still
// reports the full size for the truncation note.
constexpr size_t kPayloadTextBytes = 64U * 1024U;

// A record reads as text when its embedded HTTP Content-Type says so, or
// when the record was not an HTTP message at all (warcinfo, metadata, and
// DNS records are text by convention). Binary HTTP payloads stay out; the
// group's length items still record them.
bool looks_textual(const warcv1::RecordMetadata& metadata) {
  if (!metadata.has_http_content_type()) return true;
  const std::string& type = metadata.http_content_type();
  return type.starts_with("text/") || type.contains("json") ||
         type.contains("xml") || type.contains("html");
}

// Header blocks arrive lossless: ordered name/value byte pairs, original
// case preserved, duplicates kept. Both WARC and HTTP define names
// case-insensitively, so the lookup does too, and returns the first match.
std::optional<std::string> header_value(const warcv1::HeaderBlock& block,
                                        std::string_view name) {
  const auto same_name = [&name](const std::string& candidate) {
    if (candidate.size() != name.size()) return false;
    for (size_t index = 0; index < name.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(candidate[index])) !=
          std::tolower(static_cast<unsigned char>(name[index]))) {
        return false;
      }
    }
    return true;
  };
  for (const auto& field : block.fields()) {
    if (same_name(field.name())) return field.value();
  }
  return std::nullopt;
}

// The status line arrives as raw bytes ("HTTP/1.1 200 OK"); the code is its
// second token. Anything that does not read as three digits leaves the
// status unset rather than guessed at.
std::optional<int> status_code(const std::string& status_line) {
  const size_t version_end = status_line.find(' ');
  if (version_end == std::string::npos) return std::nullopt;
  const size_t begin = status_line.find_first_not_of(' ', version_end);
  if (begin == std::string::npos) return std::nullopt;
  size_t end = begin;
  while (end < status_line.size() &&
         std::isdigit(static_cast<unsigned char>(status_line[end])) != 0) {
    ++end;
  }
  if (end - begin != 3) return std::nullopt;
  return std::stoi(status_line.substr(begin, 3));
}

// Like lol-html, this wire carries no document event and no terminal
// status: the record stream IS the product, so the fold happens here. One
// group per record in stream order, the record's salient metadata as
// source-tagged text items, plus the payload itself when it reads as text.
class WarcFold {
 public:
  explicit WarcFold(CollectorOutcome& outcome)
      : outcome_(outcome), document_(outcome.document) {
    document_.mutable_body()->set_self_ref("#/body");
    document_.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
    document_.mutable_furniture()->set_self_ref("#/furniture");
    document_.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  }

  // The fold's open-record state: a record_start opens it, payload chunks
  // accumulate (capped), record_end closes and folds it. Chunks arriving
  // outside a record are a server bug and dropped.
  void handle(const warcv1::ParseWarcResponse& event) {
    switch (event.kind_case()) {
      case warcv1::ParseWarcResponse::kRecordStart:
        if (record_open_) {
          // A start while a record is open means a record_end was lost;
          // fold what arrived rather than dropping it.
          fold_record(current_, payload_, current_.content_length());
          outcome_.warnings.push_back("record at stream offset " +
                                      std::to_string(current_.stream_pos()) +
                                      " closed without a record_end");
        }
        current_ = event.record_start().metadata();
        record_open_ = true;
        payload_.clear();
        break;
      case warcv1::ParseWarcResponse::kPayloadChunk:
        if (!record_open_) break;
        if (payload_.size() < kPayloadTextBytes) {
          const std::string& data = event.payload_chunk().data();
          payload_.append(data, 0, std::min(data.size(), kPayloadTextBytes - payload_.size()));
        }
        break;
      case warcv1::ParseWarcResponse::kRecordEnd:
        if (!record_open_) break;
        fold_record(current_, payload_, event.record_end().payload_length());
        record_open_ = false;
        break;
      case warcv1::ParseWarcResponse::kRecordError:
        note_record_error(event.record_error());
        break;
      default:
        break;
    }
  }

  // The stream closed; a record sequence still open is folded instead of
  // discarded.
  void close() {
    if (!record_open_) return;
    fold_record(current_, payload_, current_.content_length());
    outcome_.warnings.push_back("stream ended inside the record at stream offset " +
                                std::to_string(current_.stream_pos()));
  }

  int records() const { return document_.groups_size(); }

 private:
  void note_record_error(const warcv1::RecordError& error) {
    if (error.recoverable()) {
      // A record whose HTTP headers did not parse: the stream carries on, so
      // the fold does too, with the failure noted.
      outcome_.warnings.push_back("record at stream offset " +
                                  std::to_string(error.stream_pos()) +
                                  " skipped: " + error.message());
      return;
    }
    // Framing is lost; the server ends the stream after this. What was
    // already parsed is kept: a clipped archive is a partial success,
    // matching how the demo treats truncated captures.
    outcome_.warnings.push_back("archive truncated by a framing error at stream offset " +
                                std::to_string(error.stream_pos()) + ": " + error.message());
  }

  void add_text(int group, std::string text) {
    auto* base = document_.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(document_.texts_size() - 1));
    base->mutable_parent()->set_ref("#/groups/" + std::to_string(group));
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(text);
    base->set_text(std::move(text));
    base->add_source()->mutable_collector()->set_collector("fastwarc");
    document_.mutable_groups(group)->add_children()->set_ref(base->self_ref());
  }

  // Document.origin is per-document while an archive holds many records, so
  // the document's web provenance comes from exactly one of them: the first
  // `response` that declares a WARC-Target-URI, or the first record declaring
  // one at all until a response arrives and supersedes it. Every field is set
  // only when the record actually carries it; nothing is invented from a
  // default.
  void capture_web(const warcv1::RecordMetadata& metadata) {
    const bool is_response =
        metadata.record_type() == warcv1::WARC_RECORD_TYPE_RESPONSE;
    if (web_captured_ && (web_from_response_ || !is_response)) return;
    const auto target = header_value(metadata.warc_headers(), "WARC-Target-URI");
    if (!target) return;
    docv1::WebMeta* web = document_.mutable_origin()->mutable_web();
    web->Clear();
    web->set_target_uri(*target);
    if (metadata.has_record_date()) {
      // WARC-Date arrives parsed; the typed instant carries it and the raw
      // twin keeps the RFC 3339 rendering for readers of the source form.
      *web->mutable_crawl_time() = metadata.record_date();
      web->set_crawl_time_raw(
          google::protobuf::util::TimeUtil::ToString(metadata.record_date()));
    }
    if (metadata.has_http_headers()) capture_http(metadata.http_headers(), web);
    // The declared digests are the crawl corpus's content-identity keys and
    // have no typed slot of their own; they ride the header map beside the
    // response headers, under their WARC names.
    for (const std::string_view name : {"warc-payload-digest", "warc-block-digest"}) {
      if (const auto digest = header_value(metadata.warc_headers(), name)) {
        (*web->mutable_headers())[std::string(name)] = *digest;
      }
    }
    web_captured_ = true;
    web_from_response_ = is_response;
  }

  // The response headers a retrieval pipeline reads: the status it answered
  // with, the type it fetched, the freshness pair, and the language it
  // declared. Names are lower-cased because WebMeta.headers says they are.
  static void capture_http(const warcv1::HeaderBlock& http, docv1::WebMeta* web) {
    if (http.has_status_line()) {
      if (const auto status = status_code(http.status_line())) {
        web->set_http_status(*status);
      }
    }
    for (const std::string_view name :
         {"content-type", "last-modified", "content-language", "etag"}) {
      if (const auto value = header_value(http, name)) {
        (*web->mutable_headers())[std::string(name)] = *value;
      }
    }
    if (const auto language = header_value(http, "content-language")) {
      web->set_content_language(*language);
    }
  }

  // One record's group: the metadata items every record carries, then its
  // payload when that reads as text.
  void fold_record(const warcv1::RecordMetadata& metadata, const std::string& payload,
                   uint64_t payload_length) {
    capture_web(metadata);
    const int index = document_.groups_size();
    docv1::GroupItem* group = document_.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(index));
    group->mutable_parent()->set_ref("#/body");
    group->set_content_layer(docv1::CONTENT_LAYER_BODY);
    const std::string type_name = record_type_name(metadata);
    group->set_name(type_name + " @ " + std::to_string(metadata.stream_pos()));
    group->set_label(docv1::GROUP_LABEL_SECTION);
    document_.mutable_body()->add_children()->set_ref(group->self_ref());

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
  }

  // The record's type as the group names it: the enum's own name without its
  // prefix, lower-cased, or the raw number when the enum has no name for it.
  static std::string record_type_name(const warcv1::RecordMetadata& metadata) {
    std::string type_name = warcv1::WarcRecordType_Name(metadata.record_type());
    const std::string prefix = "WARC_RECORD_TYPE_";
    if (type_name.starts_with(prefix)) type_name.erase(0, prefix.size());
    if (type_name.empty()) type_name = std::to_string(metadata.record_type());
    std::ranges::transform(type_name, type_name.begin(),
                           [](unsigned char letter) { return std::tolower(letter); });
    return type_name;
  }

  CollectorOutcome& outcome_;
  docv1::Document& document_;
  warcv1::RecordMetadata current_;
  std::string payload_;
  bool record_open_ = false;
  bool web_captured_ = false;
  bool web_from_response_ = false;
};

}  // namespace

CollectorOutcome collect_fastwarc_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline) {
  auto stub = warcv1::WarcService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseWarc(&context);

  warcv1::ParseWarcRequest request;
  warcv1::ParseWarcConfig* config = request.mutable_config();
  // The server's proto3 default for parse_http is false, unlike the Python
  // and Rust iterators it mirrors, and the fold reads the embedded HTTP
  // message; ask for it explicitly. Compression detection stays unset, which
  // enables magic-byte sniffing of gzip/zstd/lz4 streams.
  config->set_parse_http(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](warcv1::ParseWarcRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  CollectorOutcome outcome;
  WarcFold fold(outcome);
  warcv1::ParseWarcResponse event;
  while (stream->Read(&event)) {
    fold.handle(event);
    event.Clear();
  }
  fold.close();

  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    if (fold.records() == 0) {
      // Nothing parsed at all: the transport failure is the outcome.
      outcome.error = std::string("fastwarc collector: ") + status.error_message();
      outcome.code = map_code(status.error_code());
      return outcome;
    }
    // Records already folded survive a mid-stream transport failure, the
    // same partial-success reading the framing error above gets.
    outcome.warnings.push_back(std::string("stream failed after ") +
                               std::to_string(fold.records()) +
                               " records: " + status.error_message());
  }
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
