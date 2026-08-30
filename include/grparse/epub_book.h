#pragma once

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// The epub collector's Document is a skeleton by contract: one empty
// GROUP_LABEL_CHAPTER group per spine item (named by the chapter's archive
// path), one PictureItem per image resource pointing at the bytes by
// `epub:<href>` rather than carrying them, the outline, and the metadata.
// Chapter XHTML is deliberately not parsed there; the markup collector owns
// HTML. This module is the other half of that contract: it takes the
// chapters' own Documents (one per chapter, as the markup collector folds
// them) and the image bytes the epub stream carried, and plugs them into
// the skeleton so the book reads as one Document.

// One spine item as the epub stream carried it.
struct EpubChapter {
  // The resolved archive path, which is also the chapter group's name.
  std::string href;
  std::string media_type;
  std::string content;
};

// One image resource as the epub stream carried it.
struct EpubResource {
  std::string href;
  std::string media_type;
  std::string content;
};

// One chapter's parse, ready to fold: the chapter it came from and the
// Document the markup collector projected from its XHTML.
struct ParsedChapter {
  std::string href;
  ai::pipestream::document::v1::Document document;
};

// Resolves `reference` (an XHTML `src` or `href` attribute) against the
// archive path of the chapter it appears in, the way a reading system
// does: the fragment and query are dropped, percent-escapes decode, `.`
// and `..` segments collapse, and a leading `/` names the archive root.
// Returns an empty string for a reference that does not name an archive
// entry (a `data:` URI, an absolute `http(s)://` URL, a mailto:).
std::string resolve_epub_href(const std::string& chapter_href,
                              const std::string& reference);

// The largest image that is inlined as a data URI; a resource above this
// keeps its `epub:` reference and the fold says so in a warning, because a
// Document that carries a 60 MB TIFF inline serves nobody.
inline constexpr size_t kEpubInlineImageCap = 16 * 1024 * 1024;

// Plugs the chapters into `book`, in the order given (spine order), and
// carries the images:
//
// - every chapter picture whose `src` resolves to an archive entry is
//   re-pointed at `epub:<href>`; a body-level picture the skeleton emitted
//   for the same resource is retired (its manifest facts move onto the
//   chapter picture), because the chapter now says where the image sits;
// - each chapter Document's content merges into the book, and the items
//   its body held become the children of the chapter group named by its
//   href, so reading order is the spine order the groups already encode;
// - every remaining `epub:` reference whose bytes arrived becomes a
//   `data:<media type>;base64,...` URI, with the manifest's media type
//   winning over whatever the XHTML implied.
//
// A chapter whose group is missing lands at the body's end rather than
// being dropped; a resource that never arrived keeps its reference. Both
// are reported through `warnings`. Document-level identity in a chapter
// Document (origin, source metadata, claims, media, email, page styles) is
// discarded: a chapter's <title> is not the book's title.
void fold_epub_book(std::vector<ParsedChapter> chapters,
                    const std::vector<EpubResource>& resources,
                    ai::pipestream::document::v1::Document* book,
                    std::vector<std::string>* warnings);

}  // namespace grparse
