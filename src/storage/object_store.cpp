#include "object_store.h"

#include <algorithm>
#include <exception>

namespace grparse::storage {

std::string join_prefix(const std::string& prefix, const std::string& path) {
  if (prefix.empty()) return path;
  std::string key = prefix;
  while (!key.empty() && key.back() == '/') key.pop_back();
  if (key.empty()) return path;
  return key + "/" + path;
}

FetchedDocument fetch_single(ObjectStore& store, const std::string& prefix,
                             std::optional<uint32_t> limit, const std::string& option_path) {
  // Enough names to tell one object from several, with room for folder
  // markers; a prefix over a huge store is never paged through.
  constexpr uint32_t kMaxListed = 1000;
  std::vector<std::string> names = store.list(prefix, std::min(limit.value_or(kMaxListed), kMaxListed));
  std::erase_if(names, [](const std::string& name) { return name.empty() || name.ends_with('/'); });
  if (names.empty()) {
    throw StorageRequestError(option_path, option_path + ": no object under '" + prefix + "'");
  }
  if (names.size() > 1) {
    throw StorageRequestError(option_path,
                              option_path + ": " + std::to_string(names.size()) +
                                  (names.size() >= kMaxListed ? " or more" : "") +
                                  " objects under '" + prefix +
                                  "'; this endpoint converts one document per call, so the "
                                  "prefix must name exactly one object");
  }
  FetchedDocument fetched;
  const std::string& name = names.front();
  const size_t slash = name.find_last_of('/');
  fetched.filename = slash == std::string::npos ? name : name.substr(slash + 1);
  fetched.bytes = store.get(name);
  return fetched;
}

std::vector<WrittenObject> write_bundle(ObjectStore& store, const std::string& prefix,
                                        const std::vector<targets::BundleFile>& files) {
  std::vector<WrittenObject> written;
  written.reserve(files.size());
  for (const targets::BundleFile& file : files) {
    try {
      written.push_back(store.put(join_prefix(prefix, file.path), file.bytes));
    } catch (const StorageRequestError&) {
      throw;
    } catch (const std::exception& failed) {
      throw PartialWrite(failed.what(), std::move(written));
    }
  }
  return written;
}

}  // namespace grparse::storage
