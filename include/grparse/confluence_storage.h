#pragma once

#include <string>

#include "grparse/collector_coordinator.h"

namespace grparse {

// The wiki storage dialect: XHTML with the macro elements of the ac: and ri:
// namespaces layered on top. It parses in process, like the CV path and
// unlike the collectors that are dialed over gRPC, because the input is a
// small well-formed XML body with a fixed construct set.

// The content type a storage body is served under.
inline constexpr char kConfluenceStorageMimetype[] =
    "application/vnd.atlassian.confluence.storage+xhtml";

// True when the filename suffix or the content type names the storage
// dialect. The suffixes are ".confluence" and ".storage.xhtml"; a bare
// ".xhtml" is plain markup and stays with the markup collector.
bool confluence_storage_format(const std::string& filename,
                               const std::string& content_type);

// Folds one storage body into a Document fragment: headings, paragraphs
// with their inline formatting and links, lists, tables, code and task
// macros, panels, and attachment pointers, every item stamped with this
// handler's CollectorSource. Never throws; a body that carries no markup at
// all becomes an INVALID_ARGUMENT outcome, and recoverable markup damage
// becomes a warning while the content already read is kept.
CollectorOutcome parse_confluence_storage(const std::string& bytes);

}  // namespace grparse
