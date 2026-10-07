#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"
#include "support/check.h"
#include "support/compound_file.h"

namespace fs = std::filesystem;

namespace {

using grparse_test::require;

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

// A well-formed zip of stored entries: local headers, then a central
// directory listing every entry, then the end record.
std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string zip;
  std::string directory;
  for (const auto& [name, data] : entries) {
    const unsigned long offset = zip.size();
    zip += "PK\x03\x04";
    zip += le16(20) + le16(0) + le16(0) + le16(0) + le16(0) + le32(0);
    zip += le32(data.size()) + le32(data.size());
    zip += le16(static_cast<unsigned>(name.size())) + le16(0);
    zip += name + data;
    directory += "PK\x01\x02";
    directory += le16(20) + le16(20) + le16(0) + le16(0) + le16(0) + le16(0) + le32(0);
    directory += le32(data.size()) + le32(data.size());
    directory += le16(static_cast<unsigned>(name.size())) + le16(0) + le16(0);
    directory += le16(0) + le16(0) + le32(0) + le32(offset);
    directory += name;
  }
  const unsigned long directory_at = zip.size();
  zip += directory;
  zip += "PK\x05\x06";
  zip += le16(0) + le16(0);
  zip += le16(static_cast<unsigned>(entries.size())) + le16(static_cast<unsigned>(entries.size()));
  zip += le32(directory.size()) + le32(directory_at) + le16(0);
  return zip;
}

