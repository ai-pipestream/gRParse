// The fan-out that writes a bundle to an object store.  Uploads are network
// waits, not work, so they run on their own small pool rather than on the
// conversion executor: a bundle of a thousand page images must not occupy the
// workers that other conversions are queued behind.  The call still blocks
// until every object is written or the first one hard-fails, because the RPC
// cannot report objects that are not there yet.
#ifndef GRPARSE_TARGETS_S3_UPLOADER_H
#define GRPARSE_TARGETS_S3_UPLOADER_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
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

// A batch that failed after some members were already written: the message
// names the first key that failed, and written() lists the objects that are
// in the store regardless, in the bundle's own order, so the caller can
// clean up or retry.
class UploadFailure : public std::runtime_error {
 public:
  UploadFailure(const std::string& message, std::vector<UploadedObject> written)
      : std::runtime_error(message), written_(std::move(written)) {}
  const std::vector<UploadedObject>& written() const { return written_; }

 private:
  std::vector<UploadedObject> written_;
};

// Writes every member of `files` under the configured bucket and prefix and
// returns them in the bundle's own order.  Throws UploadFailure naming the
// first key that failed, with no credential material in the message; the
// uploads already in flight are waited out first, so nothing outlives the
// call.
std::vector<UploadedObject> upload_bundle(const S3Config& config,
                                          const std::vector<BundleFile>& files);

}  // namespace grparse::targets

#endif
