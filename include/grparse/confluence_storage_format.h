#pragma once

#include <string>

namespace grparse {

// How a request names the wiki storage dialect: its content type and its
// filename suffixes. Split from confluence_storage.h so that the content
// sniff, which needs only the name rules, compiles without the Document
// protos or gRPC (the standalone fuzz project builds it that way).

// The content type a storage body is served under.
inline constexpr char kConfluenceStorageMimetype[] =
    "application/vnd.atlassian.confluence.storage+xhtml";

// True when the filename suffix or the content type names the storage
// dialect. The suffixes are ".confluence" and ".storage.xhtml"; a bare
// ".xhtml" is plain markup and stays with the markup collector.
bool confluence_storage_format(const std::string& filename,
                               const std::string& content_type);

}  // namespace grparse
