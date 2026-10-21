// The S3 and HTTP sources against loopback origins in this process, so CI
// runs them on every build: the HTTP source's fetch, filename, redirects
// (the caller's headers stay with the origin it named), status mapping and
// body limit, the public-peer guard, and the S3 source's signed
// ListObjectsV2 paging and GetObject against a fake store. The MinIO and
// TLS proofs stay in remote-sources-e2e-test.

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "../src/storage/connectors.h"
#include "google/rpc/status.pb.h"
#include "grparse/base64.h"
#include "support/check.h"
#include "support/loopback_http.h"

namespace {

namespace parsev1 = ai::pipestream::parse::v1;
namespace storage = grparse::storage;
using grparse_test::LoopbackHttp;
using grparse_test::require;
using grparse_test::require_equal;

constexpr const char* kBody = "# Remote\n\nfetched in process\n";

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name); previous != nullptr) previous_ = previous;
    if (value == nullptr) ::unsetenv(name); else ::setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (previous_.has_value()) ::setenv(name_, previous_->c_str(), 1); else ::unsetenv(name_);
  }

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

parsev1::Source http(const std::string& url) {
  parsev1::Source source;
  source.mutable_http()->set_url(url);
  return source;
}

parsev1::FileSource resolved_ok(const parsev1::Source& source, const std::string& label) {
  parsev1::FileSource file;
  const grpc::Status status = storage::resolve_source(source, "sources[0]", &file);
  require(status.ok(), label + " resolves: " + status.error_message());
  return file;
}

parsev1::OptionRejection rejection_of(const grpc::Status& status, const std::string& label) {
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          label + ": expected INVALID_ARGUMENT, got " + std::to_string(status.error_code()) + ": " +
              status.error_message());
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()), label + ": details parse");
  parsev1::OptionRejection rejection;
  require(rich.details_size() == 1 && rich.details(0).UnpackTo(&rejection),
          label + ": the detail is an OptionRejection");
  return rejection;
}

grpc::Status resolve(const parsev1::Source& source) {
  parsev1::FileSource file;
  return storage::resolve_source(source, "sources[0]", &file);
}

std::string origin_handler(const LoopbackHttp::Request& request, const std::string& other_origin) {
  using Headers = std::vector<std::pair<std::string, std::string>>;
  const std::string path = request.target.substr(0, request.target.find('?'));
  if (path == "/doc.md") return LoopbackHttp::response(200, kBody);
  if (path == "/download") {
    return LoopbackHttp::response(200, kBody,
                                  Headers{{"Content-Disposition", "attachment; filename=\"real.md\""}});
  }
  if (path == "/hop") return LoopbackHttp::response(302, "", Headers{{"Location", "/doc.md"}});
  if (path == "/elsewhere") {
    return LoopbackHttp::response(302, "", Headers{{"Location", other_origin + "/doc.md"}});
  }
  if (path == "/loop") return LoopbackHttp::response(302, "", Headers{{"Location", "/loop"}});
  if (path == "/file") return LoopbackHttp::response(302, "", Headers{{"Location", "file:///etc/hostname"}});
  if (path == "/secret") {
    return request.header("x-token") == "t0ken" ? LoopbackHttp::response(200, kBody)
                                                : LoopbackHttp::response(401, "no");
  }
  if (path == "/boom") return LoopbackHttp::response(503, "down");
  if (path == "/big") return LoopbackHttp::response(200, std::string(4096, 'x'));
  return LoopbackHttp::response(404, "missing");
}

