#include "google_storage.h"

#include <random>
#include <stdexcept>

#include <google/protobuf/util/json_util.h>

#include "google_auth.h"
#include "http_call.h"
#include "storage_wire.pb.h"

namespace grparse::storage {
namespace {

namespace parsev1 = ai::pipestream::parse::v1;
namespace wire = grparse::storage::wire;

constexpr const char* kGcsScope = "https://www.googleapis.com/auth/devstorage.read_write";
constexpr const char* kDriveFolder = "application/vnd.google-apps.folder";

template <typename Message>
Message parse_reply(const HttpResponse& reply, const std::string& what) {
  Message message;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  if (!google::protobuf::util::JsonStringToMessage(reply.body, &message, options).ok()) {
    throw std::runtime_error(what + ": the reply is not a " + std::string(message.GetTypeName()));
  }
  return message;
}

std::string trimmed_base(std::string base) {
  while (base.ends_with('/')) base.pop_back();
  return base;
}

void require_http(const std::string& url, const std::string& option_path) {
  if (!url.starts_with("https://") && !url.starts_with("http://")) {
    throw StorageRequestError(option_path, option_path + " must be an http or https URL");
  }
}

class GcsStore final : public ObjectStore {
 public:
  GcsStore(std::string base, std::string bucket, std::string bearer)
      : base_(std::move(base)), bucket_(std::move(bucket)), bearer_(std::move(bearer)) {}

  std::vector<std::string> list(const std::string& prefix, std::optional<uint32_t> limit) override {
    std::vector<std::string> names;
    std::string page;
    do {
      std::string url = base_ + "/storage/v1/b/" + percent_encode(bucket_, false) +
                        "/o?prefix=" + percent_encode(prefix, false);
      if (limit.has_value()) url += "&maxResults=" + std::to_string(*limit);
      if (!page.empty()) url += "&pageToken=" + percent_encode(page, false);
      const HttpResponse reply = send("GET", url, "", "", "list objects");
      if (!reply.ok()) {
        throw std::runtime_error("google_cloud_storage: listing bucket '" + bucket_ +
                                 "' answered HTTP " + std::to_string(reply.status));
      }
      const auto listing = parse_reply<wire::GcsObjectList>(reply, "google_cloud_storage list");
      for (const wire::GcsObject& object : listing.items()) names.push_back(object.name());
      page = listing.next_page_token();
    } while (!page.empty() && (!limit.has_value() || names.size() < *limit));
    if (limit.has_value() && names.size() > *limit) names.resize(*limit);
    return names;
  }

