#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../src/targets/bundle.h"
#include "../src/targets/s3_client.h"
#include "../src/targets/s3_uploader.h"
#include "../src/targets/sha256.h"
#include "../src/targets/sigv4.h"
#include "../src/targets/target_step.h"
#include "../src/targets/zip_writer.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/base64.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace targets = grparse::targets;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

// A one-pixel PNG stands in for a page render: the bundle only ever moves the
// bytes behind a data URI, so their content is irrelevant and their identity
// is not.
std::string fake_image(unsigned char seed) {
  std::string png = "\x89PNG\r\n\x1a\n";
  png.append(64, static_cast<char>(seed));
  return png;
}

std::string data_uri(const std::string& bytes) {
  return "data:image/png;base64," + grparse::encode_base64(bytes.data(), bytes.size());
}

docv1::Document sample_document(const std::string& title) {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.set_version("1.10.0");
  document.set_name("sample.pdf");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  auto* base = document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(title);
  document.mutable_body()->add_children()->set_ref("#/texts/0");

  // Two pages inserted back to front: the bundle must order them itself, and
  // a proto map hands them over in hash order.
  for (const int page_no : {2, 1}) {
    auto& page = (*document.mutable_pages())[page_no];
    page.set_page_no(page_no);
    page.mutable_size()->set_width(612);
    page.mutable_size()->set_height(792);
    page.mutable_image()->set_mimetype("image/png");
    page.mutable_image()->set_uri(data_uri(fake_image(static_cast<unsigned char>(page_no))));
  }

  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  picture->mutable_image()->set_mimetype("image/png");
  picture->mutable_image()->set_uri(data_uri(fake_image(9)));
  document.mutable_body()->add_children()->set_ref("#/pictures/0");
  return document;
}

parsev1::DocumentExports sample_exports(const docv1::Document& document) {
  parsev1::DocumentExports exports;
  exports.set_text(document.texts(0).text().base().text());
  exports.set_md("# " + document.texts(0).text().base().text());
  return exports;
}

// The response ConvertSource would hold for `document` when its target runs:
// a successful conversion carrying the document and its exports.
parsev1::ConvertDocumentResponse sample_conversion(const docv1::Document& document) {
  parsev1::ConvertDocumentResponse converted;
  converted.set_status(parsev1::CONVERSION_STATUS_SUCCESS);
  converted.mutable_document()->set_filename(document.name());
  *converted.mutable_document()->mutable_doc() = document;
  *converted.mutable_document()->mutable_exports() = sample_exports(document);
  return converted;
}

std::vector<targets::BundleFile> sample_bundle(const docv1::Document& document) {
  parsev1::TargetResult result;
  return targets::build_bundle(sample_conversion(document), &result);
}

const targets::BundleFile& member(const std::vector<targets::BundleFile>& files,
                                  const std::string& path) {
  const auto found = std::ranges::find(files, path, &targets::BundleFile::path);
  require(found != files.end(), "the bundle carries no '" + path + "'");
  return *found;
}

// The same document twice must produce the same archive, byte for byte, and a
// different document must not. Determinism is the whole contract of the
// bundle: without it an archive's digest says nothing about its contents.
void verify_bundles_are_reproducible() {
  const docv1::Document document = sample_document("the same document");
  const std::string first = targets::write_zip(sample_bundle(document));
  const std::string second = targets::write_zip(sample_bundle(document));
  require(first == second, "two bundles of one document must be byte-identical");
  require(targets::sha256_hex(first) == targets::sha256_hex(second),
          "two bundles of one document must hash equal");
  require(!first.empty() && first.starts_with("PK\x03\x04"),
          "the archive must start with a ZIP local file header");

  const docv1::Document changed = sample_document("a different document");
  const std::string other = targets::write_zip(sample_bundle(changed));
  require(targets::sha256_hex(other) != targets::sha256_hex(first),
          "a changed document must change the archive");
}

// The members the bundle is defined to carry, in the order every target
// writes them.
void verify_bundle_carries_the_canonical_file_set() {
  const docv1::Document document = sample_document("members");
  const auto files = sample_bundle(document);
  for (const auto& path : {"document.json", "document.pb", "exports/markdown.md",
                           "exports/text.txt", "manifest.json", "pages/page_0001.png",
                           "pages/page_0002.png", "pictures/pic_0001.png", "response.pb"}) {
    member(files, path);
  }
  require(files.size() == 9, "the bundle carries exactly its own members, not more");
  require(std::ranges::is_sorted(files, {}, &targets::BundleFile::path),
          "bundle members must be sorted by path");
  require(member(files, "pages/page_0001.png").bytes == fake_image(1),
          "page one must carry the bytes behind its own data URI");
  require(member(files, "pictures/pic_0001.png").bytes == fake_image(9),
          "the picture must carry the bytes behind its own data URI");
  require(member(files, "exports/text.txt").bytes == "members",
          "the text export must land verbatim");
  // An unrequested export contributes nothing rather than an empty file.
  require(std::ranges::none_of(files,
                               [](const targets::BundleFile& file) {
                                 return file.path == "exports/html.html";
                               }),
          "an export the request never asked for must not appear");
}

