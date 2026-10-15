// Unit tests for the storage connectors: the Azure Shared Key string to
// sign, connection string parsing, base64url, the RS256 service account
// assertion (signed with a key generated here and verified with its public
// half), the source and target policy, and the typed plugin registry
// (google.protobuf.Any configs unpacked to the registered message type).

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
#include "../src/storage/google_auth.h"
#include "google/rpc/status.pb.h"
#include "grparse/base64.h"
#include "storage_wire.pb.h"
#include "support/check.h"
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
    http.mutable_http()->set_url("https://example.com/a.pdf");
    expect_rejection(storage::resolve_source(http, "sources[0]", &resolved), "sources[0].http",
                     parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED);
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

}  // namespace

int main() {
  return grparse_test::run_test_main("storage-connectors-test", "ok",
                                     {azure_string_to_sign_vector, azure_connection_strings,
                                      base64url_encoding, rs256_assertion, source_policy,
                                      storage_coordinates_checked, target_policy, plugin_registry});
}
