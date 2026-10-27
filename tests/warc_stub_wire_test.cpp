// Proves the vendored fastwarc stubs decode the bytes the shipping server
// actually writes.
//
// The collector tests stand up a fake WarcService built from these same
// generated classes, so client and fake agree by construction however skewed
// the contract is: that is exactly how `collectors/warc.proto` drifted a full
// field number away from the server without a single test failing. This test
// therefore never serializes anything with the generated code. It writes the
// protobuf wire format out longhand -- tags and varints by hand, against the
// encoding spec, with the field numbers taken from the server's proto -- and
// asserts that the generated classes land those bytes in the fields the
// server put them in. A re-vendoring of the wrong file fails here.
//
// The numbers below are the fastwarc-grpc 1.1.0 crate's (crates.io,
// published from chatnoir-resiliparse). The contract gRParse vendored before
// that carried record_index at field 1 of three messages, a HeaderBlock
// encoding at 3, and digest verdicts on RecordEnd; 1.1.0 dropped all of
// them and renumbered what followed.

#include <cstdint>
#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include <google/protobuf/util/time_util.h>

#include "fastwarc/v1/warc_service.pb.h"
#include "support/check.h"

namespace warcv1 = fastwarc::v1;

namespace {

using grparse_test::require;

// ---- longhand wire-format writers ------------------------------------------
//
// Base-128 varint, low group first, continuation bit on every group but the
// last. This is the whole of the protobuf integer encoding.
std::string varint(uint64_t value) {
  std::string out;
  do {
    auto group = static_cast<unsigned char>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) group |= 0x80U;
    out.push_back(static_cast<char>(group));
  } while (value != 0);
  return out;
}

// A field key is the field number shifted left three with the wire type in
// the low three bits: 0 for varints, 2 for length-delimited.
std::string key(uint32_t field, uint32_t wire_type) {
  return varint((static_cast<uint64_t>(field) << 3U) | wire_type);
}

std::string varint_field(uint32_t field, uint64_t value) {
  return key(field, 0) + varint(value);
}

std::string bytes_field(uint32_t field, const std::string& value) {
  return key(field, 2) + varint(value.size()) + value;
}

// ---- messages assembled from the server's field numbers --------------------

// fastwarc.v1.HeaderField: name = 1 (bytes), value = 2 (bytes).
std::string header_field(const std::string& name, const std::string& value) {
  return bytes_field(1, name) + bytes_field(2, value);
}

// fastwarc.v1.HeaderBlock: status_line = 1, fields = 2, raw_block = 3.
std::string header_block(const std::string& status_line,
                         const std::string& fields, const std::string& raw_block) {
  std::string out;
  if (!status_line.empty()) out += bytes_field(1, status_line);
  out += fields;
  out += bytes_field(3, raw_block);
  return out;
}

// ---- the tests -------------------------------------------------------------

// The un-foolable core: a RecordMetadata written as literal bytes, every one
// of them named. Under the previous contract the same bytes decoded as
// record_index = 2, content_length = 128, stream_pos = 1, is_http = true.
void verify_literal_record_metadata_bytes() {
  static constexpr unsigned char kBytes[] = {
      0x08, 0x02,              // field 1, varint: record_type = RESPONSE (2)
      0x18, 0x80, 0x20,        // field 3, varint: content_length = 4096
      0x20, 0x80, 0x01,        // field 4, varint: stream_pos = 128
      0x28, 0x01,              // field 5, varint: is_http = true
      0x30, 0x01,              // field 6, varint: http_parsed = true
  };
  const std::string wire(reinterpret_cast<const char*>(kBytes), sizeof(kBytes));

  warcv1::RecordMetadata metadata;
  require(metadata.ParseFromString(wire),
          "the server's literal RecordMetadata bytes parse");
  require(metadata.record_type() == warcv1::WARC_RECORD_TYPE_RESPONSE,
          "field 1 is record_type, and 2 is RESPONSE");
  require(metadata.content_length() == 4096,
          "field 3 is content_length");
  require(metadata.stream_pos() == 128, "field 4 is stream_pos");
  require(metadata.is_http() && metadata.http_parsed(),
          "fields 5 and 6 are the HTTP flags");
  require(metadata.unknown_fields().empty(),
          "the server's bytes leave nothing undecoded");
}

