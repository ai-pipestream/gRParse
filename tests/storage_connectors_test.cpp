// Unit tests for the storage connectors: the Azure Shared Key string to
// sign, connection string parsing, base64url, the RS256 service account
// assertion (signed with a key generated here and verified with its public
// half), the source and target policy, and the typed plugin registry
// (google.protobuf.Any configs unpacked to the registered message type),
// and the fetch hardening every connector shares: the public-peer guard
// against a live loopback origin, endpoint shape, https token endpoints,
// the response and document size caps, and the bounded single-object
// listing.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <google/protobuf/util/json_util.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "../src/storage/azure_blob.h"
#include "../src/storage/connectors.h"
#include "../src/storage/remote_sources.h"
#include "../src/storage/google_auth.h"
#include "../src/storage/http_call.h"
#include "../src/storage/object_store.h"
#include "google/rpc/status.pb.h"
#include "grparse/base64.h"
#include "storage_wire.pb.h"
#include "support/check.h"
#include "support/loopback_http.h"
#include "support/storage_fixture_plugin.h"

namespace {

namespace parsev1 = ai::pipestream::parse::v1;
namespace storage = grparse::storage;
using grparse_test::require;
using grparse_test::require_equal;

parsev1::OptionRejection rejection_of(const grpc::Status& status) {
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "expected INVALID_ARGUMENT, got " + std::to_string(status.error_code()) + ": " +
              status.error_message());
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()), "the details parse as google.rpc.Status");
  require(rich.details_size() == 1, "one detail");
  parsev1::OptionRejection rejection;
  require(rich.details(0).UnpackTo(&rejection), "the detail is an OptionRejection");
  return rejection;
}

void expect_rejection(const grpc::Status& status, const std::string& path,
                      parsev1::OptionRejectionReason reason) {
  const auto rejection = rejection_of(status);
  require_equal(rejection.option_path(), path, "option_path");
  require_equal(parsev1::OptionRejectionReason_Name(rejection.reason()),
                parsev1::OptionRejectionReason_Name(reason), "reason");
}

template <typename Body>
void expect_storage_error(Body body, const std::string& path) {
  try {
    body();
  } catch (const storage::StorageRequestError& error) {
    require_equal(error.option_path(), path, "StorageRequestError option_path");
    return;
  }
  throw std::runtime_error("expected a StorageRequestError at " + path);
}

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (value == nullptr) ::unsetenv(name); else ::setenv(name, value, 1);
  }
  ~ScopedEnv() { ::unsetenv(name_); }

 private:
  const char* name_;
};

std::string base64url_decode(std::string text) {
  for (char& c : text) {
    if (c == '-') c = '+';
    if (c == '_') c = '/';
  }
  while (text.size() % 4 != 0) text += '=';
  return grparse::decode_base64(text);
}

void azure_string_to_sign_vector() {
  // The Shared Key layout for a Put Blob: verb, the eleven standard headers
  // (only Content-Length and Content-Type set), the x-ms-* headers sorted
  // by lowercased name, then the canonicalized resource with its query
  // parameters sorted and lowercased.
  const std::string got = storage::azure_string_to_sign(
      "PUT", 11, "application/json",
      {{"x-ms-version", "2021-08-06"}, {"x-ms-date", "Thu, 15 Oct 2026 07:00:00 GMT"},
       {"X-MS-Blob-Type", "BlockBlob"}},
      "acct", "/container/out/doc.json", {{"timeout", "30"}, {"Comp", "list"}});
  const std::string expected =
      "PUT\n\n\n11\n\napplication/json\n\n\n\n\n\n\n"
      "x-ms-blob-type:BlockBlob\n"
      "x-ms-date:Thu, 15 Oct 2026 07:00:00 GMT\n"
      "x-ms-version:2021-08-06\n"
      "/acct/container/out/doc.json\ncomp:list\ntimeout:30";
  require_equal(got, expected, "string to sign");
  // A zero Content-Length is the empty string from version 2015-02-21 on.
  const std::string get = storage::azure_string_to_sign("GET", 0, "", {}, "acct", "/c", {});
  require_equal(get, std::string("GET\n\n\n\n\n\n\n\n\n\n\n\n/acct/c"), "GET string to sign");
}

