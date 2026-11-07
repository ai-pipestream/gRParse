// The wiki storage dialect's name rules: which filename suffixes and which
// content type route a body to the storage handler.
#include "grparse/confluence_storage_format.h"

#include <string>

#include "confluence/storage_text.h"

namespace grparse {

bool confluence_storage_format(const std::string& filename,
                               const std::string& content_type) {
  const std::string name = confluence::lowercase(filename);
  const std::string type = confluence::lowercase(content_type);
  return confluence::ends_with(name, ".confluence") ||
         confluence::ends_with(name, ".storage.xhtml") ||
         type == kConfluenceStorageMimetype;
}

}  // namespace grparse
