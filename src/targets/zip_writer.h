// A deterministic ZIP writer for bundle file sets.  The stock archivers are
// all wrong for this: they stamp the wall clock into every entry, so two
// archives of the same bytes differ.  This one fixes the timestamp, keeps the
// entries in the order the bundle hands them over, and holds the compressor
// to one setting, which is what lets a caller treat an archive's digest as an
// identity for its contents.
#ifndef GRPARSE_TARGETS_ZIP_WRITER_H
#define GRPARSE_TARGETS_ZIP_WRITER_H

#include <string>
#include <vector>

#include "bundle.h"

namespace grparse::targets {

// The ZIP archive of `files`, written in the given order.  Identical input
// yields byte-identical output.  Throws std::runtime_error when the archive
// would need ZIP64 (a member or the archive past 4 GiB, or past 65535
// members), which no conversion bundle reaches.
std::string write_zip(const std::vector<BundleFile>& files);

}  // namespace grparse::targets

#endif