void http_source_fetches() {
  ScopedEnv allowed("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1");
  ScopedEnv kinds("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  LoopbackHttp origin([](const auto& request) { return origin_handler(request, ""); });

  parsev1::FileSource file = resolved_ok(http(origin.base() + "/doc.md?sig=1"), "plain");
  require_equal(file.filename(), std::string("doc.md"), "filename from the URL path");
  require_equal(grparse::decode_base64(file.base64_string()), std::string(kBody), "bytes");

  file = resolved_ok(http(origin.base() + "/download"), "content-disposition");
  require_equal(file.filename(), std::string("real.md"), "filename from Content-Disposition");

  file = resolved_ok(http(origin.base() + "/hop"), "same-origin redirect");
  require_equal(file.filename(), std::string("doc.md"), "filename from the final URL");

  parsev1::Source secret = http(origin.base() + "/secret");
  (*secret.mutable_http()->mutable_headers())["X-Token"] = "t0ken";
  resolved_ok(secret, "request header");

  require_equal(rejection_of(resolve(http(origin.base() + "/secret")), "401").option_path(),
                std::string("sources[0].http.url"), "4xx names the url");
  require_equal(rejection_of(resolve(http(origin.base() + "/nothing")), "404").option_path(),
                std::string("sources[0].http.url"), "404 names the url");
  for (const auto& [path, needle] : std::vector<std::pair<std::string, std::string>>{
           {"/boom", "HTTP 503"}, {"/loop", "more than 5 redirects"}, {"/file", "not http or https"}}) {
    const grpc::Status status = resolve(http(origin.base() + path));
    require(status.error_code() == grpc::StatusCode::UNAVAILABLE,
            path + " is UNAVAILABLE: " + status.error_message());
    require(status.error_message().find(needle) != std::string::npos,
            path + " says '" + needle + "': " + status.error_message());
  }
  {
    ScopedEnv small("GRPARSE_SOURCE_MAX_BYTES", "1024");
    const grpc::Status status = resolve(http(origin.base() + "/big"));
    require(status.error_code() == grpc::StatusCode::UNAVAILABLE &&
                status.error_message().find("larger than 1024 bytes") != std::string::npos,
            "a body past GRPARSE_SOURCE_MAX_BYTES fails: " + status.error_message());
  }
}

// 127.0.0.1 and localhost are two origins on one socket: the header goes to
// the first and must not follow the redirect to the second.
void headers_stay_with_their_origin() {
  ScopedEnv allowed("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1");
  ScopedEnv kinds("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  std::string other;
  LoopbackHttp origin([&other](const auto& request) { return origin_handler(request, other); });
  other = origin.base("localhost");
  parsev1::Source source = http(origin.base() + "/elsewhere");
  (*source.mutable_http()->mutable_headers())["X-Token"] = "t0ken";
  resolved_ok(source, "cross-origin redirect");
  const auto received = origin.received();
  require(received.size() == 2, "two requests: the redirect and the document");
  require_equal(received[0].header("x-token"), std::string("t0ken"), "the named origin gets the header");
  require_equal(received[1].header("host").substr(0, 9), std::string("localhost"), "second hop host");
  require(received[1].header("x-token").empty(), "the other origin never sees the header");

  parsev1::Source same = http(origin.base() + "/hop");
  (*same.mutable_http()->mutable_headers())["X-Token"] = "t0ken";
  resolved_ok(same, "same-origin redirect with a header");
  const auto after = origin.received();
  require_equal(after.back().header("x-token"), std::string("t0ken"), "same origin keeps the header");
}

void private_origins_are_blocked() {
  ScopedEnv guarded("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", nullptr);
  ScopedEnv kinds("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  LoopbackHttp origin([](const auto& request) { return origin_handler(request, ""); });
  for (const std::string& base : {origin.base(), origin.base("localhost")}) {
    const auto rejection = rejection_of(resolve(http(base + "/doc.md")), base);
    require_equal(rejection.option_path(), std::string("sources[0].http.url"), "names the url");
  }
  parsev1::Source s3;
  s3.mutable_s3()->set_endpoint(origin.base());
  s3.mutable_s3()->set_bucket("b");
  s3.mutable_s3()->set_access_key("AKIDEXAMPLE");
  s3.mutable_s3()->set_secret_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  const grpc::Status status = resolve(s3);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              status.error_message().find("non-public") != std::string::npos,
          "an S3 source on loopback is blocked: " + status.error_message());
  require(status.error_message().find("wJalrXUtnFEMI") == std::string::npos, "no secret in the error");
  require(origin.received().empty(), "no request reached a private origin");
}

std::string list_page(const std::vector<std::string>& keys, const std::string& next) {
  std::string xml = "<?xml version=\"1.0\"?><ListBucketResult><Name>b</Name>";
  for (const std::string& key : keys) xml += "<Contents><Key>" + key + "</Key></Contents>";
  xml += next.empty() ? "<IsTruncated>false</IsTruncated>"
                      : "<IsTruncated>true</IsTruncated><NextContinuationToken>" + next +
                            "</NextContinuationToken>";
  return xml + "</ListBucketResult>";
}

// A path-style store: GET /b?list-type=2... lists, GET /b/<key> reads.
// The listing comes in two pages: the first key, then the rest.
std::string fake_s3(const LoopbackHttp::Request& request, const std::vector<std::string>& keys) {
  if (!request.header("authorization").starts_with("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/")) {
    return LoopbackHttp::response(403, "unsigned");
  }
  const std::string& target = request.target;
  if (target.starts_with("/b?")) {
    if (target.find("list-type=2") == std::string::npos) return LoopbackHttp::response(400, "v1");
    if (target.find("continuation-token=page2") != std::string::npos) {
      return LoopbackHttp::response(200, list_page({keys.begin() + 1, keys.end()}, ""));
    }
    return LoopbackHttp::response(200, list_page({keys.front()}, "page2"));
  }
  if (target == "/b/docs/report.md") return LoopbackHttp::response(200, kBody);
  if (target == "/b/docs/big.bin") return LoopbackHttp::response(200, std::string(4096, 'x'));
  return LoopbackHttp::response(404, "<Error><Code>NoSuchKey</Code></Error>");
}

parsev1::Source s3_source(const std::string& endpoint, const std::string& prefix) {
  parsev1::Source source;
  auto* s3 = source.mutable_s3();
  s3->set_endpoint(endpoint);
  s3->set_bucket("b");
  s3->set_key_prefix(prefix);
  s3->set_access_key("AKIDEXAMPLE");
  s3->set_secret_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
  return source;
}

void s3_source_reads_one_object() {
  ScopedEnv allowed("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1");
  ScopedEnv kinds("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  ScopedEnv ambient("GRPARSE_S3_AMBIENT_CREDENTIALS", nullptr);
  LoopbackHttp one([](const auto& request) {
    return fake_s3(request, {"docs/", "docs/report.md"});
  });
  const parsev1::FileSource file = resolved_ok(s3_source(one.base(), "docs/"), "s3 one object");
  require_equal(file.filename(), std::string("report.md"), "s3 filename");
  require_equal(grparse::decode_base64(file.base64_string()), std::string(kBody), "s3 bytes");
  const auto requests = one.received();
  require(requests.size() == 3, "two listing pages and one GetObject, got " +
                                    std::to_string(requests.size()));
  require(requests[0].target.find("prefix=docs%2F") != std::string::npos,
          "the prefix is encoded in the listing: " + requests[0].target);
  require(requests[1].target.find("continuation-token=page2") != std::string::npos,
          "the second page carries the token");
  require_equal(requests[2].target, std::string("/b/docs/report.md"), "GetObject path");
  require(!requests[2].header("x-amz-date").empty() &&
              !requests[2].header("x-amz-content-sha256").empty(),
          "GetObject carries the SigV4 headers");

  LoopbackHttp two([](const auto& request) {
    return fake_s3(request, {"docs/a.md", "docs/report.md"});
  });
  require_equal(rejection_of(resolve(s3_source(two.base(), "docs/")), "two objects").option_path(),
                std::string("sources[0].s3.key_prefix"), "two objects name key_prefix");

  LoopbackHttp big([](const auto& request) { return fake_s3(request, {"docs/", "docs/big.bin"}); });
  ScopedEnv small("GRPARSE_SOURCE_MAX_BYTES", "1024");
  const grpc::Status capped = resolve(s3_source(big.base(), "docs/"));
  require(capped.error_code() == grpc::StatusCode::UNAVAILABLE &&
              capped.error_message().find("larger than 1024 bytes") != std::string::npos,
          "an object past GRPARSE_SOURCE_MAX_BYTES fails: " + capped.error_message());

  parsev1::Source wrong = s3_source(one.base(), "docs/");
  wrong.mutable_s3()->set_access_key("SOMEONEELSE");
  const grpc::Status refused = resolve(wrong);
  require(refused.error_code() == grpc::StatusCode::UNAVAILABLE &&
              refused.error_message().find("HTTP 403") != std::string::npos,
          "a store refusal is UNAVAILABLE with its status: " + refused.error_message());
}

// The credential fields carry debug_redact, so protobuf's debug renderings
// (AbslStringify, logging) print a placeholder instead of the value.
void secrets_are_marked_redacted() {
  const std::vector<std::pair<const google::protobuf::Descriptor*, std::string>> fields = {
      {parsev1::S3Source::descriptor(), "secret_key"},
      {parsev1::S3Target::descriptor(), "secret_key"},
      {parsev1::HttpSource::descriptor(), "headers"}};
  for (const auto& [message, name] : fields) {
    const google::protobuf::FieldDescriptor* field = message->FindFieldByName(name);
    require(field != nullptr && field->options().debug_redact(),
            std::string(message->full_name()) + "." + name + " is debug_redact");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("remote-sources-test", "ok",
                                     {http_source_fetches, headers_stay_with_their_origin,
                                      private_origins_are_blocked, s3_source_reads_one_object,
                                      secrets_are_marked_redacted});
}
