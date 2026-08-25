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
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../src/targets/bundle.h"
#include "../src/targets/s3_client.h"
#include "../src/targets/s3_uploader.h"
#include "../src/targets/sha256.h"
#include "../src/targets/sigv4.h"
#include "../src/targets/zip_writer.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/base64.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace targets = grparse::targets;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

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
  const parsev1::DocumentExports exports = sample_exports(document);
  const std::string first = targets::write_zip(targets::build_bundle(document, exports));
  const std::string second = targets::write_zip(targets::build_bundle(document, exports));
  require(first == second, "two bundles of one document must be byte-identical");
  require(targets::sha256_hex(first) == targets::sha256_hex(second),
          "two bundles of one document must hash equal");
  require(!first.empty() && first.starts_with("PK\x03\x04"),
          "the archive must start with a ZIP local file header");

  const docv1::Document changed = sample_document("a different document");
  const std::string other =
      targets::write_zip(targets::build_bundle(changed, sample_exports(changed)));
  require(targets::sha256_hex(other) != targets::sha256_hex(first),
          "a changed document must change the archive");
}

// The members the bundle is defined to carry, in the order every target
// writes them.
void verify_bundle_carries_the_canonical_file_set() {
  const docv1::Document document = sample_document("members");
  const auto files = targets::build_bundle(document, sample_exports(document));
  for (const auto& path : {"document.json", "document.pb", "exports/markdown.md",
                           "exports/text.txt", "manifest.json", "pages/page_0001.png",
                           "pages/page_0002.png", "pictures/pic_0001.png"}) {
    member(files, path);
  }
  require(files.size() == 8, "the bundle carries exactly its own members, not more");
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
  const auto files = targets::build_bundle(document, sample_exports(document));
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

void verify_region_comes_from_the_endpoint() {
  require(targets::region_for_endpoint("https://s3.eu-west-1.amazonaws.com") == "eu-west-1",
          "a regional AWS endpoint names its region");
  require(targets::region_for_endpoint("bucket.s3.us-west-2.amazonaws.com") == "us-west-2",
          "a virtual-host AWS endpoint names its region");
  require(targets::region_for_endpoint("https://s3.amazonaws.com") == "us-east-1",
          "the regionless AWS endpoint defaults");
  require(targets::region_for_endpoint("http://127.0.0.1:9000") == "us-east-1",
          "a store that encodes no region defaults");
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
    std::string body;
  };

  FakeStore() {
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
    const std::string length = header_value(head, "content-length");
    const size_t expected = length.empty() ? 0 : std::stoul(length);

    std::string body = buffer.substr(header_end + 4);
    while (body.size() < expected) {
      std::string more;
      if (!read_more(connection, &more)) return;
      body += more;
    }
    request.body = body.substr(0, expected);

    const std::string response = "HTTP/1.1 200 OK\r\nETag: " + etag_for(request.body) +
                                 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
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
  const auto files = targets::build_bundle(document, sample_exports(document));

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
  const auto files = targets::build_bundle(document, sample_exports(document));

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

void verify_incomplete_targets_are_rejected() {
  const docv1::Document document = sample_document("incomplete");
  const auto files = targets::build_bundle(document, sample_exports(document));
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

int main() {
  try {
    verify_sha256_matches_the_published_vectors();
    verify_bundles_are_reproducible();
    verify_bundle_carries_the_canonical_file_set();
    verify_manifest_describes_every_member();
    verify_sigv4_matches_the_published_vector();
    verify_region_comes_from_the_endpoint();
    verify_uploads_land_as_objects();
    verify_a_refused_upload_fails_without_leaking();
    verify_incomplete_targets_are_rejected();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "targets-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