// A very small scanner over the manifest: it is written by hand, in one fixed
// shape, so reading it back needs no JSON parser.
std::string field_after(std::string_view text, size_t* cursor, std::string_view key,
                        bool quoted) {
  const size_t start = text.find(key, *cursor);
  require(start != std::string_view::npos, "the manifest is missing a '" + std::string(key) + "'");
  const size_t value = start + key.size();
  const size_t end = quoted ? text.find('"', value) : text.find_first_of(",}", value);
  require(end != std::string_view::npos, "the manifest ends inside a value");
  *cursor = end;
  return std::string(text.substr(value, end - value));
}

// Every member's digest and size, as the manifest states them, must be the
// member's own.
void verify_manifest_describes_every_member() {
  const docv1::Document document = sample_document("manifest");
  const auto files = sample_bundle(document);
  const std::string manifest = member(files, "manifest.json").bytes;

  require(manifest.contains("\"generator\":\"grparse/"),
          "the manifest names its generator: " + manifest);
  require(manifest.contains("\"schema_version\":1"), "the manifest names its schema version");
  require(!manifest.contains("manifest.json"), "the manifest must not describe itself");

  size_t cursor = 0;
  size_t described = 0;
  for (const auto& file : files) {
    if (file.path == "manifest.json") continue;
    const std::string path = field_after(manifest, &cursor, "\"path\":\"", true);
    const std::string digest = field_after(manifest, &cursor, "\"sha256\":\"", true);
    const std::string size = field_after(manifest, &cursor, "\"size_bytes\":", false);
    require(path == file.path, "the manifest lists members in bundle order: " + path);
    require(digest == targets::sha256_hex(file.bytes),
            "the manifest's digest for '" + path + "' must be the member's own");
    require(size == std::to_string(file.bytes.size()),
            "the manifest's size for '" + path + "' must be the member's own");
    ++described;
  }
  require(described + 1 == files.size(), "the manifest describes every other member");
}

// A picture of the document carrying `uri`, with `mimetype` as the ImageRef's
// own declaration.
void add_picture(docv1::Document* document, const std::string& mimetype, const std::string& uri) {
  auto* picture = document->add_pictures();
  picture->set_self_ref("#/pictures/" + std::to_string(document->pictures_size() - 1));
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  picture->mutable_image()->set_mimetype(mimetype);
  picture->mutable_image()->set_uri(uri);
}

std::string data_uri(const std::string& mimetype, const std::string& bytes) {
  return "data:" + mimetype + ";base64," + grparse::encode_base64(bytes.data(), bytes.size());
}

bool has_member(const std::vector<targets::BundleFile>& files, const std::string& path) {
  return std::ranges::find(files, path, &targets::BundleFile::path) != files.end();
}

// The bundle's own account of the conversion, read back the way a reader of
// the bucket reads it.
parsev1::ConvertDocumentResponse bundled_response(const std::vector<targets::BundleFile>& files) {
  parsev1::ConvertDocumentResponse response;
  require(response.ParseFromString(member(files, "response.pb").bytes),
          "response.pb must parse as a ConvertDocumentResponse");
  return response;
}

