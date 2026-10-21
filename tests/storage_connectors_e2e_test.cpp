// End to end proof of the storage connectors against emulated backends:
// Azure Blob through Azurite, Google Cloud Storage through fake-gcs-server
// (anonymously and with a service account whose RS256 assertion the mock
// token endpoint verifies), Google Drive through a mock Drive v3 API with a
// refresh-token grant, the S3 target through MinIO for the bundle layout
// parity check, and a registered plugin with a typed config.
//
// Sources go through storage::resolve_source, the call parse_source makes
// for sources[0]; targets go through targets::deliver, the call the
// conversion makes for request.target. Every object written is read back
// from the store and compared with the bundle.
//
// Skips (exit 77) unless GRPARSE_E2E_STORAGE is set;
// scripts/test/storage-connectors-e2e-test.sh starts the backends and runs it.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <set>
#include <string>

#include <google/protobuf/util/json_util.h>

#include "../src/storage/azure_blob.h"
#include "../src/storage/connectors.h"
#include "../src/storage/google_storage.h"
#include "../src/storage/http_call.h"
#include "../src/targets/bundle.h"
#include "../src/targets/target_step.h"
#include "grparse/base64.h"
#include "support/check.h"
#include "support/document_builder.h"
#include "support/storage_fixture_plugin.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
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

parsev1::AzureBlobCoordinates azure(const std::string& container, const std::string& prefix) {
  parsev1::AzureBlobCoordinates coordinates;
  coordinates.set_account_name("devstoreaccount1");
  coordinates.set_container(container);
  coordinates.set_connection_string(env("GRPARSE_E2E_AZURE_CONNECTION_STRING"));
  coordinates.set_blob_prefix(prefix);
  return coordinates;
}

parsev1::GoogleCloudStorageCoordinates gcs(const std::string& bucket, const std::string& prefix,
                                           bool service_account) {
  parsev1::GoogleCloudStorageCoordinates coordinates;
  coordinates.set_bucket(bucket);
  coordinates.set_key_prefix(prefix);
  coordinates.set_endpoint(env("GRPARSE_E2E_GCS_ENDPOINT"));
  if (service_account) {
    auto* key = coordinates.mutable_service_account_key();
    key->set_project_id("e2e");
    key->set_private_key_id("e2e-kid");
    key->set_private_key(grparse_test::read_file_bytes(env("GRPARSE_E2E_SA_KEY")));
    key->set_client_email("e2e@e2e.iam.gserviceaccount.com");
    key->set_token_uri(env("GRPARSE_E2E_GOOGLE") + "/token");
  }
  return coordinates;
}

parsev1::GoogleDriveCoordinates drive(const std::string& id) {
  parsev1::GoogleDriveCoordinates coordinates;
  coordinates.set_path_id(id);
  coordinates.set_refresh_token("e2e-refresh-token");
  coordinates.set_api_endpoint(env("GRPARSE_E2E_GOOGLE"));
  auto* credentials = coordinates.mutable_credentials();
  credentials->set_client_id("e2e-client");
  credentials->set_client_secret("e2e-client-secret");
  credentials->set_token_uri(env("GRPARSE_E2E_GOOGLE") + "/token");
  return coordinates;
}

parsev1::FileSource resolved(const parsev1::Source& source, const std::string& label) {
  parsev1::FileSource file;
  const grpc::Status status = storage::resolve_source(source, "sources[0]", &file);
  require(status.ok(), label + " resolves: " + status.error_message());
  require_equal(file.filename(), std::string("sample.md"), label + " filename");
  require_equal(grparse::decode_base64(file.base64_string()), sample(), label + " bytes");
  std::println("source {}: {} ({} bytes)", label, file.filename(), sample().size());
  return file;
}

void expect_invalid(const parsev1::Source& source, const std::string& label) {
  parsev1::FileSource file;
  const grpc::Status status = storage::resolve_source(source, "sources[0]", &file);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          label + " is INVALID_ARGUMENT, got " + std::to_string(status.error_code()) + ": " +
              status.error_message());
  std::println("source {} refused: {}", label, status.error_message());
}

