// The result bundle a Target delivers: one canonical file set folded out of a
// conversion.  The same set backs every target, so a ZIP archive and an
// object-store prefix hold byte-identical members, and the whole set is a
// pure function of the conversion's document, exports, status, errors and
// other outputs: no timestamps, no timings, no ordering that depends on a
// hash table's iteration, nothing that varies between two runs over the
// same input.
#ifndef GRPARSE_TARGETS_BUNDLE_H
#define GRPARSE_TARGETS_BUNDLE_H

#include <cstdint>
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

// The bundle's manifest schema (BundleManifest.schema_version).  Bumped
// whenever the manifest's own shape changes, never for a change in which
// files a conversion happens to produce.
inline constexpr uint32_t kManifestSchemaVersion = 1;

// The file set for `converted`, the response a conversion answers with:
//
//   manifest.pb              a BundleManifest: every member below with its
//                            SHA-256 and byte size, the generator and the
//                            schema version, deterministically serialized
//   manifest.json            the same message as JSON, for people
//   document.pb              the Document, deterministically serialized
//   document.json            the canonical JSON dialect of the same Document
//   response.pb              a ConvertDocumentResponse: the conversion's
//                            status, errors, chunks, structure findings,
//                            read quality and offset table, the source's
//                            filename, and the image issues below
//   exports/<name>.<ext>     one file per export the request asked for
//   pages/page_NNNN.<ext>    each page image the document embeds
//   pictures/pic_NNNN.<ext>  each picture image the document embeds
//
// response.pb is what a reader of the bucket has in place of the RPC
// response, so a partial conversion reads as one there too.  It repeats
// neither the document nor the exports, and it carries no timing.
//
// Page and picture images come from the data URIs the document already
// carries; a document that embeds none contributes none.  A member's
// extension is what its bytes' signature says, else what the document
// declares for it, and ".bin" when neither names a type the bundle knows.
// Every image the document carries that did not become a member of a known
// type (a reference with no bytes, a payload that does not decode, a type
// nothing names) is listed in result->image_issues, pages first, and the
// same way in response.pb: the bundle never leaves an image out without
// saying so.  The list replaces whatever result->image_issues held; nothing
// else of `result` is read or written, and converted.target_result is not
// read, so `result` may be that very field.
//
// The result is sorted by path, which is the order every target writes in.
std::vector<BundleFile> build_bundle(
    const ai::pipestream::parse::v1::ConvertDocumentResponse& converted,
    ai::pipestream::parse::v1::TargetResult* result);

}  // namespace grparse::targets

#endif