// A reader of the bucket has only the bundle. A conversion that lost a leg
// must look partial there too, and the chunks, findings, read quality and
// offset table the request paid for must be in it, as typed messages.
void verify_the_bundle_carries_the_conversions_status_and_outputs() {
  const docv1::Document document = sample_document("partial");
  parsev1::ConvertDocumentResponse converted = sample_conversion(document);
  converted.set_status(parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  auto* error = converted.add_errors();
  error->set_component_type(parsev1::COMPONENT_TYPE_PIPELINE);
  error->set_module_name("enrich");
  error->set_error_message("the enrich leg was unavailable");
  error->set_category(parsev1::FAILURE_CATEGORY_TIMEOUT);
  auto* chunk = converted.add_chunks();
  chunk->set_filename("sample.pdf");
  chunk->set_text("partial");
  chunk->add_doc_items("#/texts/0");
  auto* finding = converted.add_structure_findings();
  finding->set_rule(parsev1::STRUCTURE_RULE_DEPRECATED_FURNITURE_TREE);
  finding->set_self_ref("#/furniture");
  converted.mutable_confidence()->set_mean_score(0.5);
  auto* offset = converted.add_text_offsets();
  offset->set_self_ref("#/texts/0");
  offset->set_utf_end(7);
  converted.set_processing_time(1.25);
  (*converted.mutable_timings())["total"] = 1.25;
  (*converted.mutable_profiling())["total"].set_count(1);

  parsev1::TargetResult result;
  const auto files = targets::build_bundle(converted, &result);
  const parsev1::ConvertDocumentResponse response = bundled_response(files);
  require_equal(static_cast<int>(response.status()),
                static_cast<int>(parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS),
                "the bundle states the conversion's status");
  require_equal(response.errors_size(), 1, "the bundle carries the conversion's errors");
  require_equal(response.errors(0).module_name(), "enrich", "the error names its module");
  require_equal(static_cast<int>(response.errors(0).category()),
                static_cast<int>(parsev1::FAILURE_CATEGORY_TIMEOUT),
                "the error keeps its typed category");
  require_equal(response.document().filename(), "sample.pdf", "the bundle names its source");
  require_equal(response.chunks_size(), 1, "the bundle carries the requested chunks");
  require_equal(response.chunks(0).text(), "partial", "a chunk keeps its text");
  require_equal(response.structure_findings_size(), 1, "the bundle carries the findings");
  require_equal(response.structure_findings(0).self_ref(), "#/furniture",
                "a finding keeps the item it is about");
  require(response.has_confidence() && response.confidence().mean_score() == 0.5,
          "the bundle carries the read quality");
  require_equal(response.text_offsets_size(), 1, "the bundle carries the offset table");
  require_equal(response.text_offsets(0).utf_end(), uint64_t{7}, "an offset row keeps its span");

  // What other members already hold is not repeated, and the clock stays
  // out: a timing in the bundle would make two runs over one document
  // disagree byte for byte.
  require(!response.document().has_doc(), "the document is document.pb, not a second copy");
  require(!response.document().has_exports(), "the exports are their own members");
  require(response.processing_time() == 0 && response.timings().empty() &&
              response.profiling().empty(),
          "the bundle carries no timing");
  converted.set_processing_time(7.5);
  (*converted.mutable_timings())["total"] = 7.5;
  parsev1::TargetResult again;
  require(targets::write_zip(targets::build_bundle(converted, &again)) ==
              targets::write_zip(files),
          "a slower run of the same conversion must produce the same archive");
  require(member(files, "manifest.json").bytes.contains("\"path\":\"response.pb\""),
          "the manifest describes response.pb like every other member");
}

// An image the document only refers to, or whose payload does not decode,
// has no bytes to write. The bundle must say which images it left out, in
// the target result the caller reads and in the bundle itself.
void verify_an_image_the_bundle_cannot_write_is_reported() {
  docv1::Document document = sample_document("left out");
  (*document.mutable_pages())[2].mutable_image()->set_uri("https://example.test/pages/2.png");
  add_picture(&document, "image/png", "/OEBPS/images/cover.png");
  add_picture(&document, "image/png", "data:image/png;base64,@@not base64@@");

  parsev1::TargetResult result;
  const auto files = targets::build_bundle(sample_conversion(document), &result);
  require(has_member(files, "pages/page_0001.png") && has_member(files, "pictures/pic_0001.png"),
          "the images the document embeds are still written");
  require(std::ranges::none_of(files,
                               [](const targets::BundleFile& file) {
                                 return file.path.starts_with("pages/page_0002") ||
                                        file.path.starts_with("pictures/pic_0002") ||
                                        file.path.starts_with("pictures/pic_0003");
                               }),
          "an image with no bytes contributes no member");

  require_equal(result.image_issues_size(), 3, "every image left out is reported");
  const auto& page = result.image_issues(0);
  require(page.has_page_no() && page.page_no() == 2, "pages are reported first, by page number");
  require_equal(static_cast<int>(page.reason()),
                static_cast<int>(parsev1::BUNDLE_IMAGE_ISSUE_REASON_NOT_EMBEDDED),
                "a referenced page image is reported as not embedded");
  const auto& referenced = result.image_issues(1);
  require(referenced.has_picture_index() && referenced.picture_index() == 1,
          "a picture is reported by its index in the document");
  require_equal(static_cast<int>(referenced.reason()),
                static_cast<int>(parsev1::BUNDLE_IMAGE_ISSUE_REASON_NOT_EMBEDDED),
                "a referenced picture is reported as not embedded");
  require_equal(referenced.declared_mimetype(), "image/png",
                "the report keeps the type the document declares");
  require(!referenced.has_path(), "an image left out names no member");
  const auto& malformed = result.image_issues(2);
  require(malformed.has_picture_index() && malformed.picture_index() == 2,
          "the malformed picture is reported by its index");
  require_equal(static_cast<int>(malformed.reason()),
                static_cast<int>(parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNDECODABLE),
                "a payload that is not base64 is reported as undecodable");

  // The bucket's reader gets the same list as the RPC's caller.
  const parsev1::ConvertDocumentResponse response = bundled_response(files);
  require_equal(response.target_result().image_issues_size(), 3,
                "response.pb lists the images the bundle left out");
  require_equal(response.target_result().SerializeAsString(), result.SerializeAsString(),
                "response.pb and the target result report the same images");

  // The list is one bundle's: building again into the same result restates
  // it instead of doubling it.
  targets::build_bundle(sample_conversion(document), &result);
  require_equal(result.image_issues_size(), 3, "a second bundle replaces the first one's list");

  // The same report through the step a request actually runs.
  parsev1::Target target;
  target.mutable_zip();
  parsev1::TargetResult delivered;
  require(targets::deliver(target, sample_conversion(document), &delivered).ok(),
          "a zip target with an image left out still delivers");
  require_equal(delivered.image_issues_size(), 3, "the delivery reports the images left out");
}

// A member's extension is a claim about its bytes. It comes from the bytes'
// own signature, else from the media type the document declares; when
// neither names a type the bundle knows, the member says so instead of
// calling itself a PNG.
void verify_an_image_member_is_never_named_for_a_type_it_is_not() {
  docv1::Document document = sample_document("types");
  document.clear_pictures();
  const std::string gif = "GIF89a" + std::string(32, '\x01');
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
  const std::string jpeg = "\xFF\xD8\xFF\xE0" + std::string(32, '\x02');
  const std::string metafile = std::string("\x01\x00\x00\x00", 4) + std::string(32, '\x03');
  const std::string unknown = std::string("\x00\x07\x00\x07", 4) + std::string(32, '\x04');
  add_picture(&document, "image/gif", data_uri("image/gif", gif));
  add_picture(&document, "image/svg+xml", data_uri("image/svg+xml", svg));
  // The bytes outrank a declaration that contradicts them.
  add_picture(&document, "image/png", data_uri("image/png", jpeg));
  // No signature the bundle reads: the data URI's media type decides, with
  // its case and parameters set aside.
  add_picture(&document, "", data_uri("Image/EMF;name=a.emf", metafile));
  // The data URI declares nothing useful: the ImageRef's own type decides.
  add_picture(&document, "image/x-wmf", data_uri("application/octet-stream", metafile));
  add_picture(&document, "image/x-made-up", data_uri("image/x-made-up", unknown));

  parsev1::TargetResult result;
  const auto files = targets::build_bundle(sample_conversion(document), &result);
  const struct {
    std::string path;
    const std::string* bytes;
  } expected[] = {
      {"pictures/pic_0001.gif", &gif},  {"pictures/pic_0002.svg", &svg},
      {"pictures/pic_0003.jpg", &jpeg}, {"pictures/pic_0004.emf", &metafile},
      {"pictures/pic_0005.wmf", &metafile}, {"pictures/pic_0006.bin", &unknown},
  };
  for (const auto& one : expected) {
    require(has_member(files, one.path), "the bundle must carry '" + one.path + "'");
    require(member(files, one.path).bytes == *one.bytes,
            "'" + one.path + "' must carry its picture's own bytes");
  }
  require(std::ranges::none_of(files,
                               [](const targets::BundleFile& file) {
                                 return file.path.starts_with("pictures/") &&
                                        file.path.ends_with(".png");
                               }),
          "no picture here is a PNG, so none may be named one");

  require_equal(result.image_issues_size(), 1, "only the image of unknown type is reported");
  const auto& issue = result.image_issues(0);
  require_equal(static_cast<int>(issue.reason()),
                static_cast<int>(parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNKNOWN_TYPE),
                "an image neither its bytes nor its declaration can name is reported");
  require(issue.has_picture_index() && issue.picture_index() == 5,
          "the report names the picture");
  require_equal(issue.declared_mimetype(), "image/x-made-up",
                "the report keeps the type the document declares");
  require_equal(issue.path(), "pictures/pic_0006.bin",
                "the report names the member the bytes were written to");
}

// The digests are the bundle's own identity, so they are held to the
// published vectors rather than to themselves.
void verify_sha256_matches_the_published_vectors() {
  require(targets::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of the empty input");
  require(targets::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of 'abc' (FIPS 180-4 example)");
  require(targets::sha256_hex(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 of the two-block FIPS 180-4 example");
  require(targets::sha256_hex(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 of a million 'a' characters");
}

// The AWS "get-vanilla" signature test vector, end to end: canonical request,
// string to sign, and Authorization header.
void verify_sigv4_matches_the_published_vector() {
  targets::SigV4Request request;
  request.method = "GET";
  request.canonical_uri = "/";
  request.headers = {{"Host", "example.amazonaws.com"},
                     {"X-Amz-Date", "20150830T123600Z"}};
  request.payload_sha256_hex = targets::sha256_hex("");
  request.region = "us-east-1";
  request.service = "service";
  request.amz_date = "20150830T123600Z";

  const std::string canonical = targets::canonical_request(request);
  require(canonical ==
              "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\n"
              "host;x-amz-date\n"
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "canonical request:\n" + canonical);
  require(targets::sha256_hex(canonical) ==
              "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
          "the canonical request must hash to the published value");
  require(targets::string_to_sign(request) ==
              "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
              "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
          "string to sign:\n" + targets::string_to_sign(request));
  require(targets::to_hex(targets::signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
                                               "20150830", "us-east-1", "service")) ==
              "938127b5336810ddb6a5d6af445fcac9e371f9ed418ed386b022aed82901be75",
          "the derived signing key must match the published value");

  const std::string authorization = targets::authorization_header(
      "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", request);
  require(authorization ==
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
              "SignedHeaders=host;x-amz-date, "
              "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
          "authorization: " + authorization);
}

// A session token is an ordinary signed header. The inputs are the AWS SigV4
// test suite's "post-sts-header-after" request; the expected canonical request
// digest and signature were cross-checked with an independent Python
// (hmac/hashlib) implementation of the specification.
void verify_sigv4_signs_the_session_token() {
  const std::string token =
      "AQoDYXdzEPT//////////wEXAMPLEtc764bNrC9SAPBSM22wDOk4x4HIZ8j4FZTwdQWLWsKWHGBuFqwAeMicRX"
      "mxfpSPfIeoIYRqTflfKD8YUuwthAx7mSEI/qkPpKPi/kMcGdQrmGdeehM4IC1NtBmUpp2wUE8phUZampKsburE"
      "Dy0KPkyQDYwT7WZ0wq5VSXDvp75YU9HFvlRd8Tx6q6fE8YQcHNVXAkiY9q6d+xo0rKwT38xVqr7ZD0u0iPPkUL"
      "64lIZbqBAz+scqKmlzm8FDrypNC9Yjc8fPOLn9FX9KSYvKTr4rvx3iSIlTJabIQwj2ICCR/oLxBA==";
  const std::string secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  targets::SigV4Request request;
  request.method = "POST";
  request.canonical_uri = "/";
  // The order s3_client.cpp builds them in: the token is appended last, and
  // the signer must still sort it into place.
  request.headers = {{"host", "example.amazonaws.com"},
                     {"x-amz-date", "20150830T123600Z"},
                     {"x-amz-security-token", token}};
  request.payload_sha256_hex = targets::sha256_hex("");
  request.region = "us-east-1";
  request.service = "service";
  request.amz_date = "20150830T123600Z";

  const std::string canonical = targets::canonical_request(request);
  require(canonical.contains("\nx-amz-security-token:" + token + "\n"),
          "the token is a canonical header:\n" + canonical);
  require(targets::sha256_hex(canonical) ==
              "c237e1b440d4c63c32ca95b5b99481081cb7b13c7e40434868e71567c1a882f6",
          "the canonical request must hash to the expected value");

  const std::string authorization =
      targets::authorization_header("AKIDEXAMPLE", secret, request);
  require(authorization.contains("SignedHeaders=host;x-amz-date;x-amz-security-token,"),
          "the token is among the signed headers: " + authorization);
  require(!authorization.contains(secret), "the secret never appears in the header");
  require(!authorization.contains(token), "the token value is not in the header");
  require(authorization ==
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
              "SignedHeaders=host;x-amz-date;x-amz-security-token, "
              "Signature=85d96828115b5dc0cfc3bd16ad9e210dd772bbebba041836c64533a82be05ead",
          "authorization: " + authorization);
}

void verify_region_comes_from_the_endpoint() {
  require(targets::region_for_endpoint("https://s3.eu-west-1.amazonaws.com") == "eu-west-1",
          "a regional AWS endpoint names its region");
  require(targets::region_for_endpoint("bucket.s3.us-west-2.amazonaws.com") == "us-west-2",
          "a virtual-host AWS endpoint names its region");
  require(targets::region_for_endpoint("https://s3.amazonaws.com") == "us-east-1",
          "the regionless AWS endpoint defaults");
  require(targets::region_for_endpoint("http://127.0.0.1:9000") == "us-east-1",
          "a store that encodes no region defaults");
  require(targets::region_for_endpoint("https://s3-us-west-2.amazonaws.com") == "us-west-2",
          "the legacy dash-style endpoint names its region inside the label");
  require(targets::region_for_endpoint("bucket.s3.dualstack.us-east-2.amazonaws.com") ==
              "us-east-2",
          "the dualstack qualifier is not a region");
  require(targets::region_for_endpoint("s3-fips.us-gov-west-1.amazonaws.com") == "us-gov-west-1",
          "the fips qualifier is not a region either");
  require(targets::region_for_endpoint("s3-external-1.amazonaws.com") == "us-east-1",
          "s3-external-1 is the us-east-1 default");
}

void verify_explicit_region_overrides_endpoint() {
  targets::S3Config config;
  config.endpoint = "https://s3.amazonaws.com";
  config.access_key = "AKID";
  config.secret_key = "SECRET";
  config.bucket = "bucket";
  config.region = "us-east-2";
  targets::S3Client client(config);
  require(client.signing_region() == "us-east-2",
          "an explicit S3Target.region wins over endpoint inference");
}

// A single-threaded HTTP/1.1 origin that accepts PUTs, remembers them, and
// answers with an ETag. Enough of a store to prove the client's wire shape;
// it verifies no signature, which is what the known-answer test above is for.
class FakeStore final {
 public:
  struct Request {
    std::string method;
    std::string path;
    std::string authorization;
    std::string content_sha;
    std::string session_token;
    std::string body;
    bool accepted = true;
  };

  // `refuse_suffix`, when set, answers 403 to every path ending in it.
  explicit FakeStore(std::string refuse_suffix = {}) : refuse_suffix_(std::move(refuse_suffix)) {

    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener_ >= 0, "fake store could not open a socket");
    const int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "fake store could not bind");
    require(::listen(listener_, 64) == 0, "fake store could not listen");
    socklen_t length = sizeof(address);
    require(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "fake store could not name its port");
    port_ = ::ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
  }

  ~FakeStore() {
    stopping_.store(true);
    worker_.join();
    ::close(listener_);
  }

  FakeStore(const FakeStore&) = delete;
  FakeStore& operator=(const FakeStore&) = delete;

  int port() const { return port_; }

  std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::vector<Request> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }

  // The ETag this store answers a body with: derived from the body, so a test
  // can predict it without the store telling it.
  static std::string etag_for(const std::string& body) {
    return "\"" + targets::sha256_hex(body).substr(0, 32) + "\"";
  }

 private:
  void serve() {
    while (!stopping_.load()) {
      pollfd waiting{listener_, POLLIN, 0};
      if (::poll(&waiting, 1, 50) <= 0) continue;
      const int connection = ::accept(listener_, nullptr, nullptr);
      if (connection < 0) continue;
      handle(connection);
      ::close(connection);
    }
  }

  // Reads one request and answers it. Only what an object PUT sends: a
  // request line, headers, and a Content-Length body.
  void handle(int connection) {
    std::string buffer;
    size_t header_end = std::string::npos;
    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
      if (!read_more(connection, &buffer)) return;
    }
    const std::string head = buffer.substr(0, header_end);
    Request request;
    const size_t line_end = head.find("\r\n");
    const std::string line = head.substr(0, line_end);
    const size_t method_end = line.find(' ');
    const size_t path_end = line.find(' ', method_end + 1);
    request.method = line.substr(0, method_end);
    request.path = line.substr(method_end + 1, path_end - method_end - 1);
    request.authorization = header_value(head, "authorization");
    request.content_sha = header_value(head, "x-amz-content-sha256");
    request.session_token = header_value(head, "x-amz-security-token");
    const std::string length = header_value(head, "content-length");
    const size_t expected = length.empty() ? 0 : std::stoul(length);

    std::string body = buffer.substr(header_end + 4);
    while (body.size() < expected) {
      std::string more;
      if (!read_more(connection, &more)) return;
      body += more;
    }
    request.body = body.substr(0, expected);

    request.accepted = refuse_suffix_.empty() || !request.path.ends_with(refuse_suffix_);
    const std::string response =
        request.accepted
            ? "HTTP/1.1 200 OK\r\nETag: " + etag_for(request.body) +
                  "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : std::string("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
    // Recorded before the answer goes out: a client that has its response is
    // free to finish, and the test reads this list the moment the last one
    // does.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      received_.push_back(std::move(request));
    }
    ::send(connection, response.data(), response.size(), MSG_NOSIGNAL);
  }

  static bool read_more(int connection, std::string* buffer) {
    char chunk[4096];
    const ssize_t read = ::recv(connection, chunk, sizeof(chunk), 0);
    if (read <= 0) return false;
    buffer->append(chunk, static_cast<size_t>(read));
    return true;
  }

  static std::string header_value(const std::string& head, std::string_view name) {
    std::string lowered;
    lowered.reserve(head.size());
    for (const char character : head) {
      lowered.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(character))));
    }
    const std::string needle = "\r\n" + std::string(name) + ":";
    const size_t start = lowered.find(needle);
    if (start == std::string::npos) return {};
    const size_t value = start + needle.size();
    // The head was cut at the blank line, so its last header has no trailing
    // separator to find.
    const size_t end = std::min(head.find("\r\n", value), head.size());
    std::string_view found(head.data() + value, end - value);
    while (!found.empty() && (found.front() == ' ' || found.front() == '\t')) {
      found.remove_prefix(1);
    }
    return std::string(found);
  }

  std::string refuse_suffix_;
  mutable std::mutex mutex_;
  std::vector<Request> received_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  int listener_ = -1;
  int port_ = 0;
};