void sources() {
  parsev1::Source source;
  *source.mutable_azure_blob()->mutable_coordinates() = azure("grparse-in", "docs/");
  resolved(source, "azure_blob");
  source.Clear();
  *source.mutable_google_cloud_storage()->mutable_coordinates() = gcs("grparse-in", "docs/", false);
  resolved(source, "google_cloud_storage (anonymous emulator)");
  source.Clear();
  *source.mutable_google_cloud_storage()->mutable_coordinates() = gcs("grparse-in", "docs/", true);
  resolved(source, "google_cloud_storage (service account)");
  source.Clear();
  *source.mutable_google_drive()->mutable_coordinates() = drive(env("GRPARSE_E2E_DRIVE_FILE_ID"));
  resolved(source, "google_drive");

  source.Clear();
  *source.mutable_google_cloud_storage()->mutable_coordinates() = gcs("grparse-in", "nothing/", false);
  expect_invalid(source, "google_cloud_storage empty prefix");
  source.Clear();
  *source.mutable_azure_blob()->mutable_coordinates() = azure("grparse-in", "two/");
  expect_invalid(source, "azure_blob two objects");
  source.Clear();
  *source.mutable_google_drive()->mutable_coordinates() = drive("folder-out");
  expect_invalid(source, "google_drive folder");
  source.Clear();
  *source.mutable_google_drive()->mutable_coordinates() = drive("gdoc-1");
  expect_invalid(source, "google_drive native doc");
  source.Clear();
  auto coordinates = drive(env("GRPARSE_E2E_DRIVE_FILE_ID"));
  coordinates.set_refresh_token("wrong");
  *source.mutable_google_drive()->mutable_coordinates() = coordinates;
  parsev1::FileSource file;
  const grpc::Status refused = storage::resolve_source(source, "sources[0]", &file);
  require(refused.error_code() == grpc::StatusCode::UNAVAILABLE,
          "a refused refresh token is UNAVAILABLE: " + refused.error_message());
  std::println("source google_drive bad refresh token: {}", refused.error_message());
}

docv1::Document document() {
  docv1::Document document = grparse_test::base_document("sample.md");
  document.set_schema_name("pipestream_document_v1");
  grparse_test::add_paragraph(&document, "#/body", "storage connectors e2e");
  return document;
}

parsev1::DocumentExports exports() {
  parsev1::DocumentExports exports;
  exports.set_md("storage connectors e2e");
  return exports;
}

std::string strip(const std::string& key, const std::string& prefix) {
  require(key.starts_with(prefix), key + " starts with " + prefix);
  return key.substr(prefix.size());
}

std::map<std::string, std::string> deliver(const parsev1::Target& target, const std::string& label,
                                           const std::string& prefix) {
  parsev1::TargetResult result;
  const grpc::Status status = grparse::targets::deliver(target, document(), exports(), &result);
  require(status.ok(), label + " delivers: " + status.error_message());
  std::map<std::string, std::string> keys;
  for (const auto& object : result.objects()) {
    keys[strip(object.key(), prefix)] = object.store_id();
    std::println("target {}: {} {} bytes{}", label, object.key(), object.size_bytes(),
                 object.store_id().empty() ? "" : " id " + object.store_id());
  }
  return keys;
}

void read_back(storage::ObjectStore& store, const std::string& prefix,
               const std::vector<grparse::targets::BundleFile>& bundle, const std::string& label) {
  const auto names = store.list(prefix, std::nullopt);
  require_equal(names.size(), bundle.size(), label + " object count");
  for (const auto& file : bundle) {
    require_equal(store.get(prefix + file.path), file.bytes, label + " " + file.path + " bytes");
  }
  std::println("read back {}: {} objects match the bundle", label, bundle.size());
}

