// End to end proof of the S3 and HTTP sources: S3Source against RustFS over
// http and over https with a self-signed certificate (verify_ssl unset and
// true refuse the certificate, false accepts it), and HttpSource against a
// local http origin (plain fetch, redirect, Content-Disposition, request
// headers, 4xx, 5xx, redirect limits, non-http redirect, body cap) and a
// self-signed https origin it must refuse.
//
// Every source goes through storage::resolve_source, the call parse_source
// makes for sources[0], and the fetched bytes are compared with the sample.
//
// Skips (exit 77) unless GRPARSE_E2E_REMOTE is set;
// scripts/test/remote-sources-e2e-test.sh starts the backends and runs it.

#include <cstdlib>
#include <print>
#include <string>

#include "../src/storage/connectors.h"
#include "grparse/base64.h"
#include "support/check.h"
#include "support/document_builder.h"
#include "support/storage_fixture_plugin.h"

namespace {

namespace parsev1 = ai::pipestream::parse::v1;
namespace storage = grparse::storage;
using grparse_test::require;
using grparse_test::require_equal;

std::string env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') throw std::runtime_error(std::string(name) + " is unset");
  return value;
}

std::string sample() { return grparse_test::read_file_bytes(env("GRPARSE_E2E_SAMPLE")); }

void expect_resolved(const parsev1::Source& source, const std::string& label,
                     const std::string& filename = "sample.md") {
  parsev1::FileSource file;
  const grpc::Status status = storage::resolve_source(source, "sources[0]", &file);
  require(status.ok(), label + " resolves: " + status.error_message());
  require_equal(file.filename(), filename, label + " filename");
  require_equal(grparse::decode_base64(file.base64_string()), sample(), label + " bytes");
  std::println("source {}: {} ({} bytes)", label, file.filename(), sample().size());
}

void expect_refused(const parsev1::Source& source, const std::string& label, grpc::StatusCode code,
                    const std::string& needle) {
  parsev1::FileSource file;
  const grpc::Status status = storage::resolve_source(source, "sources[0]", &file);
  require(status.error_code() == code, label + " expected code " + std::to_string(code) + ", got " +
                                           std::to_string(status.error_code()) + ": " +
                                           status.error_message());
  require(status.error_message().find(needle) != std::string::npos,
          label + " message names '" + needle + "': " + status.error_message());
  std::println("source {} refused ({}): {}", label, static_cast<int>(code), status.error_message());
}

parsev1::Source s3(const std::string& endpoint, const std::string& prefix) {
  parsev1::Source source;
  auto* s3 = source.mutable_s3();
  s3->set_endpoint(endpoint);
  s3->set_bucket("grparse-in");
  s3->set_key_prefix(prefix);
  s3->set_access_key(env("GRPARSE_E2E_S3_ACCESS_KEY"));
  s3->set_secret_key(env("GRPARSE_E2E_S3_SECRET_KEY"));
  s3->set_region("us-east-1");
  return source;
}

void s3_over_http() {
  const std::string endpoint = env("GRPARSE_E2E_S3_HTTP_ENDPOINT");
  expect_resolved(s3(endpoint, "docs/"), "s3 http docs/");
  expect_resolved(s3(endpoint, "docs/sample.md"), "s3 http exact key");
  expect_refused(s3(endpoint, "two/"), "s3 two objects", grpc::StatusCode::INVALID_ARGUMENT,
                 "sources[0].s3.key_prefix");
  expect_refused(s3(endpoint, "nothing/"), "s3 empty prefix", grpc::StatusCode::INVALID_ARGUMENT,
                 "sources[0].s3.key_prefix");
  parsev1::Source wrong = s3(endpoint, "docs/");
  wrong.mutable_s3()->set_secret_key("not-the-secret");
  expect_refused(wrong, "s3 wrong secret", grpc::StatusCode::UNAVAILABLE, "HTTP 403");
}

void s3_over_self_signed_https() {
  const std::string endpoint = env("GRPARSE_E2E_S3_HTTPS_ENDPOINT");
  parsev1::Source source = s3(endpoint, "docs/");
  expect_refused(source, "s3 https verify_ssl unset", grpc::StatusCode::UNAVAILABLE, "certificate");
  source.mutable_s3()->set_verify_ssl(true);
  expect_refused(source, "s3 https verify_ssl true", grpc::StatusCode::UNAVAILABLE, "certificate");
  source.mutable_s3()->set_verify_ssl(false);
  expect_resolved(source, "s3 https verify_ssl false");
}

parsev1::Source http(const std::string& url) {
  parsev1::Source source;
  source.mutable_http()->set_url(url);
  return source;
}

void http_sources() {
  const std::string base = env("GRPARSE_E2E_HTTP_BASE");
  expect_resolved(http(base + "/sample.md"), "http plain");
  expect_resolved(http(base + "/redirect"), "http redirect");
  expect_resolved(http(base + "/download?id=1"), "http content-disposition");
  parsev1::Source with_header = http(base + "/needs-header");
  (*with_header.mutable_http()->mutable_headers())["X-E2E-Token"] = "e2e-token";
  expect_resolved(with_header, "http request header", "needs-header");
  expect_refused(http(base + "/needs-header"), "http 401", grpc::StatusCode::INVALID_ARGUMENT,
                 "sources[0].http.url answered HTTP 401");
  expect_refused(http(base + "/missing"), "http 404", grpc::StatusCode::INVALID_ARGUMENT,
                 "answered HTTP 404");
  expect_refused(http(base + "/error"), "http 500", grpc::StatusCode::UNAVAILABLE,
                 "answered HTTP 500");
  expect_refused(http(base + "/redirect-loop"), "http redirect loop", grpc::StatusCode::UNAVAILABLE,
                 "HttpSource");
  expect_refused(http(base + "/redirect-file"), "http redirect to file://",
                 grpc::StatusCode::UNAVAILABLE, "HttpSource");
  ::setenv("GRPARSE_SOURCE_MAX_BYTES", "1048576", 1);
  expect_refused(http(base + "/big"), "http body cap", grpc::StatusCode::UNAVAILABLE,
                 "larger than 1048576 bytes");
  ::unsetenv("GRPARSE_SOURCE_MAX_BYTES");
  expect_refused(http(env("GRPARSE_E2E_HTTPS_BASE") + "/sample.md"), "https self-signed",
                 grpc::StatusCode::UNAVAILABLE, "certificate");
}

}  // namespace

int main() {
  if (std::getenv("GRPARSE_E2E_REMOTE") == nullptr) {
    std::println("remote-sources-e2e-test: skipped (GRPARSE_E2E_REMOTE unset)");
    return 77;
  }
  ::unsetenv("GRPARSE_ALLOWED_SOURCE_TYPES");
  ::unsetenv("GRPARSE_S3_AMBIENT_CREDENTIALS");
  return grparse_test::run_test_main("remote-sources-e2e-test", "ok",
                                     {s3_over_http, s3_over_self_signed_https, http_sources});
}