// Every bundle member lands as its own object, under the configured prefix,
// with the ETag the store returned.
void verify_uploads_land_as_objects() {
  const docv1::Document document = sample_document("uploads");
  const auto files = sample_bundle(document);

  FakeStore store;
  targets::S3Config config;
  config.endpoint = store.endpoint();
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";
  config.key_prefix = "docs/sample/";
  config.verify_ssl = false;

  const auto objects = targets::upload_bundle(config, files);
  require(objects.size() == files.size(), "every member must become an object");
  for (size_t index = 0; index < files.size(); ++index) {
    require(objects[index].key == "docs/sample/" + files[index].path,
            "objects carry the prefix and the member path: " + objects[index].key);
    require(objects[index].size_bytes == files[index].bytes.size(),
            "objects report the member's own size");
    require(objects[index].etag == FakeStore::etag_for(files[index].bytes),
            "objects report the ETag the store answered with: " + objects[index].etag);
  }

  const auto received = store.received();
  require(received.size() == files.size(), "the store saw one request per member");
  for (const auto& request : received) {
    require(request.method == "PUT", "members are written with PUT");
    require(request.path.starts_with("/conversions/docs/sample/"),
            "the path is bucket then key: " + request.path);
    require(request.authorization.starts_with(
                "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/"),
            "the request is signature v4 signed: " + request.authorization);
    require(request.authorization.contains("/us-east-1/s3/aws4_request"),
            "the credential scope names the region and the service");
    require(request.authorization.contains(
                "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date"),
            "the signed headers are the ones the request carries");
    require(request.authorization.contains(", Signature="),
            "the header carries a signature");
    require(request.content_sha == targets::sha256_hex(request.body),
            "the payload hash header describes the body actually sent");
    // The secret is signing material and nothing else: it must never reach
    // the wire, in any header.
    require(!request.authorization.contains(config.secret_key),
            "the secret key must never appear on the wire");
  }

  const auto written = std::ranges::find(received, "/conversions/docs/sample/manifest.json",
                                         &FakeStore::Request::path);
  require(written != received.end(), "the manifest is one of the objects");
  require(written->body == member(files, "manifest.json").bytes,
          "the stored manifest is the bundle's own");
}

