// The mimetype ladder, rung by rung, as a table: every magic signature the
// sniffer knows with the bytes that trigger it, every extension the name
// table maps, and the evidence each resolution rests on.  The point is to
// pin the ladder itself, so a rewrite of the sniffer into a data table has a
// fixed set of inputs and answers to hold to.  Signature coverage against the
// real corpus lives in content_sniff_test.cpp; precedence findings from the
// bucket eval live in content_sniff_precedence_test.cpp.

#include <cstdint>
#include <string>
#include <vector>

#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"
#include "support/check.h"

using grparse_test::require;
using grparse_test::require_equal;

namespace {

// One rung: the leading bytes and the mimetype they declare.
struct SniffCase {
  std::string bytes;
  std::string mimetype;
  std::string what;
};

// A zip whose first local entry is an uncompressed "mimetype" file, the
// OpenDocument and EPUB convention the sniffer reads before anything else.
std::string zip_with_mimetype_entry(const std::string& mimetype) {
  std::string zip = "PK\x03\x04";
  zip.append(4, '\0');                                // version, flags
  zip.append(2, '\0');                                // method 0 (stored)
  zip.append(8, '\0');                                // time, date, crc
  const auto le32 = [&zip](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      zip.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  };
  const auto le16 = [&zip](std::uint16_t value) {
    zip.push_back(static_cast<char>(value & 0xff));
    zip.push_back(static_cast<char>((value >> 8) & 0xff));
  };
  le32(static_cast<std::uint32_t>(mimetype.size()));  // compressed size
  le32(static_cast<std::uint32_t>(mimetype.size()));  // uncompressed size
  le16(8);                                            // name length
  le16(0);                                            // extra length
  zip.append("mimetype");
  zip.append(mimetype);
  return zip;
}

// A zip that names one package part in its head, which is how the sniffer
// tells the OOXML families apart.
std::string zip_naming(const std::string& entry) {
  std::string zip = "PK\x03\x04";
  zip.append(26, '\0');
  zip.append(entry);
  return zip;
}

void verify_every_container_signature() {
  const SniffCase cases[] = {
      {zip_with_mimetype_entry("application/vnd.oasis.opendocument.text"),
       "application/vnd.oasis.opendocument.text",
       "a stored mimetype entry is the archive's own declaration"},
      {zip_with_mimetype_entry("application/epub+zip"), "application/epub+zip",
       "an epub declares itself the same way"},
      {zip_naming("word/document.xml"),
       "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
       "the word package part names a docx"},
      {zip_naming("xl/workbook.xml"),
       "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
       "the workbook part names an xlsx"},
      {zip_naming("ppt/presentation.xml"),
       "application/vnd.openxmlformats-officedocument.presentationml.presentation",
       "the presentation part names a pptx"},
      {zip_naming("META-INF/container.xml"), "application/epub+zip",
       "an epub without a mimetype entry is named by its container"},
      {zip_naming("some/other/entry.txt"), "application/zip",
       "a zip that names no known part is a plain archive"},
  };
  for (const SniffCase& one : cases) {
    require_equal(grparse::sniff_mimetype(one.bytes), one.mimetype, one.what);
  }
}

void verify_every_binary_signature() {
  const SniffCase cases[] = {
      {"%PDF-1.7\n", "application/pdf", "the PDF header"},
      {std::string("\x89PNG\r\n\x1a\n", 8), "image/png", "the PNG signature"},
      {std::string("\xff\xd8\xff\xe0", 4), "image/jpeg", "the JPEG start of image"},
      {"GIF87a", "image/gif", "the older GIF signature"},
      {"GIF89a", "image/gif", "the newer GIF signature"},
      {std::string("II*\0", 4), "image/tiff", "little-endian TIFF"},
      {std::string("MM\0*", 4), "image/tiff", "big-endian TIFF"},
      {std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16), "image/webp",
       "WebP inside a RIFF container"},
      {std::string("BM\x76\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00", 14), "image/bmp",
       "the BMP signature, which needs a full header to count"},
      {std::string("\x1f\x8b\x08", 3), "application/gzip", "the gzip signature"},
      {"WARC/1.1\r\n", "application/warc", "the WARC record header"},
      {"{\\rtf1\\ansi", "application/rtf", "the RTF header"},
      {"%!PS-Adobe-3.0", "application/postscript", "the PostScript header"},
  };
  for (const SniffCase& one : cases) {
    require_equal(grparse::sniff_mimetype(one.bytes), one.mimetype, one.what);
  }
}

