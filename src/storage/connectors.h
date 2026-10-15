// The storage connectors behind Source and Target: the built-in Azure Blob,
// Google Cloud Storage and Google Drive connectors, the registry of
// connector plugins, and the docling-serve source and target policy
// (allowed_source_types, allowed_target_types, allow_external_plugins).
//
// Policy, read from the environment per call:
//   GRPARSE_ALLOWED_SOURCE_TYPES  comma-separated source kinds; unset means
//                                 the built-in kinds (file, http, s3,
//                                 azure_blob, google_cloud_storage,
//                                 google_drive).
//   GRPARSE_ALLOWED_TARGET_TYPES  comma-separated target kinds; unset means
//                                 the built-in kinds (inbody, zip, s3,
//                                 azure_blob, google_cloud_storage,
//                                 google_drive).
//   GRPARSE_ALLOW_EXTERNAL_PLUGINS  "true" enables plugin kinds, which must
//                                 also be listed explicitly above.
//   GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS  "1" lets requests name loopback
//                                 and private-network endpoints and http
//                                 token endpoints (see http_call.h).
//   GRPARSE_SOURCE_MAX_BYTES      largest document a source downloads.
#ifndef GRPARSE_STORAGE_CONNECTORS_H
#define GRPARSE_STORAGE_CONNECTORS_H

#include <functional>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "object_store.h"

namespace grparse::storage {

// One connector plugin: a kind, the protobuf message its config must be,
// and what it serves. Either function may be empty when the plugin serves
// only sources or only targets.
struct ConnectorPlugin {
  std::string kind;
  const google::protobuf::Descriptor* config_type = nullptr;
  std::function<FetchedDocument(const google::protobuf::Message& config)> fetch;
  std::function<std::vector<WrittenObject>(const google::protobuf::Message& config,
                                           const std::vector<targets::BundleFile>& files)>
      deliver;
};

// Registers a plugin for the life of the process. std::invalid_argument on
// an empty or built-in kind, a kind already registered, or no config type.
void register_connector_plugin(ConnectorPlugin plugin);
// Removes a plugin (tests).
void unregister_connector_plugin(const std::string& kind);

// The source kind name docling-serve uses for a Source ("file",
// "azure_blob", ..., or the plugin's kind).
std::string source_kind(const ai::pipestream::parse::v1::Source& source);
std::string target_kind(const ai::pipestream::parse::v1::Target& target);

// Resolves `source` (at `option_path`, for example "sources[0]") to the
// file it names, applying the source policy. A FileSource passes through
// unchanged. INVALID_ARGUMENT with a typed OptionRejection for a refused
// kind or bad coordinates; UNAVAILABLE when the store failed.
grpc::Status resolve_source(const ai::pipestream::parse::v1::Source& source,
                            const std::string& option_path,
                            ai::pipestream::parse::v1::FileSource* resolved);

// The target policy check alone, for a request's target before the
// conversion runs.
grpc::Status check_target_allowed(const ai::pipestream::parse::v1::Target& target);

// Delivers the bundle to an Azure Blob, Google Cloud Storage, Google Drive
// or plugin target. `handled` is false for the other kinds.
grpc::Status deliver_to_store(const ai::pipestream::parse::v1::Target& target,
                              const std::vector<targets::BundleFile>& files,
                              ai::pipestream::parse::v1::TargetResult* result, bool* handled);

}  // namespace grparse::storage

#endif
