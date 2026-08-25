// The fan-out that writes a bundle to an object store.  Uploads are network
// waits, not work, so they run on their own small pool rather than on the
// conversion executor: a bundle of a thousand page images must not occupy the
// workers that other conversions are queued behind.  The call still blocks
// until every object is written or the first one hard-fails, because the RPC
// cannot report objects that are not there yet.
#ifndef GRPARSE_TARGETS_S3_UPLOADER_H
#define GRPARSE_TARGETS_S3_UPLOADER_H

#include <cstdint>
#include <string>
#include <vector>

#include "bundle.h"
#include "s3_client.h"

namespace grparse::targets {

// One written object, as the response reports it.
struct UploadedObject {
  std::string key;
  std::string etag;
  uint64_t size_bytes = 0;
};

// Writes every member of `files` under the configured bucket and prefix and
// returns them in the bundle's own order.  Throws std::runtime_error naming
// the first key that failed, with no credential material in the message; the
// uploads already in flight are waited out first, so nothing outlives the
// call.
std::vector<UploadedObject> upload_bundle(const S3Config& config,
                                          const std::vector<BundleFile>& files);

}  // namespace grparse::targets

#endif