void verify_the_signatures_that_deliberately_say_nothing() {
  require_equal(grparse::sniff_mimetype(""), "", "empty bytes declare nothing");
  require_equal(grparse::sniff_mimetype(std::string("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)), "",
                "a compound-file container does not say which office format it holds");
  require_equal(grparse::sniff_mimetype(std::string("\x00\x01\x02\x03", 4)), "",
                "arbitrary binary declares nothing");
  require_equal(grparse::sniff_mimetype(std::string("RIFF\x10\x00\x00\x00WAVEfmt ", 16)), "",
                "a RIFF container that is not WebP falls through the raster rung");
  require_equal(grparse::sniff_mimetype("BM"), "text/plain",
                "a BMP signature without its header is not a raster, and those two bytes are "
                "valid text, so the text rung answers instead");
}

void verify_every_text_signature() {
  const SniffCase cases[] = {
      {"<!DOCTYPE html><html></html>", "text/html", "an HTML doctype"},
      {"<!doctype HTML>", "text/html", "the doctype is matched without case"},
      {"<html lang=\"en\">", "text/html", "a leading html tag"},
      {"\xef\xbb\xbf   <html>", "text/html", "a byte order mark and whitespace are skipped"},
      {"<?xml version=\"1.0\"?><root/>", "application/xml", "an XML declaration"},
      {"<?xml version=\"1.0\"?><html xmlns=\"\"/>", "application/xhtml+xml",
       "an html root behind the declaration is XHTML"},
      {"<?xml version=\"1.0\"?><svg xmlns=\"\"/>", "image/svg+xml",
       "an svg root behind the declaration is SVG"},
      {"<svg viewBox=\"0 0 1 1\"/>", "image/svg+xml", "a leading svg tag"},
      {"<div><body>x</body></div>", "text/html", "a fragment that names a body tag"},
      {"From: a@b\r\nTo: c@d\r\n\r\nbody", "message/rfc822", "two RFC 822 header lines"},
      {"{\"a\": 1}", "application/json", "a bracketed object"},
      {"[1, 2, 3]\n", "application/json", "a bracketed array"},
      {"# Heading\n\ntext\n", "text/markdown", "a markdown heading"},
      {"---\ntitle: x\n", "text/markdown", "front matter"},
      {"```\ncode\n```\n", "text/markdown", "a fenced block"},
      {"| a | b |\n|---|---|\n", "text/markdown", "a table rule"},
      {"see [the docs](http://x/)\n", "text/markdown", "an inline link"},
      {"- one\n- two\n", "text/markdown", "two bullet lines"},
      {"1. one\n2. two\n", "text/markdown", "two numbered lines"},
      {"Just prose.\n", "text/plain", "text with no markers at all"},
      {"caf\xc3\xa9 au lait\n", "text/plain", "valid UTF-8 beyond ASCII is still text"},
  };
  for (const SniffCase& one : cases) {
    require_equal(grparse::sniff_mimetype(one.bytes), one.mimetype, one.what);
  }
}

void verify_a_body_that_is_not_text_falls_off_the_ladder() {
  require_equal(grparse::sniff_mimetype(std::string("hello\0world", 11)), "",
                "a NUL byte disqualifies a body from the text rungs");
  require_equal(grparse::sniff_mimetype("\x07\x07"), "",
                "a control byte outside the whitespace family disqualifies a body");
  require_equal(grparse::sniff_mimetype("\xc3\x28"), "",
                "a malformed UTF-8 sequence disqualifies a body");
  require_equal(grparse::sniff_mimetype("   \n\t  "), "",
                "a body of nothing but whitespace declares nothing");
  require_equal(grparse::sniff_mimetype("{\"a\": 1"), "text/plain",
                "an unterminated object is not JSON, only text");
}

void verify_the_extension_table() {
  const struct {
    std::string name;
    std::string mimetype;
  } cases[] = {
      {"a.pdf", "application/pdf"},
      {"a.jpg", "image/jpeg"},
      {"a.jpeg", "image/jpeg"},
      {"a.tif", "image/tiff"},
      {"a.tiff", "image/tiff"},
      {"a.png", "image/png"},
      {"a.gif", "image/gif"},
      {"a.webp", "image/webp"},
      {"a.bmp", "image/bmp"},
      {"a.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {"a.xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"a.pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
      {"a.odt", "application/vnd.oasis.opendocument.text"},
      {"a.ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"a.odp", "application/vnd.oasis.opendocument.presentation"},
      {"a.doc", "application/msword"},
      {"a.xls", "application/vnd.ms-excel"},
      {"a.ppt", "application/vnd.ms-powerpoint"},
      {"a.csv", "text/csv"},
      {"a.rtf", "application/rtf"},
      {"a.epub", "application/epub+zip"},
      {"a.eml", "message/rfc822"},
      {"a.msg", "application/vnd.ms-outlook"},
      {"a.xml", "application/xml"},
      {"a.nxml", "application/xml"},
      {"a.xbrl", "application/xml"},
      {"a.html", "text/html"},
      {"a.htm", "text/html"},
      {"a.xhtml", "application/xhtml+xml"},
      {"a.md", "text/markdown"},
      {"a.markdown", "text/markdown"},
      {"a.txt", "text/plain"},
      {"a.json", "application/json"},
      {"a.warc", "application/warc"},
      {"a.mp3", "audio/mpeg"},
      {"a.wav", "audio/wav"},
      {"a.m4a", "audio/mp4"},
      {"a.flac", "audio/flac"},
      {"a.ogg", "audio/ogg"},
      {"a.oga", "audio/ogg"},
      {"a.opus", "audio/ogg"},
      {"a.mp4", "video/mp4"},
      {"a.m4v", "video/mp4"},
      {"a.mkv", "video/x-matroska"},
      {"a.webm", "video/webm"},
      {"a.mov", "video/quicktime"},
  };
  for (const auto& one : cases) {
    require_equal(grparse::extension_mimetype(one.name), one.mimetype,
                  one.name + " maps by its extension");
    require_equal(grparse::extension_mimetype("A" + one.name.substr(1)), one.mimetype,
                  one.name + " maps whatever the case of its stem");
  }
}

void verify_the_extension_table_is_case_insensitive_and_closed() {
  require_equal(grparse::extension_mimetype("REPORT.PDF"), "application/pdf",
                "an upper-case extension maps like its lower-case spelling");
  require_equal(grparse::extension_mimetype("archive.tar.gz"), "application/octet-stream",
                "an extension the table does not name never masquerades as anything");
  require_equal(grparse::extension_mimetype("noextension"), "application/octet-stream",
                "a name with no extension falls back");
  require_equal(grparse::extension_mimetype("a.vtt"), "application/octet-stream",
                "the subtitle extension is not in the table, whatever the resolver's "
                "comment lists");
  require_equal(grparse::extension_mimetype("page.storage.xhtml"),
                grparse::kConfluenceStorageMimetype,
                "the wiki storage suffix outranks the plain xhtml extension");
  require_equal(grparse::extension_mimetype("page.confluence"),
                grparse::kConfluenceStorageMimetype,
                "the other wiki storage suffix does too");
}

void verify_the_evidence_names_the_rung_that_answered() {
  const struct {
    std::string declared;
    std::string bytes;
    std::string name;
    std::string mimetype;
    std::string evidence;
    std::string what;
  } cases[] = {
      {"application/pdf", "not really a pdf", "x.txt", "application/pdf", "declared",
       "an explicit request content type wins outright"},
      {"text/html; charset=utf-8", "x", "x.bin", "text/html", "declared",
       "a declared type is trimmed of its parameters"},
      {"application/octet-stream", "%PDF-1.4", "x.bin", "application/pdf", "magic",
       "the generic declared type steps aside for the bytes"},
      {"   ", "%PDF-1.4", "x.bin", "application/pdf", "magic",
       "a blank declared type steps aside too"},
      {"", "%PDF-1.4", "x.docx", "application/pdf", "magic",
       "the bytes outrank the name"},
      {"", "\xff\xfe", "report.docx",
       "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "extension",
       "bytes that say nothing leave the name to answer"},
      {"", "\xff\xfe", "blob", "application/octet-stream", "fallback",
       "nothing to go on ends at the fallback"},
      {"", "<p>markup</p>", "page.storage.xhtml", grparse::kConfluenceStorageMimetype,
       "extension", "the wiki storage suffix is a declaration a sniff cannot see"},
  };
  for (const auto& one : cases) {
    const grparse::MimetypeResolution got =
        grparse::resolve_mimetype(one.declared, one.bytes, one.name);
    require_equal(got.mimetype, one.mimetype, one.what);
    require_equal(got.evidence, one.evidence, one.what + " (evidence)");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("content-sniff-ladder-test", "ok", {
      verify_every_container_signature,
      verify_every_binary_signature,
      verify_the_signatures_that_deliberately_say_nothing,
      verify_every_text_signature,
      verify_a_body_that_is_not_text_falls_off_the_ladder,
      verify_the_extension_table,
      verify_the_extension_table_is_case_insensitive_and_closed,
      verify_the_evidence_names_the_rung_that_answered,
  });
}
