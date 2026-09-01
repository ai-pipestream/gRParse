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

// fastwarc.v1.HeaderBlock: status_line = 1, fields = 2, encoding = 3,
// raw_block = 4. The gRParse copy had raw_block at 3 and no encoding at all.
std::string header_block(const std::string& status_line,
                         const std::string& fields, uint64_t encoding,
                         const std::string& raw_block) {
  std::string out;
  if (!status_line.empty()) out += bytes_field(1, status_line);
  out += fields;
  out += varint_field(3, encoding);
  out += bytes_field(4, raw_block);
  return out;
}

// ---- the tests -------------------------------------------------------------

// The un-foolable core: a RecordMetadata written as literal bytes, every one
// of them named. Under the stale stubs the same bytes decoded as record_type
// = CONVERSION, stream_pos = 4096, is_http = true, and nothing else at all.
void verify_literal_record_metadata_bytes() {
  static constexpr unsigned char kBytes[] = {
      0x08, 0x07,              // field 1, varint: record_index = 7
      0x10, 0x02,              // field 2, varint: record_type = RESPONSE (2)
      0x20, 0x80, 0x20,        // field 4, varint: content_length = 4096
      0x28, 0x80, 0x01,        // field 5, varint: stream_pos = 128
      0x30, 0x01,              // field 6, varint: is_http = true
      0x38, 0x01,              // field 7, varint: http_parsed = true
  };
  const std::string wire(reinterpret_cast<const char*>(kBytes), sizeof(kBytes));

  warcv1::RecordMetadata metadata;
  require(metadata.ParseFromString(wire),
          "the server's literal RecordMetadata bytes parse");
  require(metadata.record_index() == 7,
          "field 1 is record_index, not record_type");
  require(metadata.record_type() == warcv1::WARC_RECORD_TYPE_RESPONSE,
          "field 2 is record_type, and 2 is RESPONSE");
  require(metadata.content_length() == 4096,
          "field 4 is content_length, not stream_pos");
  require(metadata.stream_pos() == 128, "field 5 is stream_pos, not is_http");
  require(metadata.is_http() && metadata.http_parsed(),
          "fields 6 and 7 are the HTTP flags");
  require(metadata.unknown_fields().empty(),
          "the server's bytes leave nothing undecoded");
}