// The full record: both header blocks, the promoted HTTP fields, the record
// id, and the parsed WARC-Date.
void verify_full_record_metadata_wire() {
  const std::string warc_headers = header_block(
      "WARC/1.1",
      bytes_field(2, header_field("WARC-Type", "response")) +
          bytes_field(2, header_field("WARC-Target-URI", "https://example.com/")),
      "WARC/1.1\r\nWARC-Type: response\r\nWARC-Target-URI: https://example.com/\r\n");
  const std::string http_headers = header_block(
      "HTTP/1.1 200 OK",
      bytes_field(2, header_field("Content-Type", "text/html; charset=utf-8")) +
          bytes_field(2, header_field("ETag", "\"deadbeef\"")),
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n");
  // google.protobuf.Timestamp: seconds = 1.
  const std::string record_date = varint_field(1, 1704164645);

  const std::string wire =
      varint_field(1, 32768) +                    // record_type = UNKNOWN
      bytes_field(2, warc_headers) +              // warc_headers
      varint_field(3, 4096) +                     // content_length
      varint_field(4, 65536) +                    // stream_pos
      varint_field(5, 1) +                        // is_http
      varint_field(6, 1) +                        // http_parsed
      bytes_field(7, http_headers) +              // http_headers
      bytes_field(8, "text/html") +               // http_content_type
      bytes_field(9, "utf-8") +                   // http_charset
      bytes_field(10, "<urn:uuid:wire-test>") +   // record_id
      bytes_field(11, record_date);               // record_date

  warcv1::RecordMetadata metadata;
  require(metadata.ParseFromString(wire), "the full RecordMetadata parses");
  require(metadata.unknown_fields().empty(),
          "every field of the server's record decodes into a known field");
  require(metadata.stream_pos() == 65536 && metadata.content_length() == 4096,
          "the two counters land in their own fields");
  require(metadata.record_type() == warcv1::WARC_RECORD_TYPE_UNKNOWN,
          "record_type 32768 is UNKNOWN, the crate's bitmask value");

  const warcv1::HeaderBlock& warc = metadata.warc_headers();
  require(warc.has_status_line() && warc.status_line() == "WARC/1.1",
          "a WARC header block carries its version line as the status line");
  require(warc.fields_size() == 2 && warc.fields(1).name() == "WARC-Target-URI" &&
              warc.fields(1).value() == "https://example.com/",
          "field 2 of HeaderBlock is the ordered header list");
  require(warc.raw_block().starts_with("WARC/1.1\r\nWARC-Type: response"),
          "field 3 of HeaderBlock is raw_block");

  require(metadata.has_http_headers(), "field 7 is the embedded HTTP block");
  const warcv1::HeaderBlock& http = metadata.http_headers();
  require(http.has_status_line() && http.status_line() == "HTTP/1.1 200 OK",
          "the HTTP block keeps its status line verbatim");
  require(http.fields_size() == 2 && http.fields(1).value() == "\"deadbeef\"",
          "duplicate-tolerant header ordering survives");

  require(metadata.http_content_type() == "text/html" &&
              metadata.http_charset() == "utf-8",
          "fields 8 and 9 are the promoted content type and charset");
  require(metadata.record_id() == "<urn:uuid:wire-test>",
          "field 10 is the record id, a string and not a Timestamp");
  require(metadata.has_record_date() &&
              google::protobuf::util::TimeUtil::ToString(metadata.record_date()) ==
                  "2024-01-02T03:04:05Z",
          "field 11 is the parsed WARC-Date");
}

// PayloadChunk and RecordEnd lost record_index at 1, which pulled every later
// field back by one.
void verify_payload_chunk_and_record_end_wire() {
  const std::string chunk_wire = varint_field(1, 64) +       // offset
                                 bytes_field(2, "payload");  // data
  warcv1::PayloadChunk chunk;
  require(chunk.ParseFromString(chunk_wire), "the PayloadChunk bytes parse");
  require(chunk.offset() == 64 && chunk.data() == "payload",
          "payload bytes reach data at field 2");
  require(chunk.unknown_fields().empty(), "no payload field is dropped");

  const std::string end_wire = varint_field(1, 200);  // payload_length
  warcv1::RecordEnd end;
  require(end.ParseFromString(end_wire), "the RecordEnd bytes parse");
  require(end.payload_length() == 200 && end.unknown_fields().empty(),
          "field 1 is payload_length, and RecordEnd carries nothing else");
}

// The response envelope: a record_start arm carrying the metadata message,
// and the batch arm (5) carrying several events in stream order.
void verify_response_envelope_wire() {
  const std::string metadata = varint_field(1, 2) + varint_field(4, 7);
  const std::string record_start = bytes_field(1, bytes_field(1, metadata));
  const std::string payload = bytes_field(2, varint_field(1, 0) + bytes_field(2, "abc"));
  const std::string record_end = bytes_field(3, varint_field(1, 3));

  warcv1::ParseWarcResponse single;
  require(single.ParseFromString(record_start), "a lone record_start parses");
  require(single.kind_case() == warcv1::ParseWarcResponse::kRecordStart &&
              single.record_start().metadata().record_type() ==
                  warcv1::WARC_RECORD_TYPE_RESPONSE &&
              single.record_start().metadata().stream_pos() == 7,
          "arm 1 of the oneof is record_start, with its metadata nested");

  // RecordBatch.items = 1, each item a whole ParseWarcResponse.
  const std::string batch =
      bytes_field(1, record_start) + bytes_field(1, payload) + bytes_field(1, record_end);
  warcv1::ParseWarcResponse batched;
  require(batched.ParseFromString(bytes_field(5, batch)), "a batch message parses");
  require(batched.kind_case() == warcv1::ParseWarcResponse::kBatch &&
              batched.batch().items_size() == 3,
          "arm 5 is the batch, holding every event it was sent");
  require(batched.batch().items(0).has_record_start() &&
              batched.batch().items(1).payload_chunk().data() == "abc" &&
              batched.batch().items(2).record_end().payload_length() == 3,
          "batched events keep their stream order and their own arms");
}

// The shape checks that catch a re-vendoring of the wrong file even before a
// byte is decoded: the numbers and the fields the server's contract does and
// does not have.
void verify_contract_shape() {
  const auto* metadata = warcv1::RecordMetadata::descriptor();
  require(metadata->FindFieldByName("record_index") == nullptr,
          "RecordMetadata has no record_index in 1.1.0");
  require(metadata->FindFieldByName("record_type")->number() == 1 &&
              metadata->FindFieldByName("record_date")->number() == 11,
          "RecordMetadata runs record_type = 1 through record_date = 11");
  const auto* block = warcv1::HeaderBlock::descriptor();
  require(block->FindFieldByName("encoding") == nullptr &&
              block->FindFieldByName("raw_block")->number() == 3,
          "HeaderBlock has no encoding and raw_block is field 3");
  require(warcv1::WarcRecordType_descriptor()
                  ->FindValueByName("WARC_RECORD_TYPE_UNKNOWN")
                  ->number() == 32768,
          "WARC_RECORD_TYPE_UNKNOWN is the crate's bitmask 32768");
  require(warcv1::RecordEnd::descriptor()->field_count() == 1,
          "RecordEnd carries payload_length alone; digest verdicts are gone");

  const auto* config = warcv1::ParseWarcConfig::descriptor();
  require(config->FindFieldByName("parse_http")->has_presence(),
          "parse_http is optional: unset means true");
  require(config->FindFieldByName("include_payload")->number() == 13 &&
              config->FindFieldByName("include_headers")->number() == 14 &&
              config->FindFieldByName("response_batch_size")->number() == 15 &&
              config->FindFieldByName("archive_path")->number() == 16,
          "the 1.1.0 config knobs sit at 13 through 16");
  require(warcv1::ParseWarcResponse::descriptor()->FindFieldByName("batch")->number() == 5,
          "the response stream has its batch arm at 5");
  const auto* service =
      warcv1::ParseWarcRequest::descriptor()->file()->FindServiceByName("WarcService");
  require(service != nullptr && service->method_count() == 2 &&
              service->FindMethodByName("GetServiceInfo") == nullptr,
          "1.1.0 serves ParseWarc and ParseArchive only");
}

}  // namespace

int main() {
  return grparse_test::run_test_main({.on_failure = "FAILED", .on_success = "warc stub wire test passed"}, {
      verify_literal_record_metadata_bytes,
      verify_full_record_metadata_wire,
      verify_payload_chunk_and_record_end_wire,
      verify_response_envelope_wire,
      verify_contract_shape,
  });
}