void azure_connection_strings() {
  const std::string key = grparse::encode_base64("secret-key", 10);
  const auto parsed = storage::parse_azure_connection_string(
      "DefaultEndpointsProtocol=https;AccountName=acct;AccountKey=" + key +
          ";EndpointSuffix=core.windows.net",
      "acct", "sources[0].azure_blob.coordinates");
  require_equal(parsed.account_name, std::string("acct"), "account");
  require_equal(parsed.account_key, std::string("secret-key"), "decoded key");
  require_equal(parsed.blob_endpoint, std::string("https://acct.blob.core.windows.net"), "endpoint");
  const auto emulator = storage::parse_azure_connection_string(
      "AccountName=devstoreaccount1;AccountKey=" + key +
          ";BlobEndpoint=http://127.0.0.1:10000/devstoreaccount1/",
      "devstoreaccount1", "p");
  require_equal(emulator.blob_endpoint, std::string("http://127.0.0.1:10000/devstoreaccount1"),
                "BlobEndpoint wins, trailing slash dropped");
  expect_storage_error([] { storage::parse_azure_connection_string("UseDevelopmentStorage=true", "a", "p"); },
                       "p.connection_string");
  expect_storage_error([] { storage::parse_azure_connection_string("AccountName=a", "a", "p"); },
                       "p.connection_string");
  expect_storage_error(
      [&] { storage::parse_azure_connection_string("AccountName=a;AccountKey=" + key, "b", "p"); },
      "p.account_name");
}

void base64url_encoding() {
  require_equal(storage::base64url("f"), std::string("Zg"), "no padding");
  require_equal(storage::base64url(std::string("\xfb\xff", 2)), std::string("-_8"), "url alphabet");
  require_equal(storage::base64url(""), std::string(""), "empty");
}

struct KeyPair {
  std::string private_pem;
  bssl::UniquePtr<EVP_PKEY> key;
};

KeyPair generate_key() {
  bssl::UniquePtr<RSA> rsa(RSA_new());
  bssl::UniquePtr<BIGNUM> exponent(BN_new());
  require(BN_set_word(exponent.get(), RSA_F4) == 1, "exponent");
  require(RSA_generate_key_ex(rsa.get(), 2048, exponent.get(), nullptr) == 1, "RSA key");
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  require(EVP_PKEY_set1_RSA(key.get(), rsa.get()) == 1, "EVP key");
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  require(PEM_write_bio_PKCS8PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                        nullptr) == 1,
          "PEM");
  const uint8_t* data = nullptr;
  size_t size = 0;
  require(BIO_mem_contents(bio.get(), &data, &size) == 1, "PEM bytes");
  return {std::string(reinterpret_cast<const char*>(data), size), std::move(key)};
}

void rs256_assertion() {
  const KeyPair pair = generate_key();
  parsev1::GoogleCloudStorageServiceAccountKey key;
  key.set_client_email("bot@project.iam.gserviceaccount.com");
  key.set_private_key_id("kid-1");
  key.set_private_key(pair.private_pem);
  key.set_token_uri("https://oauth2.googleapis.com/token");
  const std::string jwt = storage::service_account_assertion(
      key, "https://www.googleapis.com/auth/devstorage.read_write", 1760000000);
  const size_t first = jwt.find('.');
  const size_t second = jwt.find('.', first + 1);
  require(first != std::string::npos && second != std::string::npos, "three JWT parts");
  grparse::storage::wire::JwtHeader header;
  require(google::protobuf::util::JsonStringToMessage(base64url_decode(jwt.substr(0, first)), &header).ok(),
          "header parses as JwtHeader");
  require_equal(header.alg(), std::string("RS256"), "alg");
  require_equal(header.kid(), std::string("kid-1"), "kid");
  grparse::storage::wire::JwtClaims claims;
  require(google::protobuf::util::JsonStringToMessage(
              base64url_decode(jwt.substr(first + 1, second - first - 1)), &claims)
              .ok(),
          "claims parse as JwtClaims");
  require_equal(claims.iss(), key.client_email(), "iss");
  require_equal(claims.aud(), key.token_uri(), "aud");
  require_equal(claims.iat(), 1760000000u, "iat");
  require_equal(claims.exp(), 1760003600u, "exp");
  const std::string signing_input = jwt.substr(0, second);
  const std::string signature = base64url_decode(jwt.substr(second + 1));
  bssl::ScopedEVP_MD_CTX context;
  require(EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, pair.key.get()) == 1,
          "verify init");
  require(EVP_DigestVerify(context.get(), reinterpret_cast<const uint8_t*>(signature.data()),
                           signature.size(), reinterpret_cast<const uint8_t*>(signing_input.data()),
                           signing_input.size()) == 1,
          "the RS256 signature verifies with the public key");
  bool threw = false;
  try {
    storage::rs256_sign("not a key", "x");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an unparsable key is std::invalid_argument");
}