void targets() {
  const auto bundle = grparse::targets::build_bundle(document(), exports());
  std::set<std::string> expected;
  for (const auto& file : bundle) expected.insert(file.path);

  parsev1::Target target;
  *target.mutable_azure_blob()->mutable_coordinates() = azure("grparse-out", "run/");
  const auto azure_keys = deliver(target, "azure_blob", "run/");
  auto azure_store = storage::open_azure_blob(azure("grparse-out", "run/"), "t");
  read_back(*azure_store, "run/", bundle, "azure_blob");

  target.Clear();
  *target.mutable_google_cloud_storage()->mutable_coordinates() = gcs("grparse-out", "run/", true);
  const auto gcs_keys = deliver(target, "google_cloud_storage", "run/");
  auto gcs_store = storage::open_google_cloud_storage(gcs("grparse-out", "run/", false), "t");
  read_back(*gcs_store, "run/", bundle, "google_cloud_storage");

  target.Clear();
  *target.mutable_google_drive()->mutable_coordinates() = drive("folder-out");
  const auto drive_keys = deliver(target, "google_drive", "");
  grparse::storage::HttpRequest request;
  request.url = env("GRPARSE_E2E_GOOGLE") + "/_test/uploads";
  const auto reply = grparse::storage::http_call(request, "mock uploads");
  grparse::test::storage::MockDriveUploads uploads;
  require(google::protobuf::util::JsonStringToMessage(reply.body, &uploads).ok(),
          "the mock's uploads parse as MockDriveUploads");
  require_equal(uploads.uploads_size(), static_cast<int>(bundle.size()), "drive upload count");
  for (const auto& upload : uploads.uploads()) {
    const auto found = std::ranges::find_if(bundle, [&](const auto& f) { return f.path == upload.name(); });
    require(found != bundle.end(), "drive upload " + upload.name() + " is a bundle member");
    require_equal(upload.content(), found->bytes, "drive " + upload.name() + " bytes");
    require_equal(drive_keys.at(upload.name()), upload.id(), "drive store_id is the file id");
  }
  std::println("read back google_drive: {} uploads match the bundle", bundle.size());

  target.Clear();
  auto* s3 = target.mutable_s3();
  s3->set_endpoint(env("GRPARSE_E2E_S3_ENDPOINT"));
  s3->set_access_key(env("GRPARSE_E2E_S3_ACCESS_KEY"));
  s3->set_secret_key(env("GRPARSE_E2E_S3_SECRET_KEY"));
  s3->set_bucket("grparse-out");
  s3->set_key_prefix("run");
  s3->set_verify_ssl(false);
  s3->set_region("us-east-1");
  const auto s3_keys = deliver(target, "s3", "run/");
  {
    std::ofstream listing(env("GRPARSE_E2E_OUT_DIR") + "/s3-keys.txt");
    for (const auto& [key, id] : s3_keys) listing << "run/" << key << "\n";
  }

  const std::filesystem::path root = env("GRPARSE_E2E_OUT_DIR") + "/plugin";
  grparse::test::storage::DirectoryConnectorConfig config;
  config.set_root(root.string());
  target.Clear();
  target.mutable_plugin()->set_kind("test_directory");
  require(target.mutable_plugin()->mutable_config()->PackFrom(config), "pack");
  const auto plugin_keys = deliver(target, "plugin test_directory", "");
  for (const auto& file : bundle) {
    require_equal(grparse_test::read_file_bytes(root / file.path), file.bytes, "plugin " + file.path);
  }

  // The parity check: every connector lays the bundle out exactly as the S3
  // target does.
  const auto names = [](const std::map<std::string, std::string>& keys) {
    std::set<std::string> out;
    for (const auto& [key, id] : keys) out.insert(key);
    return out;
  };
  require(names(s3_keys) == expected, "s3 writes the bundle");
  require(names(azure_keys) == names(s3_keys), "azure_blob layout matches s3");
  require(names(gcs_keys) == names(s3_keys), "google_cloud_storage layout matches s3");
  require(names(drive_keys) == names(s3_keys), "google_drive layout matches s3");
  require(names(plugin_keys) == names(s3_keys), "plugin layout matches s3");
  std::println("parity: azure_blob, google_cloud_storage, google_drive and plugin write the "
               "same {} bundle members as s3", expected.size());
}

void plugin_source() {
  const std::filesystem::path root = env("GRPARSE_E2E_OUT_DIR") + "/plugin-in";
  std::filesystem::create_directories(root);
  std::ofstream(root / "sample.md", std::ios::binary) << sample();
  grparse::test::storage::DirectoryConnectorConfig config;
  config.set_root(root.string());
  config.set_name("sample.md");
  parsev1::Source source;
  source.mutable_plugin()->set_kind("test_directory");
  require(source.mutable_plugin()->mutable_config()->PackFrom(config), "pack");
  resolved(source, "plugin test_directory");
}

}  // namespace

int main() {
  if (std::getenv("GRPARSE_E2E_STORAGE") == nullptr) {
    std::println("storage-connectors-e2e-test: skipped (GRPARSE_E2E_STORAGE unset)");
    return 77;
  }
  ::setenv("GRPARSE_ALLOW_EXTERNAL_PLUGINS", "true", 1);
  ::setenv("GRPARSE_ALLOWED_SOURCE_TYPES", "file,azure_blob,google_cloud_storage,google_drive,test_directory", 1);
  ::setenv("GRPARSE_ALLOWED_TARGET_TYPES",
           "inbody,zip,s3,azure_blob,google_cloud_storage,google_drive,test_directory", 1);
  grparse_test::register_directory_plugin();
  return grparse_test::run_test_main("storage-connectors-e2e-test", "ok",
                                     {sources, plugin_source, targets});
}
