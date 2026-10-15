#include "connectors.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

#include "../source_parse.h"
#include "azure_blob.h"
#include "google_storage.h"
#include "grparse/base64.h"

namespace grparse::storage {
namespace {

namespace parsev1 = ai::pipestream::parse::v1;

const std::set<std::string>& builtin_source_kinds() {
  static const std::set<std::string> kinds = {"file", "azure_blob", "google_cloud_storage",
                                              "google_drive"};
  return kinds;
}

const std::set<std::string>& builtin_target_kinds() {
  static const std::set<std::string> kinds = {"inbody", "zip", "s3", "azure_blob",
                                              "google_cloud_storage", "google_drive"};
  return kinds;
}

// Every kind name the contract itself defines, which a plugin may not take.
const std::set<std::string>& reserved_kinds() {
  static const std::set<std::string> kinds = {"file", "http", "s3", "azure_blob",
                                              "google_cloud_storage", "google_drive", "inbody",
                                              "zip", "put", "presigned_url", "local_path",
                                              "plugin"};
  return kinds;
}

std::mutex& registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, ConnectorPlugin>& registry() {
  static std::map<std::string, ConnectorPlugin> plugins;
  return plugins;
}

std::optional<ConnectorPlugin> find_plugin(const std::string& kind) {
  std::lock_guard lock(registry_mutex());
  const auto found = registry().find(kind);
  if (found == registry().end()) return std::nullopt;
  return found->second;
}

std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::string();
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

std::optional<std::set<std::string>> listed(const char* variable) {
  const char* value = std::getenv(variable);
  if (value == nullptr || trimmed(value).empty()) return std::nullopt;
  std::set<std::string> kinds;
  std::string_view rest(value);
  while (true) {
    const size_t comma = rest.find(',');
    const std::string entry = trimmed(rest.substr(0, comma));
    if (!entry.empty()) kinds.insert(entry);
    if (comma == std::string_view::npos) break;
    rest = rest.substr(comma + 1);
  }
  return kinds;
}

bool external_plugins_allowed() {
  const char* value = std::getenv("GRPARSE_ALLOW_EXTERNAL_PLUGINS");
  return value != nullptr && trimmed(value) == "true";
}

grpc::Status check_kind(const std::string& kind, bool plugin, const std::string& option_path,
                        const char* variable, const std::set<std::string>& builtin,
                        const std::string& noun) {
  using parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED;
  if (plugin && !external_plugins_allowed()) {
    return option_rejected(option_path + ".kind", OPTION_REJECTION_REASON_NOT_ALLOWED, kind,
                           option_path + ": plugin " + noun + " '" + kind +
                               "' needs GRPARSE_ALLOW_EXTERNAL_PLUGINS=true");
  }
  const auto allowed = listed(variable);
  const bool ok = allowed.has_value() ? allowed->contains(kind) : (!plugin && builtin.contains(kind));
  if (!ok) {
    return option_rejected(plugin ? option_path + ".kind" : option_path,
                           OPTION_REJECTION_REASON_NOT_ALLOWED, kind,
                           option_path + ": " + noun + " kind '" + kind + "' is not allowed (" +
                               variable + (plugin ? "; plugin kinds must be listed explicitly)"
                                                  : ")"));
  }
  return grpc::Status::OK;
}

// The plugin and its config message, unpacked and type-checked.
std::expected<std::pair<ConnectorPlugin, std::unique_ptr<google::protobuf::Message>>, grpc::Status>
open_plugin(const std::string& kind, const google::protobuf::Any& config,
            const std::string& option_path) {
  using parsev1::OPTION_REJECTION_REASON_INVALID;
  using parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE;
  const auto plugin = find_plugin(kind);
  if (!plugin.has_value()) {
    return std::unexpected(option_rejected(option_path + ".kind", OPTION_REJECTION_REASON_UNKNOWN_VALUE,
                                           kind, option_path + ": no connector plugin '" + kind +
                                                     "' is registered"));
  }
  const google::protobuf::Message* prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(plugin->config_type);
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const std::string expected = std::string(plugin->config_type->full_name());
  const std::string_view type_url = config.type_url();
  const size_t slash = type_url.rfind('/');
  const std::string_view packed =
      slash == std::string_view::npos ? std::string_view() : type_url.substr(slash + 1);
  if (packed != expected || !config.UnpackTo(message.get())) {
    return std::unexpected(option_rejected(
        option_path + ".config", OPTION_REJECTION_REASON_INVALID, std::string(config.type_url()),
        option_path + ".config must pack " + expected + " for plugin '" + kind + "'"));
  }
  return std::make_pair(*plugin, std::move(message));
}

grpc::Status from_request_error(const StorageRequestError& error) {
  return option_rejected(error.option_path(), parsev1::OPTION_REJECTION_REASON_INVALID,
                         std::string(), error.what());
}

void report(const std::vector<WrittenObject>& objects, parsev1::TargetResult* result) {
  for (const WrittenObject& object : objects) {
    auto* stored = result->add_objects();
    stored->set_key(object.key);
    stored->set_etag(object.etag);
    stored->set_size_bytes(object.size_bytes);
    stored->set_store_id(object.store_id);
  }
}

}  // namespace

void register_connector_plugin(ConnectorPlugin plugin) {
  if (plugin.kind.empty() || reserved_kinds().contains(plugin.kind)) {
    throw std::invalid_argument("connector plugin kind '" + plugin.kind + "' is empty or reserved");
  }
  if (plugin.config_type == nullptr) {
    throw std::invalid_argument("connector plugin '" + plugin.kind + "' names no config type");
  }
  std::lock_guard lock(registry_mutex());
  if (!registry().emplace(plugin.kind, plugin).second) {
    throw std::invalid_argument("connector plugin '" + plugin.kind + "' is already registered");
  }
}

void unregister_connector_plugin(const std::string& kind) {
  std::lock_guard lock(registry_mutex());
  registry().erase(kind);
}

std::string source_kind(const parsev1::Source& source) {
  switch (source.source_case()) {
    case parsev1::Source::kFile: return "file";
    case parsev1::Source::kHttp: return "http";
    case parsev1::Source::kS3: return "s3";
    case parsev1::Source::kAzureBlob: return "azure_blob";
    case parsev1::Source::kGoogleCloudStorage: return "google_cloud_storage";
    case parsev1::Source::kGoogleDrive: return "google_drive";
    case parsev1::Source::kPlugin: return source.plugin().kind();
    case parsev1::Source::SOURCE_NOT_SET: return std::string();
  }
  return std::string();
}

std::string target_kind(const parsev1::Target& target) {
  switch (target.target_case()) {
    case parsev1::Target::kInbody: return "inbody";
    case parsev1::Target::kPut: return "put";
    case parsev1::Target::kS3: return "s3";
    case parsev1::Target::kZip: return "zip";
    case parsev1::Target::kPresignedUrl: return "presigned_url";
    case parsev1::Target::kAzureBlob: return "azure_blob";
    case parsev1::Target::kGoogleCloudStorage: return "google_cloud_storage";
    case parsev1::Target::kGoogleDrive: return "google_drive";
    case parsev1::Target::kPlugin: return target.plugin().kind();
    case parsev1::Target::TARGET_NOT_SET: return "inbody";
  }
  return std::string();
}

grpc::Status resolve_source(const parsev1::Source& source, const std::string& option_path,
                            parsev1::FileSource* resolved) {
  using parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED;
  const std::string kind = source_kind(source);
  if (source.source_case() == parsev1::Source::SOURCE_NOT_SET) {
    return option_rejected(option_path, parsev1::OPTION_REJECTION_REASON_INVALID, std::string(),
                           option_path + " names no source");
  }
  if (source.has_http() || source.has_s3()) {
    return option_rejected(option_path + "." + kind, OPTION_REJECTION_REASON_NOT_IMPLEMENTED, kind,
                           option_path + ": " + kind + " sources are not implemented by this "
                           "server");
  }
  const bool plugin = source.has_plugin();
  if (grpc::Status allowed = check_kind(kind, plugin, plugin ? option_path + ".plugin" : option_path,
                                        "GRPARSE_ALLOWED_SOURCE_TYPES", builtin_source_kinds(),
                                        "source");
      !allowed.ok()) {
    return allowed;
  }
  if (source.has_file()) {
    *resolved = source.file();
    return grpc::Status::OK;
  }
  try {
    FetchedDocument fetched;
    if (source.has_azure_blob()) {
      const auto& coordinates = source.azure_blob().coordinates();
      const std::string path = option_path + ".azure_blob.coordinates";
      auto store = open_azure_blob(coordinates, path);
      fetched = fetch_single(*store, coordinates.blob_prefix(),
                             coordinates.has_max_num_elements()
                                 ? std::optional<uint32_t>(coordinates.max_num_elements())
                                 : std::nullopt,
                             path + ".blob_prefix");
    } else if (source.has_google_cloud_storage()) {
      const auto& coordinates = source.google_cloud_storage().coordinates();
      const std::string path = option_path + ".google_cloud_storage.coordinates";
      auto store = open_google_cloud_storage(coordinates, path);
      fetched = fetch_single(*store, coordinates.key_prefix(),
                             coordinates.has_max_num_elements()
                                 ? std::optional<uint32_t>(coordinates.max_num_elements())
                                 : std::nullopt,
                             path + ".key_prefix");
    } else if (source.has_google_drive()) {
      fetched = fetch_google_drive_file(source.google_drive().coordinates(),
                                        option_path + ".google_drive.coordinates");
    } else if (plugin) {
      auto opened = open_plugin(kind, source.plugin().config(), option_path + ".plugin");
      if (!opened.has_value()) return opened.error();
      if (!opened->first.fetch) {
        return option_rejected(option_path + ".plugin.kind", OPTION_REJECTION_REASON_NOT_IMPLEMENTED,
                               kind, option_path + ": plugin '" + kind + "' serves no source");
      }
      fetched = opened->first.fetch(*opened->second);
    }
    resolved->set_filename(fetched.filename);
    resolved->set_base64_string(encode_base64(fetched.bytes.data(), fetched.bytes.size()));
    return grpc::Status::OK;
  } catch (const StorageRequestError& invalid) {
    return from_request_error(invalid);
  } catch (const std::exception& failed) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, failed.what());
  }
}

