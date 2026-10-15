// The S3 and HTTP sources: each resolves to the one document it names.
#ifndef GRPARSE_STORAGE_REMOTE_SOURCES_H
#define GRPARSE_STORAGE_REMOTE_SOURCES_H

#include <cstdint>
#include <string>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "object_store.h"

namespace grparse::storage {

// The single object under source.key_prefix in source.bucket, read with
// ListObjectsV2 and GetObject signed by SigV4. verify_ssl is honoured: unset
// or true verifies the endpoint's certificate, only an explicit false skips
// it. Credentials follow the S3 target's rule (the request's pair, else the
// opt-in ambient identity). StorageRequestError naming the field for a
// request the caller must fix; std::runtime_error when the store fails.
FetchedDocument fetch_s3_source(const ai::pipestream::parse::v1::S3Source& source,
                                const std::string& option_path);

// The bytes at source.url (http or https, certificate always verified),
// with the request's headers, following up to 5 redirects within http and
// https. The filename comes from Content-Disposition, else the last
// segment of the URL the body came from (after redirects), else
// "document". A body past `max_bytes` fails. 4xx is a StorageRequestError
// on the url; 5xx and transport failures are std::runtime_error.
FetchedDocument fetch_http_source(const ai::pipestream::parse::v1::HttpSource& source,
                                  const std::string& option_path, uint64_t max_bytes);

// GRPARSE_HTTP_SOURCE_MAX_BYTES (bytes; default 268435456, 256 MiB).
uint64_t http_source_max_bytes();

// The filename a response names (exposed for tests): Content-Disposition's
// filename* or filename, else the last segment of the URL path,
// percent-decoded, else "document". Always a bare name, never a path.
std::string http_source_filename(const std::string& url, const std::string& content_disposition);

}  // namespace grparse::storage

#endif