parsev1::Source file_source() {
  parsev1::Source source;
  source.mutable_file()->set_filename("a.txt");
  source.mutable_file()->set_base64_string(grparse::encode_base64("hi", 2));
  return source;
}

void source_policy() {
  parsev1::FileSource resolved;
  {
    ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
    require(storage::resolve_source(file_source(), "sources[0]", &resolved).ok(), "file passes");
    require_equal(resolved.filename(), std::string("a.txt"), "file passes through");
    parsev1::Source http;
    http.mutable_http()->set_url("ftp://example.com/a.pdf");
    expect_rejection(storage::resolve_source(http, "sources[0]", &resolved), "sources[0].http.url",
                     parsev1::OPTION_REJECTION_REASON_INVALID);
    parsev1::Source empty;
    expect_rejection(storage::resolve_source(empty, "sources[0]", &resolved), "sources[0]",
                     parsev1::OPTION_REJECTION_REASON_INVALID);
  }
  {
    ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", "file");
    parsev1::Source azure;
    azure.mutable_azure_blob()->mutable_coordinates()->set_container("c");
    expect_rejection(storage::resolve_source(azure, "sources[0]", &resolved), "sources[0]",
                     parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED);
    require(storage::resolve_source(file_source(), "sources[0]", &resolved).ok(),
            "a listed kind passes");
  }
}