grpc::Status check_target_allowed(const parsev1::Target& target) {
  if (target.target_case() == parsev1::Target::TARGET_NOT_SET) return grpc::Status::OK;
  const bool plugin = target.has_plugin();
  return check_kind(target_kind(target), plugin, plugin ? "target.plugin" : "target",
                    "GRPARSE_ALLOWED_TARGET_TYPES", builtin_target_kinds(), "target");
}

grpc::Status deliver_to_store(const parsev1::Target& target,
                              const std::vector<targets::BundleFile>& files,
                              parsev1::TargetResult* result, bool* handled) {
  *handled = true;
  try {
    if (target.has_azure_blob()) {
      const auto& coordinates = target.azure_blob().coordinates();
      auto store = open_azure_blob(coordinates, "target.azure_blob.coordinates");
      report(write_bundle(*store, coordinates.blob_prefix(), files), result);
    } else if (target.has_google_cloud_storage()) {
      const auto& coordinates = target.google_cloud_storage().coordinates();
      auto store = open_google_cloud_storage(coordinates, "target.google_cloud_storage.coordinates");
      report(write_bundle(*store, coordinates.key_prefix(), files), result);
    } else if (target.has_google_drive()) {
      auto store = open_google_drive_folder(target.google_drive().coordinates(),
                                            "target.google_drive.coordinates");
      report(write_bundle(*store, std::string(), files), result);
    } else if (target.has_plugin()) {
      const std::string kind = target.plugin().kind();
      auto opened = open_plugin(kind, target.plugin().config(), "target.plugin");
      if (!opened.has_value()) return opened.error();
      if (!opened->first.deliver) {
        return option_rejected("target.plugin.kind",
                               parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED, kind,
                               "target: plugin '" + kind + "' serves no target");
      }
      report(opened->first.deliver(*opened->second, files), result);
    } else {
      *handled = false;
    }
  } catch (const StorageRequestError& invalid) {
    return from_request_error(invalid);
  } catch (const PartialWrite& failed) {
    report(failed.written(), result);
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, failed.what());
  } catch (const std::exception& failed) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, failed.what());
  }
  return grpc::Status::OK;
}

}  // namespace grparse::storage