// The full record: both lossless header blocks, the promoted HTTP fields, the
// record id, and the parsed WARC-Date.
void verify_full_record_metadata_wire() {
  const std::string warc_headers = header_block(
      /*status_line=*/"",
      bytes_field(2, header_field("WARC-Type", "response")) +
          bytes_field(2, header_field("WARC-Target-URI", "https://example.com/")),
      /*encoding=*/1,  // HEADER_ENCODING_UNICODE
      "WARC-Type: response\r\nWARC-Target-URI: https://example.com/\r\n");
  const std::string http_headers = header_block(
      "HTTP/1.1 200 OK",
      bytes_field(2, header_field("Content-Type", "text/html; charset=utf-8")) +
          bytes_field(2, header_field("ETag", "\"deadbeef\"")),
      /*encoding=*/2,  // HEADER_ENCODING_LATIN1
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n");
  // google.protobuf.Timestamp: seconds = 1, nanos = 2.
  const std::string record_date = varint_field(1, 1704164645);

  const std::string wire =
      varint_field(1, 12) +                       // record_index
      varint_field(2, 9) +                        // record_type = UNKNOWN
      bytes_field(3, warc_headers) +              // warc_headers
      varint_field(4, 4096) +                     // content_length
      varint_field(5, 65536) +                    // stream_pos
      varint_field(6, 1) +                        // is_http
      varint_field(7, 1) +                        // http_parsed
      bytes_field(8, http_headers) +              // http_headers
      bytes_field(9, "text/html") +               // http_content_type
      bytes_field(10, "utf-8") +                  // http_charset
      bytes_field(11, "<urn:uuid:wire-test>") +   // record_id
      bytes_field(12, record_date);               // record_date

  warcv1::RecordMetadata metadata;
  require(metadata.ParseFromString(wire), "the full RecordMetadata parses");
  require(metadata.unknown_fields().empty(),
          "every field of the server's record decodes into a known field");
  require(metadata.record_index() == 12 && metadata.stream_pos() == 65536 &&
              metadata.content_length() == 4096,
          "the three counters land in their own fields");
  require(metadata.record_type() == warcv1::WARC_RECORD_TYPE_UNKNOWN,
          "record_type 9 is UNKNOWN, the server's value, not 32768");

  const warcv1::HeaderBlock& warc = metadata.warc_headers();
  require(!warc.has_status_line(), "a WARC header block carries no status line");
  require(warc.fields_size() == 2 && warc.fields(1).name() == "WARC-Target-URI" &&
              warc.fields(1).value() == "https://example.com/",
          "field 2 of HeaderBlock is the ordered header list");
  require(warc.encoding() == warcv1::HEADER_ENCODING_UNICODE,
          "field 3 of HeaderBlock is the encoding verdict");
  require(warc.raw_block().starts_with("WARC-Type: response"),
          "field 4 of HeaderBlock is raw_block, which the stale copy had at 3");

  require(metadata.has_http_headers(), "field 8 is the embedded HTTP block");
  const warcv1::HeaderBlock& http = metadata.http_headers();
  require(http.has_status_line() && http.status_line() == "HTTP/1.1 200 OK",
          "the HTTP block keeps its status line verbatim");
  require(http.encoding() == warcv1::HEADER_ENCODING_LATIN1,
          "HTTP header blocks decode as Latin-1, as the server reports them");
  require(http.fields_size() == 2 && http.fields(1).value() == "\"deadbeef\"",
          "duplicate-tolerant header ordering survives");

  require(metadata.http_content_type() == "text/html" &&
              metadata.http_charset() == "utf-8",
          "fields 9 and 10 are the promoted content type and charset");
  require(metadata.record_id() == "<urn:uuid:wire-test>",
          "field 11 is the record id, a string and not a Timestamp");
  require(metadata.has_record_date() &&
              google::protobuf::util::TimeUtil::ToString(metadata.record_date()) ==
                  "2024-01-02T03:04:05Z",
          "field 12 is the parsed WARC-Date");
}

// PayloadChunk and RecordEnd both gained record_index at 1, which pushed every
// later field along. Under the stale stubs a payload chunk decoded with empty
// data: its bytes met a varint field and were dropped.
void verify_payload_chunk_and_record_end_wire() {
  const std::string chunk_wire = varint_field(1, 5) +      // record_index
                                 varint_field(2, 64) +     // offset
                                 bytes_field(3, "payload");  // data
  warcv1::PayloadChunk chunk;
  require(chunk.ParseFromString(chunk_wire), "the PayloadChunk bytes parse");
  require(chunk.record_index() == 5 && chunk.offset() == 64 &&
              chunk.data() == "payload",
          "payload bytes reach data, correlated by record_index");
  require(chunk.unknown_fields().empty(), "no payload field is dropped");

  const std::string end_wire = varint_field(1, 5) +    // record_index
                               varint_field(2, 200) +  // payload_length
                               varint_field(3, 2) +    // block_digest_status
                               varint_field(4, 3) +    // payload_digest_status
                               bytes_field(5, "sha1 mismatch");  // digest_detail
  warcv1::RecordEnd end;
  require(end.ParseFromString(end_wire), "the RecordEnd bytes parse");
  require(end.record_index() == 5 && end.payload_length() == 200,
          "field 2 is payload_length, not the record index");
  require(end.block_digest_status() == warcv1::DIGEST_STATUS_VALID &&
              end.payload_digest_status() == warcv1::DIGEST_STATUS_MISMATCH &&
              end.digest_detail() == "sha1 mismatch",
          "the digest verdicts the server computes decode into their fields");
}

// The response envelope: a record_start arm carrying the metadata message.
void verify_response_envelope_wire() {
  const std::string metadata = varint_field(1, 3) + varint_field(2, 2);
  const std::string record_start = bytes_field(1, metadata);
  const std::string wire = bytes_field(1, record_start);

  warcv1::ParseWarcResponse response;
  require(response.ParseFromString(wire), "the ParseWarcResponse bytes parse");
  require(response.kind_case() == warcv1::ParseWarcResponse::kRecordStart,
          "arm 1 of the oneof is record_start");
  require(response.record_start().metadata().record_index() == 3 &&
              response.record_start().metadata().record_type() ==
                  warcv1::WARC_RECORD_TYPE_RESPONSE,
          "the nested metadata decodes through the envelope");
}

// The shape checks that catch a re-vendoring of the wrong file even before a
// byte is decoded: the numbers and the fields the server's contract does and
// does not have.
void verify_contract_shape() {
  const auto* metadata = warcv1::RecordMetadata::descriptor();
  require(metadata->FindFieldByName("record_index")->number() == 1,
          "RecordMetadata.record_index is field 1");
  require(metadata->FindFieldByName("record_date")->number() == 12,
          "RecordMetadata.record_date is field 12");
  const auto* block = warcv1::HeaderBlock::descriptor();
  require(block->FindFieldByName("encoding")->number() == 3 &&
              block->FindFieldByName("raw_block")->number() == 4,
          "HeaderBlock gained encoding at 3 and moved raw_block to 4");
  require(warcv1::WarcRecordType_descriptor()
                  ->FindValueByName("WARC_RECORD_TYPE_UNKNOWN")
                  ->number() == 9,
          "WARC_RECORD_TYPE_UNKNOWN is 9, not the crate's bitmask 32768");

  const auto* config = warcv1::ParseWarcConfig::descriptor();
  require(!config->FindFieldByName("parse_http")->has_presence(),
          "parse_http is a plain bool: the server defaults it to false");
  for (const char* gone :
       {"include_payload", "include_headers", "response_batch_size", "archive_path"}) {
    require(config->FindFieldByName(gone) == nullptr,
            std::string("ParseWarcConfig.") + gone +
                " is not in the shipping contract");
  }
  require(warcv1::ParseWarcResponse::descriptor()->FindFieldByName("batch") == nullptr,
          "the shipping response stream has no batch arm");
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
