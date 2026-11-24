#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/time_util.h>
#include <simdutf.h>

#include "collector_support.h"
#include "fastwarc/v1/warc_service.grpc.pb.h"

namespace docv1 = ai::pipestream::document::v1;
namespace warcv1 = fastwarc::v1;

namespace grparse {
namespace {

// Payload text is capped: a WARC payload can be a whole video, and the
// Document model is a text plane, not an archive. The cap matches the
// server's default payload chunk, and record_end.payload_length still
// reports the full size for the truncation warning.
constexpr size_t kPayloadTextBytes = 64U * 1024U;

// Events per gRPC message. Small records (warcinfo, request, metadata) are a
// few hundred bytes each, so one message per event is mostly framing; the
// server packs this many into a `batch` arm instead. Payload chunks are at
// most 64 KiB, which bounds a batch near 1 MiB.
constexpr uint32_t kResponseBatchSize = 16;

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

// Document text is a proto3 string, which must be UTF-8; a strict decoder
// (Python's, for one) rejects the whole message over a single bad byte.
// WARC payloads are whatever the crawled server sent, so every byte string
// that reaches the Document passes through here. Bytes that start no valid
// sequence become U+FFFD.
std::string utf8_text(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  while (true) {
    const simdutf::result valid = simdutf::validate_utf8_with_errors(bytes.data(), bytes.size());
    if (valid.error == simdutf::error_code::SUCCESS) {
      out.append(bytes);
      return out;
    }
    out.append(bytes.substr(0, valid.count));
    out.append("\xEF\xBF\xBD");
    bytes.remove_prefix(valid.count + 1);
  }
}

// Bytes truncated at the fold cap can end inside a UTF-8 character; that
// incomplete tail is dropped rather than turned into U+FFFD.
std::string_view drop_split_tail(std::string_view bytes) {
  for (size_t back = 1; back <= 3 && back <= bytes.size(); ++back) {
    const auto byte = static_cast<unsigned char>(bytes[bytes.size() - back]);
    if ((byte & 0xC0U) == 0x80U) continue;
    // The last lead byte: keep it only if its sequence is complete.
    const size_t needed = byte >= 0xF0U ? 4 : byte >= 0xE0U ? 3 : byte >= 0xC0U ? 2 : 1;
    if (needed > back) bytes.remove_suffix(back);
    break;
  }
  return bytes;
}

bool is_latin1_charset(std::string charset) {
  std::ranges::transform(charset, charset.begin(),
                         [](unsigned char letter) { return std::tolower(letter); });
  return charset == "iso-8859-1" || charset == "iso8859-1" || charset == "latin1" ||
         charset == "latin-1" || charset == "l1";
}

// A payload as Document text, with how many of the payload's bytes the text
// was made from.
struct PayloadText {
  std::string text;
  size_t bytes_kept = 0;
};

// UTF-8 (and ASCII) passes through; a declared ISO-8859-1 payload is
// transcoded, since every byte maps to one code point. Any other charset is
// not decoded here: its invalid bytes become U+FFFD and `undecoded` names
// the charset for the caller's warning.
PayloadText payload_text(const warcv1::RecordMetadata& metadata, std::string_view bytes,
                         bool cut, std::optional<std::string>& undecoded) {
  const std::string_view whole = cut ? drop_split_tail(bytes) : bytes;
  if (simdutf::validate_utf8(whole.data(), whole.size())) {
    return {std::string(whole), whole.size()};
  }
  if (metadata.has_http_charset() && is_latin1_charset(metadata.http_charset())) {
    std::string out(simdutf::utf8_length_from_latin1(bytes.data(), bytes.size()), '\0');
    out.resize(simdutf::convert_latin1_to_utf8(bytes.data(), bytes.size(), out.data()));
    return {std::move(out), bytes.size()};
  }
  undecoded = metadata.has_http_charset() ? metadata.http_charset() : "undeclared";
  return {utf8_text(whole), whole.size()};
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
    if (same_name(field.name())) return utf8_text(field.value());
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

// The coding the embedded HTTP message says its body is stored under,
// lower-cased: "gzip", "br", "gzip, br". Unset when the record declares
// none, or only "identity", which changes nothing. By convention an archiver
// that decodes a body before storing it renames or drops the header, so a
// declared coding means the payload bytes are still encoded.
std::optional<std::string> content_encoding(const warcv1::RecordMetadata& metadata) {
  if (!metadata.has_http_headers()) return std::nullopt;
  auto declared = header_value(metadata.http_headers(), "Content-Encoding");
  if (!declared) return std::nullopt;
  std::string& coding = *declared;
  std::ranges::transform(coding, coding.begin(),
                         [](unsigned char letter) { return std::tolower(letter); });
  const auto is_space = [](unsigned char letter) { return std::isspace(letter) != 0; };
  coding.erase(coding.begin(), std::ranges::find_if_not(coding, is_space));
  while (!coding.empty() && is_space(static_cast<unsigned char>(coding.back()))) {
    coding.pop_back();
  }
  if (coding.empty() || coding == "identity") return std::nullopt;
  return declared;
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
      case warcv1::ParseWarcResponse::kBatch:
        // Batches hold events in stream order and are never nested.
        for (const auto& item : event.batch().items()) handle(item);
        break;
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

  // A non-recoverable record error. The contract ends the stream after one,
  // and whether it cut an archive short or left nothing at all is known only
  // once the stream has ended, so the fold holds it for the caller.
  struct LostFraming {
    std::string message;
    // The offset in the uncompressed archive at which the failed record
    // began; unset when the service reported none.
    std::optional<uint64_t> stream_pos;
  };
  const std::optional<LostFraming>& lost_framing() const { return lost_framing_; }

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
    // Framing is lost, or the open record's payload could not be read to its
    // end (the record_end that follows still closes that record). Nothing
    // after this is a record; the first such error is the one that counts.
    if (lost_framing_) return;
    lost_framing_.emplace();
    lost_framing_->message = error.message();
    // The contract leaves stream_pos zero when the parser exposes no
    // position, which is what fastwarc-grpc sends for a framing error. A
    // zero is therefore an offset only for the record that is open, whose
    // own start says where it began.
    if (error.stream_pos() > 0) {
      lost_framing_->stream_pos = error.stream_pos();
    } else if (record_open_) {
      lost_framing_->stream_pos = current_.stream_pos();
    }
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
    if (!looks_textual(metadata) || payload_length == 0) return;
    const std::string at_record =
        "record at stream offset " + std::to_string(metadata.stream_pos());
    if (const auto coding = content_encoding(metadata)) {
      // The bytes are a compressed stream, not text in any charset, and this
      // leg does not decode them (see the request below). Run through the
      // charset path they would come out as U+FFFD and read as the page's
      // text, so the body is left out and the record says why.
      outcome_.warnings.push_back(
          Warning(docv1::WARNING_CODE_PAYLOAD_NOT_DECODED,
                  at_record + ": payload stored under Content-Encoding " + *coding +
                      " is not decoded; its text is left out")
              .on_item(group->self_ref()));
      return;
    }
    const bool cut = payload_length > payload.size();
    std::optional<std::string> undecoded;
    PayloadText text = payload_text(metadata, payload, cut, undecoded);
    if (undecoded && undecoded_charsets_.insert(*undecoded).second) {
      outcome_.warnings.push_back("payload text in charset " + *undecoded +
                                  " is not decoded; invalid UTF-8 bytes became U+FFFD");
    }
    add_text(index, std::move(text.text));
    if (cut) {
      // The cut is said beside the text, never inside it: the item's text
      // goes on to chunking and embedding as the record's content.
      outcome_.warnings.push_back(
          Warning(docv1::WARNING_CODE_PAYLOAD_TRUNCATED,
                  at_record + ": payload text holds the first " +
                      std::to_string(text.bytes_kept) + " of " +
                      std::to_string(payload_length) + " bytes")
              .on_item("#/texts/" + std::to_string(document_.texts_size() - 1))
              .kept(text.bytes_kept, payload_length));
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
  std::optional<LostFraming> lost_framing_;
  // Charsets already warned about, so a crawl of one site warns once.
  std::set<std::string> undecoded_charsets_;
};

}  // namespace

CollectorOutcome collect_fastwarc_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline,
                                           CollectorCancelled cancelled) {
  auto stub = warcv1::WarcService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->ParseWarc(&context);

  warcv1::ParseWarcRequest request;
  warcv1::ParseWarcConfig* config = request.mutable_config();
  // The fold reads the embedded HTTP message. parse_http already defaults to
  // true when unset; it is set anyway so the request says what the fold
  // depends on. Compression detection, payloads and header blocks stay at
  // their defaults (on), which enables magic-byte sniffing of gzip/zstd/lz4
  // streams.
  config->set_parse_http(true);
  // decode_http_payload stays unset, so a body arrives as it was stored. The
  // service can decode content encodings, but the contract makes a payload
  // that fails to read a non-recoverable error that ends the stream, and
  // fastwarc-grpc 2.0.1 ends it the same way at a record whose coding it has
  // no decoder for: asking would let one damaged or oddly labelled body cost
  // every record after it. The fold leaves a still-encoded body out instead,
  // under WARNING_CODE_PAYLOAD_NOT_DECODED.
  config->set_response_batch_size(kResponseBatchSize);
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](warcv1::ParseWarcRequest& frame, size_t offset, size_t length, bool /*last*/) {
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

  upload.join();
  const grpc::Status status = stream->Finish();
  // A leg cut off by its deadline or by cancellation is not a clipped
  // archive: the server never got to say where the archive ended, and the
  // records folded so far are whatever arrived before the cut. A failed call
  // that folded nothing has only its failure to report.
  if (!status.ok() &&
      (fold.records() == 0 || status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
       status.error_code() == grpc::StatusCode::CANCELLED)) {
    outcome.error = std::string("fastwarc collector: ") + status.error_message();
    outcome.code = map_code(status.error_code());
    return outcome;
  }
  if (const auto& lost = fold.lost_framing()) {
    if (fold.records() == 0) {
      // The service read the bytes and framed no record in them, then closed
      // the stream OK: that is how it answers a file that is no archive, or
      // one damaged from its first byte. There is no document to hand back,
      // and an empty one under a warning would read as a parse that worked.
      // The call itself went through, so the failure is the input's.
      outcome.error = "fastwarc collector: no WARC record could be read: " + lost->message;
      outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
      return outcome;
    }
    // A clipped archive is a partial success: the records before the cut are
    // real and are kept, and the cut is typed so a caller can tell this
    // document from a whole archive. The archive's full size is not known
    // (the offset counts uncompressed bytes), so only the kept side is said.
    Warning truncated(
        docv1::WARNING_CODE_ARCHIVE_TRUNCATED,
        "archive truncated at " +
            (lost->stream_pos ? "stream offset " + std::to_string(*lost->stream_pos)
                              : std::string("an offset the service did not report")) +
            " (records read: " + std::to_string(fold.records()) + "): " + lost->message);
    if (lost->stream_pos) truncated = std::move(truncated).kept(*lost->stream_pos);
    outcome.warnings.push_back(std::move(truncated));
  }
  if (!status.ok()) {
    // Records already folded survive a mid-stream transport failure, the
    // same partial-success reading a clipped archive gets, and the caller is
    // told the stream never reached its end.
    outcome.warnings.push_back(
        Warning(docv1::WARNING_CODE_COLLECTOR_STREAM_INCOMPLETE,
                std::string("stream failed after ") + std::to_string(fold.records()) +
                    " records: " + status.error_message()));
  }
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
