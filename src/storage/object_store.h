// The object-store shape every storage connector shares: list names under a
// prefix, read one object, write one bundle member. Sources read exactly one
// document through it and targets write the result bundle through it, so
// Azure Blob, Google Cloud Storage, Google Drive and connector plugins share
// one source rule and one target rule.
#ifndef GRPARSE_STORAGE_OBJECT_STORE_H
#define GRPARSE_STORAGE_OBJECT_STORE_H

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../targets/bundle.h"

namespace grparse::storage {

// One object written.
struct WrittenObject {
  // The name it is stored under (prefix included).
  std::string key;
  std::string etag;
  uint64_t size_bytes = 0;
  // The store's own id when names are not ids (Google Drive file id).
  std::string store_id;
};

// The document a source resolved to.
struct FetchedDocument {
  std::string filename;
  std::string bytes;
};

// A refusal the caller can fix (wrong or incomplete coordinates, nothing or
// too much under the prefix): INVALID_ARGUMENT, with the option it is about.
class StorageRequestError : public std::runtime_error {
 public:
  StorageRequestError(std::string option_path, const std::string& message)
      : std::runtime_error(message), option_path_(std::move(option_path)) {}
  const std::string& option_path() const { return option_path_; }

 private:
  std::string option_path_;
};

// A write that failed after some members were written: the message names
// the member that failed; written() lists the objects already in the store.
class PartialWrite : public std::runtime_error {
 public:
  PartialWrite(const std::string& message, std::vector<WrittenObject> written)
      : std::runtime_error(message), written_(std::move(written)) {}
  const std::vector<WrittenObject>& written() const { return written_; }

 private:
  std::vector<WrittenObject> written_;
};

class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  // Object names under `prefix`, in the store's order, at most `limit`.
  virtual std::vector<std::string> list(const std::string& prefix,
                                        std::optional<uint32_t> limit) = 0;
  virtual std::string get(const std::string& name) = 0;
  // Writes `bytes` under `name` (already prefixed).
  virtual WrittenObject put(const std::string& name, const std::string& bytes) = 0;
};

// `prefix` and a bundle path joined by one '/'; an empty prefix adds nothing.
std::string join_prefix(const std::string& prefix, const std::string& path);

// The single document under `prefix`: the prefix may name the object itself
// or a "folder" holding exactly one object. Names ending in '/' (folder
// markers) do not count. At most 1000 names are listed (fewer when `limit`
// says so), so a prefix over a large store costs at most one page or two.
// StorageRequestError naming `option_path` when there
// is none, or more than one (gRParse converts one document per call).
FetchedDocument fetch_single(ObjectStore& store, const std::string& prefix,
                             std::optional<uint32_t> limit, const std::string& option_path);

// Writes every bundle member under `prefix` in bundle order. PartialWrite on
// the first failure.
std::vector<WrittenObject> write_bundle(ObjectStore& store, const std::string& prefix,
                                        const std::vector<targets::BundleFile>& files);

}  // namespace grparse::storage

#endif
