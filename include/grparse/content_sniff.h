#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace grparse {

// The mimetype the leading bytes of a document declare, by container
// signature or leading content; empty when nothing is recognized. Covers
// the zip-based office families (OpenDocument and EPUB by their stored
// "mimetype" entry, OOXML by the package part the entry names name), PDF,
// the raster signatures (PNG, JPEG, GIF, TIFF, WebP, BMP), gzip, WARC, RTF,
// PostScript, and for text: HTML by a leading doctype or html tag after
// whitespace or a BOM, XML by its declaration (XHTML and SVG by their root
// element behind it), mail by RFC 822 header lines, JSON by a bracketed
// body, Markdown by its markers, and text/plain for any other valid text.
// Compound-file (OLE2) containers return empty on purpose: the signature
// does not say whether the payload is a .doc, .xls, .ppt, or .msg, and the
// extension does.
std::string sniff_mimetype(std::string_view bytes);

// The mimetype a filename extension implies; "application/octet-stream"
// when the extension is unknown. Extension only; never reads bytes.
std::string extension_mimetype(const std::filesystem::path& filename);

// One resolved mimetype and what it rests on: "declared" (the request's
// content type), "magic" (sniff_mimetype), "extension", or "fallback"
// (application/octet-stream). The order is the contract: an explicit
// request content type wins, then the bytes, then the name. The one name
// that precedes the bytes is the wiki storage suffix (".storage.xhtml"),
// a dialect declaration a sniff cannot see.
struct MimetypeResolution {
  std::string mimetype;
  std::string evidence;
};

MimetypeResolution resolve_mimetype(std::string_view declared_content_type,
                                    std::string_view bytes,
                                    const std::filesystem::path& filename);

}  // namespace grparse