// A store that refuses fails the whole delivery, and the failure names the
// key without naming the credentials that signed for it.
void verify_a_refused_upload_fails_without_leaking() {
  const docv1::Document document = sample_document("refused");
  const auto files = sample_bundle(document);

  targets::S3Config config;
  // Port zero never accepts, so every member fails at connect.
  config.endpoint = "http://127.0.0.1:1";
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";

  bool refused = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const std::exception& error) {
    refused = true;
    const std::string message = error.what();
    require(!message.contains(config.secret_key),
            "a failure must not quote the secret key");
    require(!message.contains(config.access_key),
            "a failure must not quote the access key");
    require(message.contains("s3 target could not write"),
            "a failure names what it could not do: " + message);
  }
  require(refused, "an unreachable store must fail the delivery");
}

// A batch that fails partway still reports what it wrote: those objects are
// in the store whatever became of the rest.
void verify_a_partial_failure_reports_the_written_objects() {
  const docv1::Document document = sample_document("partial");
  const auto files = sample_bundle(document);

  FakeStore store("/manifest.json");
  targets::S3Config config;
  config.endpoint = store.endpoint();
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";
  config.verify_ssl = false;

  bool failed = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const targets::UploadFailure& failure) {
    failed = true;
    require(std::string(failure.what()).contains("manifest.json"),
            "the failure names the refused key: " + std::string(failure.what()));
    const auto received = store.received();
    size_t accepted = 0;
    for (const auto& request : received) accepted += request.accepted ? 1 : 0;
    require(failure.written().size() == accepted,
            "every object the store accepted is reported, and nothing else");
    for (const auto& object : failure.written()) {
      require(object.key != "manifest.json", "the refused member is not reported");
      require(std::ranges::any_of(received,
                                  [&object](const FakeStore::Request& request) {
                                    return request.accepted &&
                                           request.path == "/conversions/" + object.key;
                                  }),
              "a reported object is one the store accepted: " + object.key);
    }
  }
  require(failed, "a refused member fails the batch");
}