  std::string get(const std::string& name) override {
    const HttpResponse reply =
        send("GET",
             base_ + "/storage/v1/b/" + percent_encode(bucket_, false) + "/o/" +
                 percent_encode(name, false) + "?alt=media",
             "", "", "get object");
    if (!reply.ok()) {
      throw std::runtime_error("google_cloud_storage: reading '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    return reply.body;
  }

  WrittenObject put(const std::string& name, const std::string& bytes) override {
    const HttpResponse reply =
        send("POST",
             base_ + "/upload/storage/v1/b/" + percent_encode(bucket_, false) +
                 "/o?uploadType=media&name=" + percent_encode(name, false),
             bytes, "application/octet-stream", "upload object");
    if (!reply.ok()) {
      throw std::runtime_error("google_cloud_storage: writing '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    const auto object = parse_reply<wire::GcsObject>(reply, "google_cloud_storage upload");
    return WrittenObject{.key = name, .etag = object.etag(), .size_bytes = bytes.size(),
                         .store_id = std::string()};
  }

 private:
  HttpResponse send(const std::string& method, const std::string& url, const std::string& body,
                    const std::string& content_type, const std::string& what) const {
    HttpRequest request;
    request.method = method;
    request.url = url;
    request.body = body;
    if (!content_type.empty()) request.headers.emplace_back("Content-Type", content_type);
    if (!bearer_.empty()) request.headers.emplace_back("Authorization", "Bearer " + bearer_);
    return http_call(request, "google_cloud_storage: " + what);
  }

  std::string base_;
  std::string bucket_;
  std::string bearer_;
};

std::string drive_base(const parsev1::GoogleDriveCoordinates& coordinates,
                       const std::string& option_path) {
  std::string base = coordinates.has_api_endpoint() ? coordinates.api_endpoint()
                                                    : std::string("https://www.googleapis.com");
  require_http(base, option_path + ".api_endpoint");
  return trimmed_base(base);
}

std::string drive_token(const parsev1::GoogleDriveCoordinates& coordinates,
                        const std::string& option_path) {
  if (coordinates.path_id().empty()) {
    throw StorageRequestError(option_path + ".path_id", option_path + ".path_id is required");
  }
  if (coordinates.refresh_token().empty()) {
    throw StorageRequestError(option_path + ".refresh_token",
                              option_path + ".refresh_token is required (token_path and "
                              "credentials_path name server files and are not accepted)");
  }
  const auto& credentials = coordinates.credentials();
  if (credentials.client_id().empty() || credentials.client_secret().empty() ||
      credentials.token_uri().empty()) {
    throw StorageRequestError(option_path + ".credentials",
                              option_path + ".credentials needs client_id, client_secret and "
                              "token_uri");
  }
  require_http(credentials.token_uri(), option_path + ".credentials.token_uri");
  return refresh_access_token(credentials, coordinates.refresh_token());
}

HttpResponse drive_send(const std::string& method, const std::string& url, const std::string& bearer,
                        const std::string& body, const std::string& content_type,
                        const std::string& what) {
  HttpRequest request;
  request.method = method;
  request.url = url;
  request.body = body;
  request.headers.emplace_back("Authorization", "Bearer " + bearer);
  if (!content_type.empty()) request.headers.emplace_back("Content-Type", content_type);
  return http_call(request, "google_drive: " + what);
}

class DriveFolderStore final : public ObjectStore {
 public:
  DriveFolderStore(std::string base, std::string folder, std::string bearer)
      : base_(std::move(base)), folder_(std::move(folder)), bearer_(std::move(bearer)) {}

  std::vector<std::string> list(const std::string&, std::optional<uint32_t>) override {
    throw std::logic_error("a Drive folder target is write-only");
  }
  std::string get(const std::string&) override {
    throw std::logic_error("a Drive folder target is write-only");
  }

  WrittenObject put(const std::string& name, const std::string& bytes) override {
    wire::DriveFile metadata;
    metadata.set_name(name);
    metadata.add_parents(folder_);
    std::string metadata_json;
    if (!google::protobuf::util::MessageToJsonString(metadata, &metadata_json).ok()) {
      throw std::runtime_error("google_drive: could not render the file metadata");
    }
    std::random_device random;
    const std::string boundary = "grparse-" + std::to_string(random()) + std::to_string(random());
    std::string body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                       metadata_json + "\r\n--" + boundary +
                       "\r\nContent-Type: application/octet-stream\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";
    const HttpResponse reply = drive_send(
        "POST",
        base_ + "/upload/drive/v3/files?uploadType=multipart&supportsAllDrives=true&fields=id,name,size",
        bearer_, body, "multipart/related; boundary=" + boundary, "upload file");
    if (!reply.ok()) {
      throw std::runtime_error("google_drive: writing '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    const auto file = parse_reply<wire::DriveFile>(reply, "google_drive upload");
    return WrittenObject{.key = name, .etag = std::string(), .size_bytes = bytes.size(),
                         .store_id = file.id()};
  }

 private:
  std::string base_;
  std::string folder_;
  std::string bearer_;
};

}  // namespace

std::unique_ptr<ObjectStore> open_google_cloud_storage(
    const parsev1::GoogleCloudStorageCoordinates& coordinates, const std::string& option_path) {
  if (coordinates.bucket().empty()) {
    throw StorageRequestError(option_path + ".bucket", option_path + ".bucket is required");
  }
  std::string base = coordinates.has_endpoint() ? coordinates.endpoint()
                                                : std::string("https://storage.googleapis.com");
  require_http(base, option_path + ".endpoint");
  std::string bearer;
  if (coordinates.has_service_account_key()) {
    const auto& key = coordinates.service_account_key();
    if (key.client_email().empty() || key.private_key().empty() || key.token_uri().empty()) {
      throw StorageRequestError(option_path + ".service_account_key",
                                option_path + ".service_account_key needs client_email, "
                                "private_key and token_uri");
    }
    require_http(key.token_uri(), option_path + ".service_account_key.token_uri");
    try {
      bearer = service_account_access_token(key, kGcsScope);
    } catch (const std::invalid_argument& bad_key) {
      throw StorageRequestError(option_path + ".service_account_key.private_key",
                                option_path + ".service_account_key: " + bad_key.what());
    }
  } else if (!coordinates.has_endpoint()) {
    throw StorageRequestError(option_path + ".service_account_key",
                              option_path + ": without service_account_key only an explicit "
                              "endpoint (an emulator) is reached anonymously");
  }
  return std::make_unique<GcsStore>(trimmed_base(base), coordinates.bucket(), bearer);
}

FetchedDocument fetch_google_drive_file(const parsev1::GoogleDriveCoordinates& coordinates,
                                        const std::string& option_path) {
  const std::string base = drive_base(coordinates, option_path);
  const std::string bearer = drive_token(coordinates, option_path);
  const std::string file_url = base + "/drive/v3/files/" + percent_encode(coordinates.path_id(), false);
  const HttpResponse meta = drive_send(
      "GET", file_url + "?fields=id,name,mimeType,size&supportsAllDrives=true", bearer, "", "",
      "file metadata");
  if (!meta.ok()) {
    throw std::runtime_error("google_drive: file '" + coordinates.path_id() + "' answered HTTP " +
                             std::to_string(meta.status));
  }
  const auto file = parse_reply<wire::DriveFile>(meta, "google_drive file");
  if (file.mime_type() == kDriveFolder) {
    throw StorageRequestError(option_path + ".path_id",
                              option_path + ".path_id names a folder; a Google Drive source "
                              "converts one file (docling-serve accepts folders in batch only)");
  }
  if (file.mime_type().starts_with("application/vnd.google-apps.")) {
    throw StorageRequestError(option_path + ".path_id",
                              option_path + ".path_id names a native Google document (" +
                                  file.mime_type() + "), which has no file bytes to download");
  }
  const HttpResponse media =
      drive_send("GET", file_url + "?alt=media&supportsAllDrives=true", bearer, "", "", "download");
  if (!media.ok()) {
    throw std::runtime_error("google_drive: downloading '" + coordinates.path_id() +
                             "' answered HTTP " + std::to_string(media.status));
  }
  return FetchedDocument{.filename = file.name(), .bytes = media.body};
}

std::unique_ptr<ObjectStore> open_google_drive_folder(
    const parsev1::GoogleDriveCoordinates& coordinates, const std::string& option_path) {
  const std::string base = drive_base(coordinates, option_path);
  return std::make_unique<DriveFolderStore>(base, coordinates.path_id(),
                                            drive_token(coordinates, option_path));
}

}  // namespace grparse::storage