// A deck that carries a chart's workbook stored uncompressed: the inner
// package's "xl/workbook.xml" is in the outer archive's bytes, but the outer
// archive lists only the deck's own parts.
void verify_embedded_packages_do_not_decide_the_type() {
  const std::string workbook = stored_zip({{"[Content_Types].xml", "<Types/>"},
                                           {"xl/workbook.xml", "<workbook/>"}});
  const std::string deck = stored_zip({{"[Content_Types].xml", "<Types/>"},
                                       {"ppt/embeddings/Microsoft_Excel_Worksheet1.xlsx", workbook},
                                       {"ppt/presentation.xml", "<presentation/>"}});
  require_sniff(deck, "application/vnd.openxmlformats-officedocument.presentationml.presentation",
                "a deck with an embedded workbook is a deck");
  require_sniff(workbook, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                "the workbook on its own is a workbook");
  const std::string letter = stored_zip({{"word/document.xml", "<document/>"},
                                         {"word/embeddings/Microsoft_Excel_Worksheet.xlsx", workbook}});
  require_sniff(letter, "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                "a document with an embedded workbook is a document");
}

// The compound-file fixtures: what Office writes for each family, encrypted
// and not, built with the test-only writer.
namespace office {

using grparse_test::CompoundObject;
using grparse_test::compound_file;

// An encrypted .docx/.xlsx/.pptx: the package, its encryption header and
// the DataSpaces storage Office adds beside them.
std::string encrypted_package() {
  return compound_file({
      CompoundObject::stream("EncryptionInfo", std::string(200, '\x01')),
      CompoundObject::stream("EncryptedPackage", std::string(6000, '\x02')),
      CompoundObject::folder("\x06"
                             "DataSpaces",
                             {CompoundObject::stream("Version", std::string(76, '\0')),
                              CompoundObject::stream("DataSpaceMap", std::string(104, '\0'))}),
  });
}

// A Word binary file's WordDocument stream: a FIB whose flags word carries
// `flags`, followed by enough bytes to look like a document.
std::string word_document(unsigned flags) {
  std::string fib(1472, '\0');
  fib[0] = '\xEC';
  fib[1] = '\xA5';
  fib[10] = static_cast<char>(flags & 0xFF);
  fib[11] = static_cast<char>((flags >> 8) & 0xFF);
  return fib + std::string(5000, 'w');
}

std::vector<CompoundObject> word_file(unsigned fib_flags, std::vector<CompoundObject> extra = {}) {
  std::vector<CompoundObject> objects{
      CompoundObject::stream("WordDocument", word_document(fib_flags)),
      CompoundObject::stream("1Table", std::string(300, 't')),
      CompoundObject::stream("\x05SummaryInformation", std::string(120, 's')),
  };
  for (auto& object : extra) objects.push_back(std::move(object));
  return objects;
}

std::string biff_record(unsigned type, const std::string& body) {
  return le16(type) + le16(static_cast<unsigned>(body.size())) + body;
}

// A BIFF8 Workbook stream: BOF, then the records given, then EOF.
std::string workbook_stream(const std::vector<std::string>& records) {
  std::string stream = biff_record(0x0809, le16(0x0600) + le16(0x0005) + std::string(12, '\0'));
  for (const auto& record : records) stream += record;
  stream += biff_record(0x000A, "");
  return stream;
}

std::string filepass() { return biff_record(0x002F, le16(1) + le16(1) + le16(1) + std::string(48, 'k')); }
std::string interface_header() { return biff_record(0x00E1, le16(0x04B0)); }

std::vector<CompoundObject> excel_file(const std::string& stream_name, const std::string& workbook) {
  return {CompoundObject::stream(stream_name, workbook),
          CompoundObject::stream("\x05SummaryInformation", std::string(120, 's'))};
}

// A PowerPoint binary file: the document stream and the Current User atom,
// whose header token says whether the file is encrypted.
std::vector<CompoundObject> powerpoint_file(unsigned long header_token) {
  std::string current_user = le16(0x000F) + le16(0x0FF6) + le32(20);  // record header
  current_user += le32(20);           // size
  current_user += le32(header_token);  // headerToken
  current_user += std::string(12, '\0');
  return {CompoundObject::stream("PowerPoint Document", std::string(7000, 'p')),
          CompoundObject::stream("Current User", current_user)};
}

}  // namespace office

void require_encrypted(const std::string& bytes, const std::string& format,
                       const std::string& what) {
  const auto verdict = grparse::encrypted_office_document(bytes);
  require(verdict.has_value(), what + ": expected a password-protected verdict");
  require(verdict->format == format,
          what + ": expected format '" + format + "', got '" + verdict->format + "' (" +
              verdict->evidence + ")");
}

void require_not_encrypted(const std::string& bytes, const std::string& what) {
  const auto verdict = grparse::encrypted_office_document(bytes);
  require(!verdict.has_value(),
          what + ": expected no verdict, got '" + (verdict ? verdict->format : "") + "' (" +
              (verdict ? verdict->evidence : "") + ")");
}

// An encrypted Office Open XML document is a compound file whose root
// lists the EncryptedPackage stream; the same name anywhere else is not one.
void verify_encrypted_office_packages_are_recognised() {
  require_encrypted(office::encrypted_package(), "Office Open XML package",
                    "an encrypted package");
  require_not_encrypted("EncryptedPackage", "the name alone, outside a compound file");

  // A legacy document that embeds an encrypted workbook keeps the package
  // under ObjectPool, two storages down; that is the embedded object's
  // business, not the document's, and the byte search used to refuse it.
  const auto embedded = office::word_file(
      0, {grparse_test::CompoundObject::folder(
             "ObjectPool",
             {grparse_test::CompoundObject::folder(
                 "_1234567890",
                 {grparse_test::CompoundObject::stream("EncryptionInfo", std::string(200, 'e')),
                  grparse_test::CompoundObject::stream("EncryptedPackage",
                                                       std::string(4500, 'E')),
                  grparse_test::CompoundObject::stream("\x01Ole", std::string(20, 'o'))})})});
  require_not_encrypted(grparse_test::compound_file(embedded),
                        "a plain .doc embedding an encrypted package");
  // The directory walk starts at the root, so a package name in stream
  // bytes is nothing either.
  const auto name_in_body = office::word_file(
      0, {grparse_test::CompoundObject::stream("Data", std::string("E\0n\0c\0r\0y\0p\0t\0e\0d\0P\0a\0c\0k\0a\0g\0e\0", 32) + std::string(4100, 'd'))});
  require_not_encrypted(grparse_test::compound_file(name_in_body),
                        "a .doc whose data stream spells the name");
}

// The binary formats say so in their own structures: the Word FIB flag, the
// Excel FILEPASS record, the PowerPoint Current User header token.
void verify_encrypted_legacy_office_files_are_recognised() {
  require_encrypted(grparse_test::compound_file(office::word_file(0x0100)), "Word document",
                    "a .doc with fEncrypted set");
  require_not_encrypted(grparse_test::compound_file(office::word_file(0x0000)),
                        "a plain .doc");
  require_not_encrypted(grparse_test::compound_file(office::word_file(0x0200)),
                        "a .doc with a neighbouring FIB flag (fWhichTblStm) set");

  require_encrypted(
      grparse_test::compound_file(office::excel_file(
          "Workbook", office::workbook_stream({office::filepass(), office::interface_header()}))),
      "Excel workbook", "a BIFF8 workbook with FILEPASS");
  require_encrypted(
      grparse_test::compound_file(office::excel_file(
          "Book", office::workbook_stream({office::interface_header(), office::filepass()}))),
      "Excel workbook", "a BIFF5 workbook with FILEPASS after another record");
  require_not_encrypted(
      grparse_test::compound_file(office::excel_file(
          "Workbook", office::workbook_stream({office::interface_header()}))),
      "a plain workbook");
  // FILEPASS past the first records' walk is not looked for: a record body
  // that spells the type is not a record.
  require_not_encrypted(
      grparse_test::compound_file(office::excel_file(
          "Workbook", office::workbook_stream({office::biff_record(0x00FC, le16(0x002F) + "x")}))),
      "a workbook whose SST body holds the FILEPASS type");

  require_encrypted(grparse_test::compound_file(office::powerpoint_file(0xF3D1C4DF)),
                    "PowerPoint presentation", "an encrypted .ppt");
  require_not_encrypted(grparse_test::compound_file(office::powerpoint_file(0xE391C05F)),
                        "a plain .ppt");
}

// Damaged compound files end the walk, never the process.
void verify_damaged_compound_files_are_not_encrypted() {
  const std::string encrypted = office::encrypted_package();
  require_not_encrypted(encrypted.substr(0, 511), "a header cut short");
  require_not_encrypted(encrypted.substr(0, 1024), "a file cut before its directory");
  std::string looping = encrypted;
  // The directory chain points back at itself: FAT entry for sector 1.
  looping[512 + 4 * 1] = '\x01';
  looping[512 + 4 * 1 + 1] = '\0';
  looping[512 + 4 * 1 + 2] = '\0';
  looping[512 + 4 * 1 + 3] = '\0';
  grparse::encrypted_office_document(looping);  // terminates
  std::string wild = encrypted;
  // The root's child points far outside the directory.
  wild[1024 + 76] = '\xFF';
  wild[1024 + 77] = '\xFF';
  wild[1024 + 78] = '\xFF';
  wild[1024 + 79] = '\x7F';
  require_not_encrypted(wild, "a root whose child is out of range");
  require_not_encrypted(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + std::string(2000, '\xFF'),
                        "a signature followed by noise");
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
  require_sniff("bad \xC3 sequence", "text/plain", "a lone high byte reads as single-byte text");
  require_sniff("Dear Customer,\r\nOn Saturday the caf\xE9 is closed.\r\n", "text/plain",
                "Latin-1 prose is text");
  require_sniff("\xE9\xE8\xE0\xF9 ok", "", "mostly high bytes are not a single-byte text");
  require_sniff(std::string("abc\x01\x02\xFF" "def", 9), "", "control bytes are not text");
  require_sniff("From:\tTransport for London <info@example.org>\r\nSent:\t13 July 2015\r\n\r\n"
                "Dear Customer,\r\n",
                "text/plain", "a pasted From/Sent display block is not a mail header block");
  require_sniff("a,b,c\n1,2,3\n4,\"5,5\",6\n", "text/csv", "three lines of three fields");
  require_sniff("name;amount\nx;1\ny;2\n", "text/csv", "semicolon separated values");
  require_sniff("a,b\n1,2\n", "text/plain", "two narrow lines are too few to call csv");
  require_sniff("Family,Entity,Date,Amount\r\nHMRC,VOA,01/09/2014,510.00\r\n", "text/csv",
                "a header and one row of four fields");
  require_sniff("\"id\",\"note\",\"n\"\n\"1\",\"first line\nsecond line\",\"2\"\n\"3\",\"x\",\"4\"\n",
                "text/csv", "a quoted field may hold a line break");
  require_sniff("Hello, world.\nThis line, too, has commas.\nBut, here, more.\n", "text/plain",
                "prose with unequal comma counts stays plain");
  require_sniff("one\ntwo\nthree\n", "text/plain", "one field per line is not csv");
}

// A compound file is placed by its root streams: the legacy Office families
// by their document stream, an encrypted package by its EncryptedPackage
// stream, and any other compound file not at all.
void verify_compound_files_are_placed_by_their_streams() {
  using grparse_test::CompoundObject;
  require_sniff(grparse_test::compound_file(office::word_file(0)), "application/msword",
                "a Word binary file");
  require_sniff(grparse_test::compound_file(office::excel_file("Workbook", office::workbook_stream({}))),
                "application/vnd.ms-excel", "an Excel 97 workbook");
  require_sniff(grparse_test::compound_file(office::excel_file("Book", office::workbook_stream({}))),
                "application/vnd.ms-excel", "an Excel 5 workbook");
  require_sniff(grparse_test::compound_file(office::powerpoint_file(0xE391C05F)),
                "application/vnd.ms-powerpoint", "a PowerPoint binary file");
  require_sniff(office::encrypted_package(), std::string(grparse::kEncryptedOfficePackageMimetype),
                "an encrypted Office Open XML package");
  require_sniff(grparse_test::compound_file(
                    {CompoundObject::stream("__substg1.0_0037001F", std::string(80, 'm'))}),
                "", "an Outlook message is left to its extension");

  const auto named = grparse::resolve_mimetype("", office::encrypted_package(), "report.docx");
  require(named.mimetype ==
                  "application/vnd.openxmlformats-officedocument.wordprocessingml.document" &&
              named.evidence == "extension",
          "an encrypted package's name says which package it is");
  const auto nameless = grparse::resolve_mimetype("", office::encrypted_package(), "upload");
  require(nameless.mimetype == grparse::kEncryptedOfficePackageMimetype &&
              nameless.evidence == "magic",
          "a nameless encrypted package rests on its bytes");
  const auto txt = grparse::resolve_mimetype("", "a,b\n1,2\n3,4\n", "table.txt");
  require(txt.mimetype == "text/plain" && txt.evidence == "extension",
          "a text name outranks a csv guess");
  const auto bare = grparse::resolve_mimetype("", "a,b\n1,2\n3,4\n", "table");
  require(bare.mimetype == "text/csv" && bare.evidence == "magic",
          "a nameless csv rests on its bytes");
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
  return grparse_test::run_test_main("content-sniff-test", "all checks passed", {
      verify_container_signatures,
      verify_encrypted_office_packages_are_recognised,
      verify_encrypted_legacy_office_files_are_recognised,
      verify_damaged_compound_files_are_not_encrypted,
      verify_embedded_packages_do_not_decide_the_type,
      verify_binary_signatures,
      verify_text_signatures,
      verify_compound_files_are_placed_by_their_streams,
      verify_extension_map,
      verify_resolution_order,
      verify_corpus_fixtures,
  });
}
