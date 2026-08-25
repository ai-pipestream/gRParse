// The result bundle a Target delivers: one canonical file set folded out of a
// converted document.  The same set backs every target, so a ZIP archive and
// an object-store prefix hold byte-identical members, and the whole set is a
// pure function of the document and its exports: no timestamps, no ordering
// that depends on a hash table's iteration, nothing that varies between two
// runs over the same input.
#ifndef GRPARSE_TARGETS_BUNDLE_H
#define GRPARSE_TARGETS_BUNDLE_H

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::targets {

// One member of the bundle.  `path` is the relative slash-separated name it
// carries in an archive and the suffix appended to an object key prefix.
struct BundleFile {
  std::string path;
  std::string bytes;
};

// The bundle's manifest schema.  Bumped whenever the manifest's own shape
// changes, never for a change in which files a conversion happens to
// produce.
inline constexpr int kManifestSchemaVersion = 1;

// The file set for `document` and the exports already rendered for it:
//
//   manifest.json          every other file with its SHA-256 and byte size
//   document.pb            the Document, deterministically serialized
//   document.json          the canonical JSON dialect of the same Document
//   exports/<name>.<ext>   one file per export the request asked for
//   pages/page_NNNN.png    each page image the document embeds
//   pictures/pic_NNNN.png  each picture image the document embeds
//
// Page and picture images come from the data URIs the document already
// carries; a document that embeds none contributes none.  The result is
// sorted by path, which is the order every target writes in.
std::vector<BundleFile> build_bundle(
    const ai::pipestream::document::v1::Document& document,
    const ai::pipestream::parse::v1::DocumentExports& exports);

}  // namespace grparse::targets

#endif
