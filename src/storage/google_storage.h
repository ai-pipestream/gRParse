// Google Cloud Storage (JSON API) and Google Drive (v3) as object stores.
#ifndef GRPARSE_STORAGE_GOOGLE_STORAGE_H
#define GRPARSE_STORAGE_GOOGLE_STORAGE_H

#include <memory>
#include <string>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "object_store.h"

namespace grparse::storage {

std::unique_ptr<ObjectStore> open_google_cloud_storage(
    const ai::pipestream::parse::v1::GoogleCloudStorageCoordinates& coordinates,
    const std::string& option_path);

// Drive has ids, not paths: a source reads the one file path_id names, and
// a target writes each bundle member into the folder path_id names.
FetchedDocument fetch_google_drive_file(
    const ai::pipestream::parse::v1::GoogleDriveCoordinates& coordinates,
    const std::string& option_path);
std::unique_ptr<ObjectStore> open_google_drive_folder(
    const ai::pipestream::parse::v1::GoogleDriveCoordinates& coordinates,
    const std::string& option_path);

}  // namespace grparse::storage

#endif