// A session token never crosses the wire in cleartext: an http endpoint is
// refused before any request is made.
void verify_session_token_is_refused_over_http() {
  const docv1::Document document = sample_document("session");
  const auto files = sample_bundle(document);

  FakeStore store;
  targets::S3Config config;
  config.endpoint = store.endpoint();
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";
  config.verify_ssl = false;
  config.session_token = "session-token-that-must-not-leak";

  bool refused = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const std::invalid_argument& cleartext) {
    refused = true;
    require(!std::string(cleartext.what()).contains(config.session_token),
            "the refusal must not quote the token");
  }
  require(refused, "a session token over http must be refused");
  require(store.received().empty(), "the store saw no request at all");
}

// Nor is it sent to a peer whose certificate went unchecked, https or not.
void verify_session_token_is_refused_without_verification() {
  const docv1::Document document = sample_document("unverified");
  const auto files = sample_bundle(document);

  targets::S3Config config;
  // Nothing listens here: the refusal must come before any connection.
  config.endpoint = "https://127.0.0.1:1";
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";
  config.verify_ssl = false;
  config.session_token = "session-token-that-must-not-leak";

  bool refused = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const std::invalid_argument& unverified) {
    refused = true;
    const std::string message = unverified.what();
    require(message.contains("verify_ssl"), "the refusal names the setting: " + message);
    require(!message.contains(config.session_token), "the refusal must not quote the token");
  }
  require(refused, "a session token with verify_ssl false must be refused");
}

