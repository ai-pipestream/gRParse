// Azure Blob Storage over the REST API with Shared Key authorization
// (https://learn.microsoft.com/rest/api/storageservices/authorize-with-shared-key).
// The account key comes from the request's connection string and signs each
// request; it is never put into a message.
#ifndef GRPARSE_STORAGE_AZURE_BLOB_H
#define GRPARSE_STORAGE_AZURE_BLOB_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "object_store.h"

namespace grparse::storage {

// The parts of a connection string the connector uses.
struct AzureConnection {
  std::string account_name;
  // Decoded account key bytes.
  std::string account_key;
  // Blob service base URL without a trailing slash, for example
  // "https://acct.blob.core.windows.net" or, for an emulator,
  // "http://127.0.0.1:10000/devstoreaccount1".
  // An emulator's endpoint carries the account as its first path segment,
  // and the signed resource then repeats it after the leading account.
  std::string blob_endpoint;
};

// Parses and checks a connection string against `account_name`.
// StorageRequestError naming `option_path` on anything unusable.
AzureConnection parse_azure_connection_string(const std::string& connection_string,
                                              const std::string& account_name,
                                              const std::string& option_path);

// The Shared Key signature string for one request (exposed for the
// signing test vectors). `resource_path` is the URL path, already encoded.
std::string azure_string_to_sign(const std::string& method, size_t content_length,
                                 const std::string& content_type,
                                 const std::vector<std::pair<std::string, std::string>>& ms_headers,
                                 const std::string& account, const std::string& resource_path,
                                 const std::vector<std::pair<std::string, std::string>>& query);

std::unique_ptr<ObjectStore> open_azure_blob(
    const ai::pipestream::parse::v1::AzureBlobCoordinates& coordinates,
    const std::string& option_path);

}  // namespace grparse::storage

#endif