void storage_coordinates_checked() {
  ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  parsev1::FileSource resolved;
  parsev1::Source gcs;
  gcs.mutable_google_cloud_storage()->mutable_coordinates()->set_key_prefix("in/");
  expect_rejection(storage::resolve_source(gcs, "sources[0]", &resolved),
                   "sources[0].google_cloud_storage.coordinates.bucket",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  gcs.mutable_google_cloud_storage()->mutable_coordinates()->set_bucket("b");
  expect_rejection(storage::resolve_source(gcs, "sources[0]", &resolved),
                   "sources[0].google_cloud_storage.coordinates.service_account_key",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  parsev1::Source azure;
  auto* coordinates = azure.mutable_azure_blob()->mutable_coordinates();
  coordinates->set_account_name("acct");
  coordinates->set_container("c");
  coordinates->set_connection_string("UseDevelopmentStorage=true");
  expect_rejection(storage::resolve_source(azure, "sources[0]", &resolved),
                   "sources[0].azure_blob.coordinates.connection_string",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  parsev1::Source drive;
  drive.mutable_google_drive()->mutable_coordinates()->set_path_id("file-1");
  expect_rejection(storage::resolve_source(drive, "sources[0]", &resolved),
                   "sources[0].google_drive.coordinates.refresh_token",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
}

void remote_sources_checked() {
  ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  ScopedEnv ambient("GRPARSE_S3_AMBIENT_CREDENTIALS", nullptr);
  parsev1::FileSource resolved;
  parsev1::Source http;
  http.mutable_http()->set_url("http://127.0.0.1:1/a b.pdf");
  expect_rejection(storage::resolve_source(http, "sources[0]", &resolved), "sources[0].http.url",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  http.mutable_http()->set_url("http://127.0.0.1:1/a.pdf");
  (*http.mutable_http()->mutable_headers())["Bad Name"] = "x";
  expect_rejection(storage::resolve_source(http, "sources[0]", &resolved),
                   "sources[0].http.headers[Bad Name]", parsev1::OPTION_REJECTION_REASON_INVALID);
  http.mutable_http()->clear_headers();
  (*http.mutable_http()->mutable_headers())["Host"] = "elsewhere";
  expect_rejection(storage::resolve_source(http, "sources[0]", &resolved),
                   "sources[0].http.headers[Host]", parsev1::OPTION_REJECTION_REASON_INVALID);
  http.mutable_http()->clear_headers();
  (*http.mutable_http()->mutable_headers())["X-Token"] = "a\r\nInjected: 1";
  expect_rejection(storage::resolve_source(http, "sources[0]", &resolved),
                   "sources[0].http.headers[X-Token]", parsev1::OPTION_REJECTION_REASON_INVALID);
  http.mutable_http()->clear_headers();
  const grpc::Status refused = storage::resolve_source(http, "sources[0]", &resolved);
  require(refused.error_code() == grpc::StatusCode::UNAVAILABLE,
          "a closed port is UNAVAILABLE: " + refused.error_message());

  parsev1::Source s3;
  s3.mutable_s3()->set_endpoint("http://127.0.0.1:1");
  expect_rejection(storage::resolve_source(s3, "sources[0]", &resolved), "sources[0].s3.bucket",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  s3.mutable_s3()->set_bucket("b");
  s3.mutable_s3()->set_access_key("only-access");
  expect_rejection(storage::resolve_source(s3, "sources[0]", &resolved), "sources[0].s3",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  s3.mutable_s3()->clear_access_key();
  const grpc::Status no_credentials = storage::resolve_source(s3, "sources[0]", &resolved);
  require(no_credentials.error_message().find("S3Source omitted credentials") != std::string::npos,
          "no credentials and no ambient opt-in names S3Source: " + no_credentials.error_message());
  s3.mutable_s3()->clear_endpoint();
  expect_rejection(storage::resolve_source(s3, "sources[0]", &resolved), "sources[0].s3.endpoint",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
}

void http_source_filenames() {
  using storage::http_source_filename;
  require_equal(http_source_filename("https://h/x/report.pdf?sig=1#p", ""), std::string("report.pdf"),
                "last path segment, no query");
  require_equal(http_source_filename("https://h/x/my%20doc.pdf", ""), std::string("my doc.pdf"),
                "percent-decoded");
  require_equal(http_source_filename("https://h/", ""), std::string("document"), "no segment");
  require_equal(http_source_filename("https://h", ""), std::string("document"), "no path");
  require_equal(http_source_filename("https://h/a.bin", "attachment; filename=\"real.docx\""),
                std::string("real.docx"), "quoted filename");
  require_equal(http_source_filename("https://h/a.bin", "attachment; filename=plain.txt"),
                std::string("plain.txt"), "token filename");
  require_equal(http_source_filename("https://h/a.bin",
                                     "attachment; filename=\"x.txt\"; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"),
                std::string("r\xC3\xA9sum\xC3\xA9.pdf"), "filename* wins");
  require_equal(http_source_filename("https://h/a.bin", "attachment; filename=\"../../etc/passwd\""),
                std::string("passwd"), "directory parts dropped");
  require_equal(http_source_filename("https://h/a.bin", "attachment; filename=\"..\""),
                std::string("a.bin"), "a dot-dot name falls back to the URL");
}

void target_policy() {
  parsev1::Target inbody;
  inbody.mutable_inbody();
  ScopedEnv allowed("GRPARSE_ALLOWED_TARGET_TYPES", "inbody,zip");
  require(storage::check_target_allowed(inbody).ok(), "inbody listed");
  require(storage::check_target_allowed(parsev1::Target()).ok(), "no target is inbody");
  parsev1::Target put;
  put.mutable_put();
  require(storage::check_target_allowed(put).ok(),
          "put is left to the target step, which answers UNIMPLEMENTED");
  parsev1::Target gcs;
  gcs.mutable_google_cloud_storage()->mutable_coordinates()->set_bucket("b");
  expect_rejection(storage::check_target_allowed(gcs), "target",
                   parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED);
}

void plugin_registry() {
  bool threw = false;
  try {
    storage::ConnectorPlugin reserved;
    reserved.kind = "s3";
    reserved.config_type = parsev1::Source::descriptor();
    storage::register_connector_plugin(reserved);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a built-in kind cannot be taken by a plugin");
  grparse_test::register_directory_plugin();
  threw = false;
  try {
    grparse_test::register_directory_plugin();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a kind registers once");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("grparse-plugin-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  std::ofstream(root / "doc.md") << "# Plugin\n\nfetched through a typed plugin config\n";
  grparse::test::storage::DirectoryConnectorConfig config;
  config.set_root(root.string());
  config.set_name("doc.md");
  parsev1::Source source;
  source.mutable_plugin()->set_kind("test_directory");
  require(source.mutable_plugin()->mutable_config()->PackFrom(config), "pack");
  parsev1::FileSource resolved;
  {
    ScopedEnv external("GRPARSE_ALLOW_EXTERNAL_PLUGINS", nullptr);
    ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", "file,test_directory");
    expect_rejection(storage::resolve_source(source, "sources[0]", &resolved),
                     "sources[0].plugin.kind", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED);
  }
  ScopedEnv external("GRPARSE_ALLOW_EXTERNAL_PLUGINS", "true");
  {
    ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
    expect_rejection(storage::resolve_source(source, "sources[0]", &resolved),
                     "sources[0].plugin.kind", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED);
  }
  ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", "file,test_directory,unregistered");
  require(storage::resolve_source(source, "sources[0]", &resolved).ok(), "plugin source resolves");
  require_equal(resolved.filename(), std::string("doc.md"), "plugin filename");
  require_equal(grparse::decode_base64(resolved.base64_string()),
                std::string("# Plugin\n\nfetched through a typed plugin config\n"), "plugin bytes");

  parsev1::Source wrong = source;
  grparse::test::storage::OtherConfig other;
  other.set_value("x");
  require(wrong.mutable_plugin()->mutable_config()->PackFrom(other), "pack other");
  const auto rejection = rejection_of(storage::resolve_source(wrong, "sources[0]", &resolved));
  require_equal(rejection.option_path(), std::string("sources[0].plugin.config"), "config path");
  require_equal(rejection.rejected_value_raw(),
                std::string("type.googleapis.com/grparse.test.storage.OtherConfig"), "type url");
  parsev1::Source unknown = source;
  unknown.mutable_plugin()->set_kind("unregistered");
  expect_rejection(storage::resolve_source(unknown, "sources[0]", &resolved),
                   "sources[0].plugin.kind", parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE);

  parsev1::Target target;
  target.mutable_plugin()->set_kind("test_directory");
  config.set_root((root / "out").string());
  require(target.mutable_plugin()->mutable_config()->PackFrom(config), "pack target");
  {
    ScopedEnv targets("GRPARSE_ALLOWED_TARGET_TYPES", "inbody,test_directory");
    parsev1::TargetResult result;
    bool handled = false;
    const grpc::Status delivered = storage::deliver_to_store(
        target, {{"doc.json", "{}"}, {"manifest.json", "[]"}}, &result, &handled);
    require(delivered.ok() && handled, "plugin target delivers: " + delivered.error_message());
    require_equal(result.objects_size(), 2, "two objects reported");
    require_equal(grparse_test::read_file_bytes(root / "out" / "manifest.json"), std::string("[]"),
                  "written through the plugin");
  }
  std::filesystem::remove_all(root);
  storage::unregister_connector_plugin("test_directory");
}

void public_addresses() {
  const std::vector<std::pair<std::string, bool>> cases = {
      {"8.8.8.8", true},          {"1.1.1.1", true},         {"172.32.0.1", true},
      {"100.128.0.1", true},      {"2606:4700::1111", true}, {"::ffff:8.8.8.8", true},
      {"127.0.0.1", false},       {"127.255.0.9", false},    {"10.1.2.3", false},
      {"172.16.0.1", false},      {"172.31.255.255", false}, {"192.168.1.1", false},
      {"169.254.169.254", false}, {"100.64.0.1", false},     {"0.0.0.0", false},
      {"224.0.0.1", false},       {"255.255.255.255", false}, {"198.18.0.1", false},
      {"::1", false},             {"::", false},             {"fe80::1", false},
      {"fd00::1", false},         {"fc00::1", false},        {"ff02::1", false},
      {"2001:db8::1", false},     {"::ffff:127.0.0.1", false}, {"::ffff:10.0.0.1", false},
      {"64:ff9b::a9fe:a9fe", false}, {"not-an-address", false}, {"", false}};
  for (const auto& [ip, expected] : cases) {
    require(storage::public_address(ip) == expected,
            "public_address(" + ip + ") should be " + (expected ? "true" : "false"));
  }
}

void endpoint_shapes() {
  require_equal(storage::endpoint_problem("https://storage.example.com"), std::string(), "plain https");
  require_equal(storage::endpoint_problem("http://127.0.0.1:10000/devstoreaccount1"), std::string(),
                "http with a path");
  for (const char* bad : {"ftp://h", "h.example.com", "http://h/a?x=1", "http://h#f",
                                "http://user:pw@h/", "http://h /a"}) {
    require(!storage::endpoint_problem(bad).empty(), std::string("endpoint_problem refuses ") + bad);
  }
  require_equal(storage::url_origin("HTTP://Example.COM/a/b?c"), std::string("http://example.com:80"),
                "default http port, lowercased");
  require_equal(storage::url_origin("https://h.example.com:8443/"),
                std::string("https://h.example.com:8443"), "explicit port");
  require_equal(storage::url_origin("https://h.example.com"), std::string("https://h.example.com:443"),
                "default https port");
}

void source_size_setting() {
  {
    ScopedEnv unset("GRPARSE_SOURCE_MAX_BYTES", nullptr);
    require(storage::source_max_bytes() == 256ull * 1024 * 1024, "default 256 MiB");
  }
  {
    ScopedEnv set("GRPARSE_SOURCE_MAX_BYTES", "1024");
    require(storage::source_max_bytes() == 1024, "set value");
  }
  for (const char* bad : {"0", "-1", "12k", "x"}) {
    ScopedEnv set("GRPARSE_SOURCE_MAX_BYTES", bad);
    bool refused = false;
    try {
      storage::source_max_bytes();
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    require(refused, std::string("GRPARSE_SOURCE_MAX_BYTES=") + bad + " is refused");
  }
}

// The guard runs on the connected socket, before a byte of the request is
// sent: the origin sees a connection and no request.
void private_peers_refused() {
  grparse_test::LoopbackHttp origin([](const auto&) {
    return grparse_test::LoopbackHttp::response(200, "hello");
  });
  storage::HttpRequest request;
  request.url = origin.base() + "/secret";
  request.headers = {{"Authorization", "Bearer must-not-leave"}};
  {
    ScopedEnv guarded("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", nullptr);
    bool refused = false;
    try {
      storage::http_call(request, "probe");
    } catch (const storage::HttpPeerRefused& error) {
      refused = true;
      require(std::string(error.what()).find("non-public") != std::string::npos, error.what());
    }
    require(refused, "a loopback peer is refused by default");
    require(origin.received().empty(), "no request reached the refused peer");
    request.url = origin.base("localhost") + "/secret";
    refused = false;
    try {
      storage::http_call(request, "probe");
    } catch (const storage::HttpPeerRefused&) {
      refused = true;
    }
    require(refused, "a name that resolves to loopback is refused too");
    require(origin.received().empty(), "still no request");
  }
  {
    ScopedEnv allowed("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", "1");
    const storage::HttpResponse reply = storage::http_call(request, "probe");
    require(reply.status == 200 && reply.body == "hello", "allowed private endpoints are reached");
    request.max_body_bytes = 4;
    bool capped = false;
    try {
      storage::http_call(request, "probe");
    } catch (const std::runtime_error& error) {
      capped = std::string(error.what()).find("larger than 4 bytes") != std::string::npos;
    }
    require(capped, "a body past max_body_bytes fails the exchange");
  }
}

void connectors_guard_their_endpoints() {
  grparse_test::LoopbackHttp origin([](const auto&) {
    return grparse_test::LoopbackHttp::response(200, "{}");
  });
  ScopedEnv allowed("GRPARSE_ALLOWED_SOURCE_TYPES", nullptr);
  ScopedEnv guarded("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS", nullptr);
  parsev1::FileSource resolved;
  parsev1::Source gcs;
  auto* coordinates = gcs.mutable_google_cloud_storage()->mutable_coordinates();
  coordinates->set_bucket("b");
  coordinates->set_endpoint(origin.base());
  expect_rejection(storage::resolve_source(gcs, "sources[0]", &resolved), "sources[0]",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
  require(origin.received().empty(), "the GCS listing never reached the loopback store");

  coordinates->set_endpoint(origin.base() + "/storage?");
  expect_rejection(storage::resolve_source(gcs, "sources[0]", &resolved),
                   "sources[0].google_cloud_storage.coordinates.endpoint",
                   parsev1::OPTION_REJECTION_REASON_INVALID);

  coordinates->set_endpoint("https://storage.example.com");
  auto* key = coordinates->mutable_service_account_key();
  key->set_client_email("svc@example.iam.gserviceaccount.com");
  key->set_private_key("unused");
  key->set_token_uri("http://oauth2.example.com/token");
  expect_rejection(storage::resolve_source(gcs, "sources[0]", &resolved),
                   "sources[0].google_cloud_storage.coordinates.service_account_key.token_uri",
                   parsev1::OPTION_REJECTION_REASON_INVALID);
}

// A store that records the limit fetch_single asked for.
class CountingStore final : public storage::ObjectStore {
 public:
  explicit CountingStore(std::vector<std::string> names) : names_(std::move(names)) {}
  std::vector<std::string> list(const std::string&, std::optional<uint32_t> limit) override {
    asked = limit;
    std::vector<std::string> out = names_;
    if (limit.has_value() && out.size() > *limit) out.resize(*limit);
    return out;
  }
  std::string get(const std::string&) override { return "bytes"; }
  storage::WrittenObject put(const std::string&, const std::string&) override { return {}; }
  std::optional<uint32_t> asked;

 private:
  std::vector<std::string> names_;
};

void single_object_listing_is_bounded() {
  std::vector<std::string> many;
  for (int i = 0; i < 5000; ++i) many.push_back("p/" + std::to_string(i));
  CountingStore store(many);
  bool refused = false;
  try {
    storage::fetch_single(store, "p/", std::nullopt, "prefix");
  } catch (const storage::StorageRequestError& error) {
    refused = std::string(error.what()).find("1000 or more objects") != std::string::npos;
  }
  require(refused, "a prefix over many objects is refused after one bounded listing");
  require(store.asked == std::optional<uint32_t>(1000), "no limit asks for 1000");
  try {
    storage::fetch_single(store, "p/", 100000u, "prefix");
  } catch (const storage::StorageRequestError&) {
  }
  require(store.asked == std::optional<uint32_t>(1000), "a larger limit is capped at 1000");
  CountingStore one({"p/", "p/only.pdf"});
  const storage::FetchedDocument fetched = storage::fetch_single(one, "p/", 5u, "prefix");
  require_equal(fetched.filename, std::string("only.pdf"), "folder marker skipped");
  require(one.asked == std::optional<uint32_t>(5), "a smaller limit is kept");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("storage-connectors-test", "ok",
                                     {azure_string_to_sign_vector, azure_connection_strings,
                                      base64url_encoding, rs256_assertion, source_policy,
                                      storage_coordinates_checked, remote_sources_checked,
                                      http_source_filenames, target_policy, plugin_registry,
                                      public_addresses, endpoint_shapes, source_size_setting,
                                      private_peers_refused, connectors_guard_their_endpoints,
                                      single_object_listing_is_bounded});
}