// Sets an environment variable for one scope and puts the old value back.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name); old != nullptr) old_ = old;
    if (value == nullptr) {
      ::unsetenv(name);
    } else {
      ::setenv(name, value, 1);
    }
  }
  ~ScopedEnv() {
    if (old_.has_value()) {
      ::setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
  std::optional<std::string> old_;
};

// The server's own identity only signs for a peer whose certificate is
// checked: a caller who turns verification off is refused before any request,
// while the same target with verification on goes through.
void verify_ambient_credentials_need_verification() {
  const ScopedEnv opt_in("GRPARSE_S3_AMBIENT_CREDENTIALS", "1");
  const ScopedEnv access("AWS_ACCESS_KEY_ID", "AKIAIOSFODNN7EXAMPLE");
  const ScopedEnv secret("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  const ScopedEnv token("AWS_SESSION_TOKEN", nullptr);

  const parsev1::ConvertDocumentResponse converted =
      sample_conversion(sample_document("ambient"));
  FakeStore store;
  parsev1::Target target;
  target.mutable_s3()->set_endpoint(store.endpoint());
  target.mutable_s3()->set_bucket("conversions");

  target.mutable_s3()->set_verify_ssl(false);
  parsev1::TargetResult refused_result;
  const grpc::Status refused = targets::deliver(target, converted, &refused_result);
  require(refused.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "ambient credentials with verify_ssl false are refused: " + refused.error_message());
  require(refused.error_message().contains("verify_ssl"),
          "the refusal names the setting: " + refused.error_message());
  require(store.received().empty(), "the store saw no request at all");

  // Nor over cleartext, even to a listed endpoint with verification on:
  // the access key ID would cross the wire readable and replayable.
  const ScopedEnv listed("GRPARSE_S3_AMBIENT_ENDPOINTS", store.endpoint().c_str());
  target.mutable_s3()->clear_verify_ssl();
  parsev1::TargetResult result;
  const grpc::Status cleartext = targets::deliver(target, converted, &result);
  require(cleartext.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              cleartext.error_message().contains("not https"),
          "ambient credentials over http are refused: " + cleartext.error_message());
  require(store.received().empty(), "the store still saw no request");

  // The control: the request's own credentials still sign for the same
  // http store.
  target.mutable_s3()->set_access_key("AKIAIOSFODNN7EXAMPLE");
  target.mutable_s3()->set_secret_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  const grpc::Status delivered = targets::deliver(target, converted, &result);
  require(delivered.ok(), "request credentials still deliver over http: " +
                              delivered.error_message());
  require(!store.received().empty(), "the store saw the upload");
}

// The endpoint goes into the URL and the signed Host header: userinfo and
// control characters are refused before any request.
void verify_endpoint_hygiene() {
  const parsev1::ConvertDocumentResponse converted =
      sample_conversion(sample_document("hygiene"));
  for (const char* endpoint : {"https://user:pw@s3.example.test", "https://s3.example.test\r\nX: y",
                               "https://s3.example .test"}) {
    parsev1::Target target;
    target.mutable_s3()->set_endpoint(endpoint);
    target.mutable_s3()->set_bucket("conversions");
    target.mutable_s3()->set_access_key("AKIAIOSFODNN7EXAMPLE");
    target.mutable_s3()->set_secret_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    parsev1::TargetResult result;
    const grpc::Status refused = targets::deliver(target, converted, &result);
    require(refused.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
            std::string("an endpoint with userinfo or a control character is refused: ") +
                refused.error_message());
  }
}

void verify_incomplete_targets_are_rejected() {
  const docv1::Document document = sample_document("incomplete");
  const auto files = sample_bundle(document);
  targets::S3Config config;
  config.endpoint = "http://127.0.0.1:1";
  config.bucket = "conversions";
  bool rejected = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const std::invalid_argument& missing) {
    rejected = true;
    require(std::string(missing.what()).contains("access key"),
            "the rejection names the missing field");
  }
  require(rejected, "an S3 target without credentials must be rejected");
}

}  // namespace

