#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_sniff(const std::string& bytes, const std::string& expected,
                   const std::string& what) {
  const std::string got = grparse::sniff_mimetype(bytes);
  if (got != expected) {
    throw std::runtime_error(what + ": expected '" + expected + "', sniffed '" + got + "'");
  }
}

std::string le16(unsigned value) {
  return {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
}

std::string le32(unsigned long value) {
  return le16(static_cast<unsigned>(value & 0xFFFF)) +
         le16(static_cast<unsigned>((value >> 16) & 0xFFFF));
}

// A zip whose first local entry is the stored "mimetype" file, the
// OpenDocument and EPUB layout.
std::string zip_with_mimetype_entry(const std::string& mime) {
  std::string zip = "PK\x03\x04";
  zip += le16(20);          // version needed
  zip += le16(0);           // flags
  zip += le16(0);           // method: stored
  zip += le16(0) + le16(0); // time, date
  zip += le32(0);           // crc
  zip += le32(mime.size()); // compressed size
  zip += le32(mime.size()); // uncompressed size
  zip += le16(8);           // name length
  zip += le16(0);           // extra length
  zip += "mimetype";
  zip += mime;
  zip += std::string(64, '\0');
  return zip;
}

// A zip whose central directory (at the tail) names one package part.
std::string zip_naming(const std::string& entry) {
  std::string zip = "PK\x03\x04";
  zip += le16(20) + le16(8) + le16(8) + le16(0) + le16(0) + le32(0) + le32(0) + le32(0);
  zip += le16(9) + le16(0);
  zip += "_rels/.re";
  zip += std::string(70000, 'x');  // data far past the head window
  zip += "PK\x01\x02";
  zip += entry;
  zip += "PK\x05\x06";
  return zip;
}

void verify_container_signatures() {
  require_sniff(zip_with_mimetype_entry("application/vnd.oasis.opendocument.text"),
                "application/vnd.oasis.opendocument.text", "odt by mimetype entry");
  require_sniff(zip_with_mimetype_entry("application/epub+zip"), "application/epub+zip",
                "epub by mimetype entry");
  require_sniff(zip_naming("word/document.xml"),
                "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                "docx by package part");
  require_sniff(zip_naming("xl/workbook.xml"),
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                "xlsx by package part");
  require_sniff(zip_naming("ppt/presentation.xml"),
                "application/vnd.openxmlformats-officedocument.presentationml.presentation",
                "pptx by package part");
  require_sniff(zip_naming("META-INF/container.xml"), "application/epub+zip",
                "epub by its container file when the mimetype entry is not first");
  require_sniff(zip_naming("readme.txt"), "application/zip", "a bare zip");
  require_sniff(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + std::string(512, '\0'), "",
                "a compound file defers to the extension");
}

void verify_binary_signatures() {
  require_sniff("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n", "application/pdf", "pdf");
  require_sniff(std::string("\x89PNG\r\n\x1A\n", 8) + "IHDR", "image/png", "png");
  require_sniff("\xFF\xD8\xFF\xE0JFIF", "image/jpeg", "jpeg");
  require_sniff("GIF89a", "image/gif", "gif");
  require_sniff(std::string("II*\0", 4), "image/tiff", "tiff little-endian");
  require_sniff(std::string("MM\0*", 4), "image/tiff", "tiff big-endian");
  require_sniff(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16), "image/webp", "webp");
  require_sniff(std::string("BM") + std::string(20, '\0'), "image/bmp", "bmp");
  require_sniff("\x1F\x8B\x08", "application/gzip", "gzip");
  require_sniff("WARC/1.1\r\nWARC-Type: warcinfo\r\n", "application/warc", "warc");
  require_sniff("{\\rtf1\\ansi", "application/rtf", "rtf");
  require_sniff("", "", "empty input sniffs nothing");
}

void verify_text_signatures() {
  require_sniff("<!DOCTYPE html>\n<html><body></body></html>", "text/html", "html doctype");
  require_sniff("\xEF\xBB\xBF  \n<html lang=\"en\">", "text/html", "html after BOM and space");
  require_sniff("<div><p>x</p></div>\n<body>", "text/html", "html fragment naming body");
  require_sniff("<?xml version=\"1.0\"?>\n<article/>", "application/xml", "xml declaration");
  require_sniff("<?xml version=\"1.0\"?>\n<!DOCTYPE html>\n<html xmlns=\"x\"/>",
                "application/xhtml+xml", "xhtml behind an xml declaration");
  require_sniff("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>",
                "image/svg+xml", "svg behind an xml declaration");
  require_sniff("<svg viewBox=\"0 0 1 1\"></svg>", "image/svg+xml", "bare svg");
  require_sniff("From: a@example.org\r\nTo: b@example.org\r\nSubject: hi\r\n\r\nbody\r\n",
                "message/rfc822", "mail headers");
  require_sniff("Received: from x\n\tby y\nMessage-ID: <1@x>\n\nhi", "message/rfc822",
                "folded mail headers");
  require_sniff("Title: not mail\nAuthor: nobody\n\ntext", "text/plain",
                "header-shaped lines without a mail header are plain text");
  require_sniff("---\ntitle: x\n---\n\ntext", "text/markdown", "front matter");
  require_sniff("# Heading\n\nParagraph.\n", "text/markdown", "atx heading");
  require_sniff("intro\n\n```\ncode\n```\n", "text/markdown", "fenced code");
  require_sniff("See [the docs](https://example.org) for more.", "text/markdown", "link");
  require_sniff("| a | b |\n|---|---|\n| 1 | 2 |\n", "text/markdown", "pipe table rule");
  require_sniff("shopping\n- eggs\n- milk\n", "text/markdown", "two list lines");
  require_sniff("- a single dash line is not enough\n", "text/plain", "one list line stays plain");
  require_sniff("Plain prose with #hashtag and 3.5 numbers.\n", "text/plain", "plain text");
  require_sniff("{\"a\": [1, 2]}\n", "application/json", "json object");
  require_sniff("[1, 2, 3]", "application/json", "json array");
  require_sniff(std::string("text\0with nul", 13), "", "a NUL byte is not text");
  require_sniff("caf\xC3\xA9 au lait\n", "text/plain", "valid utf-8 is text");
  require_sniff("bad \xC3 sequence", "", "invalid utf-8 is not text");
}

void verify_extension_map() {
  require(grparse::extension_mimetype("a.html") == "text/html", "html by extension");
  require(grparse::extension_mimetype("a.MD") == "text/markdown", "case-insensitive extension");
  require(grparse::extension_mimetype("a.unknown") == "application/octet-stream",
          "unknown extension is octet-stream");
  require(grparse::extension_mimetype("page.storage.xhtml") == grparse::kConfluenceStorageMimetype,
          "the storage dialect keeps its own type");
}

void verify_resolution_order() {
  const std::string pdf = "%PDF-1.4\n";
  auto declared = grparse::resolve_mimetype("text/plain; charset=utf-8", pdf, "x.pdf");
  require(declared.mimetype == "text/plain" && declared.evidence == "declared",
          "an explicit content type wins, parameters dropped");
  auto sniffed = grparse::resolve_mimetype("application/octet-stream", pdf, "x.bin");
  require(sniffed.mimetype == "application/pdf" && sniffed.evidence == "magic",
          "octet-stream is no declaration; the bytes decide");
  auto by_name = grparse::resolve_mimetype("", std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8),
                                           "old.doc");
  require(by_name.mimetype == "application/msword" && by_name.evidence == "extension",
          "unsniffable bytes fall back to the extension");
  auto fallback = grparse::resolve_mimetype("", std::string("\x01\x02\x03", 3), "blob.zzz");
  require(fallback.mimetype == "application/octet-stream" && fallback.evidence == "fallback",
          "nothing known is octet-stream");
  auto storage = grparse::resolve_mimetype("", "<p>Body</p><ac:structured-macro/>",
                                           "page.storage.xhtml");
  require(storage.mimetype == grparse::kConfluenceStorageMimetype &&
              storage.evidence == "extension",
          "the storage dialect's suffix outranks a fragment sniff");
  auto html = grparse::resolve_mimetype("", "<!doctype html><html></html>", "page");
  require(html.mimetype == "text/html" && html.evidence == "magic",
          "a nameless html body is still html");
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The corpus tier: every in-corpus fixture sniffs to the type its name
// says, so a signature the synthetic cases above model wrongly fails here
// against the real bytes. Skipped quietly when the corpus is not wired in.
void verify_corpus_fixtures() {
  const char* corpus = std::getenv("GRPARSE_TEST_CORPUS_DIR");
  if (corpus == nullptr || !fs::is_directory(corpus)) {
    std::println("content-sniff-test: GRPARSE_TEST_CORPUS_DIR unset, corpus tier skipped");
    return;
  }
  static const std::map<std::string, std::string> kExpected = {
      {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
      {".epub", "application/epub+zip"},
      {".pdf", "application/pdf"},
      {".png", "image/png"},
      {".html", "text/html"},
      {".md", "text/markdown"},
      {".xml", "application/xml"},
      {".eml", "message/rfc822"},
  };
  int checked = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(corpus)) {
    const auto expected = kExpected.find(entry.path().extension().string());
    if (expected == kExpected.end()) continue;
    require_sniff(read_file(entry.path()), expected->second, entry.path().filename().string());
    checked++;
  }
  require(checked >= 10, "the corpus tier saw the fixtures it expects");
  std::println("content-sniff-test: {} corpus fixtures sniffed as their extension says", checked);
}

}  // namespace

int main() {
  try {
    verify_container_signatures();
    verify_binary_signatures();
    verify_text_signatures();
    verify_extension_map();
    verify_resolution_order();
    verify_corpus_fixtures();
    std::println("content-sniff-test: all checks passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "content-sniff-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
