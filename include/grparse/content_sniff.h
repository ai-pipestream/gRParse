#pragma once

#include <filesystem>
#include <optional>
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
// body, Markdown by its markers, comma or semicolon separated values by a
// steady field count over three or more lines, and text/plain for any other
// text (UTF-8, or a single-byte encoding such as Latin-1). A compound file
// (OLE2) is placed by its root streams: WordDocument, Workbook and
// PowerPoint Document name .doc, .xls and .ppt, and an EncryptedPackage
// stream gets kEncryptedOfficePackageMimetype. Any other compound file (an
// Outlook .msg) returns empty and its extension decides.
std::string sniff_mimetype(std::string_view bytes);

// The type of an encrypted Office Open XML package: an OLE compound file
// whose EncryptedPackage stream hides which of docx, xlsx or pptx it is
// until the office collector decrypts it. Routes to the office collector.
inline constexpr std::string_view kEncryptedOfficePackageMimetype =
    "application/x-tika-ooxml-protected";

// Why an Office document in an OLE compound file is password-protected: the
// family the evidence belongs to ("Office Open XML package", "Word document",
// "Excel workbook", "PowerPoint presentation") and the structure that says so.
struct EncryptedOfficeDocument {
  std::string format;
  std::string evidence;
};

// The evidence that a document is a password-protected Office document, when
// it is one. The bytes must be an OLE compound file; its directory is walked
// from the root rather than searched for a name, so the evidence has to be
// where Office puts it:
//   - an "EncryptedPackage" stream directly under the root (an encrypted
//     .docx/.xlsx/.pptx, which Office wraps in a compound file);
//   - a "WordDocument" stream whose FIB sets fEncrypted (an encrypted .doc);
//   - a "Workbook" or "Book" stream opening with a FILEPASS record (an
//     encrypted .xls; a workbook saved under Excel's default password
//     VelvetSweatshop carries the same record and is reported the same way);
//   - a "Current User" stream with the encrypted header token beside a
//     "PowerPoint Document" stream (an encrypted .ppt).
// A legacy document that merely embeds an encrypted package (a .doc with an
// encrypted workbook object under ObjectPool) is not encrypted and gets
// nullopt. No collector can open any of these without the password, so the
// caller says so instead of reporting a load failure.
std::optional<EncryptedOfficeDocument> encrypted_office_document(std::string_view bytes);

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