// The fake stores below listen on loopback, which the public-peer guard
// refuses unless the deployment allows private endpoints. This test proves
// the refusal; main() allows them for the rest.
void verify_a_loopback_store_is_refused_by_default() {
  FakeStore store;
  const docv1::Document document = sample_document("guarded");
  const auto files = sample_bundle(document);
  targets::S3Config config;
  config.endpoint = store.endpoint();
  config.access_key = "AKIAIOSFODNN7EXAMPLE";
  config.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
  config.bucket = "conversions";
  ::unsetenv("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS");
  bool refused = false;
  try {
    targets::upload_bundle(config, files);
  } catch (const std::exception& error) {
    refused = std::string(error.what()).contains("non-public");
  }
  ::setenv("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1", 1);
  require(refused, "an upload to a loopback store is refused by default");
  require(store.received().empty(), "no request reached the loopback store");
}

int main() {
  ::setenv("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1", 1);
  return grparse_test::run_test_main("targets-test", {
      verify_sha256_matches_the_published_vectors,
      verify_bundles_are_reproducible,
      verify_bundle_carries_the_canonical_file_set,
      verify_manifest_describes_every_member,
      verify_the_bundle_carries_the_conversions_status_and_outputs,
      verify_an_image_the_bundle_cannot_write_is_reported,
      verify_an_image_member_is_never_named_for_a_type_it_is_not,
      verify_sigv4_matches_the_published_vector,
      verify_sigv4_signs_the_session_token,
      verify_region_comes_from_the_endpoint,
      verify_explicit_region_overrides_endpoint,
      verify_uploads_land_as_objects,
      verify_session_token_is_refused_over_http,
      verify_session_token_is_refused_without_verification,
      verify_ambient_credentials_need_verification,
      verify_endpoint_hygiene,
      verify_a_partial_failure_reports_the_written_objects,
      verify_a_refused_upload_fails_without_leaking,
      verify_incomplete_targets_are_rejected,
      verify_a_loopback_store_is_refused_by_default,
  });
}
