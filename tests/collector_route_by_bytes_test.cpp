// S3 eval finding: an object sent under a name with no extension was routed
// by the name alone, so docx, mail and markup bytes all landed on the CV
// path and failed as "not a raster". route_document consults the bytes the
// way the origin stamp does; a name that says something keeps its say, and
// plain text reads through the markup collector as the trivial Markdown.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse/collector_coordinator.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace markupv1 = ai::pipestream::markup::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string le16(unsigned value) {
  return {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
}

std::string le32(unsigned long value) {
  return le16(static_cast<unsigned>(value & 0xFFFF)) +
         le16(static_cast<unsigned>((value >> 16) & 0xFFFF));
}

// A zip whose first local entry is the stored "mimetype" file (OpenDocument).
std::string odt_bytes() {
  const std::string mime = "application/vnd.oasis.opendocument.text";
  std::string zip = "PK\x03\x04";
  zip += le16(20) + le16(0) + le16(0) + le16(0) + le16(0) + le32(0);
  zip += le32(mime.size()) + le32(mime.size()) + le16(8) + le16(0);
  zip += "mimetype" + mime;
  return zip;
}

std::string docx_bytes() {
  std::string zip = "PK\x03\x04";
  zip += std::string(26, '\0');
  zip += "word/document.xml<w:document/>";
  return zip;
}

void verify_extension_less_names_route_by_bytes() {
  using grparse::route_document;
  require(route_document("upload", "", docx_bytes()) == parsev1::COLLECTOR_LIBREOFFICE,
          "docx bytes under a bare name reach libreoffice");
  require(route_document("upload", "", odt_bytes()) == parsev1::COLLECTOR_LIBREOFFICE,
          "opendocument bytes under a bare name reach libreoffice");
  require(route_document("mail", "", "From: a@example.org\r\nTo: b@example.org\r\nSubject: hi\r\n\r\nbody\r\n") ==
              parsev1::COLLECTOR_EMAIL,
          "rfc822 bytes under a bare name reach the email collector");
  require(route_document("notes", "", "# Title\n\nSome prose.\n") == parsev1::COLLECTOR_MARKUP,
          "markdown bytes under a bare name reach the markup collector");
  require(route_document("page", "", "<!doctype html><html><body><p>x</p></body></html>") ==
              parsev1::COLLECTOR_MARKUP,
          "html bytes under a bare name reach the markup collector");
  require(route_document("scan", "", "%PDF-1.7\n") == parsev1::COLLECTOR_GRPARSE_CV,
          "pdf bytes stay on the CV path");
  require(route_document("photo", "", std::string("\x89PNG\r\n\x1A\n", 8)) == parsev1::COLLECTOR_GRPARSE_CV,
          "raster bytes stay on the CV path");
  require(route_document("feed", "", "<?xml version=\"1.0\"?><article/>") == parsev1::COLLECTOR_XML,
          "xml bytes under a bare name reach the xml collector");
}

void verify_a_name_that_says_something_keeps_its_say() {
  using grparse::route_document;
  require(route_document("data.csv", "", "a,b\n1,2\n") == parsev1::COLLECTOR_LIBREOFFICE,
          "a csv whose bytes read as plain text still routes by its name");
  require(route_document("page.xml", "", "<?xml version=\"1.0\"?><html xmlns=\"x\"><body/></html>") ==
              parsev1::COLLECTOR_XML,
          "an .xml whose bytes sniff as xhtml stays with the xml collector");
  require(route_document("notes.md", "", "just prose, no markers\n") == parsev1::COLLECTOR_MARKUP,
          "a .md with no markers still reaches the markup collector");
  require(route_document("blob", "", std::string("\x00\x01\x02\x03", 4)) == parsev1::COLLECTOR_GRPARSE_CV,
          "bytes nothing recognises fall to the CV path as before");
  require(route_document("image.png", "", "memory") == parsev1::COLLECTOR_GRPARSE_CV,
          "a declared raster with text bytes stays a (bad) raster, not a markup document");
  require(route_document("scan.pdf", "", "plain text, not a pdf") == parsev1::COLLECTOR_GRPARSE_CV,
          "a declared pdf keeps the CV path whatever its bytes say");
  require(route_document("upload.bin", "", docx_bytes()) == parsev1::COLLECTOR_LIBREOFFICE,
          "an unknown extension lets the bytes route");
  require(route_document("page", "text/html", "anything") == parsev1::COLLECTOR_MARKUP,
          "a declared content type wins without a sniff");
}

void verify_plain_text_reads_as_markdown() {
  using grparse::markup_format_for;
  require(markup_format_for("notes.txt", "") == markupv1::MARKUP_FORMAT_MARKDOWN,
          ".txt is dialed as markdown");
  require(markup_format_for("notes", "text/plain") == markupv1::MARKUP_FORMAT_MARKDOWN,
          "text/plain is dialed as markdown");
  require(grparse::route_document("notes.txt", "", "Plain prose.\nSecond line.\n") == parsev1::COLLECTOR_MARKUP,
          "a .txt reaches the markup collector instead of the CV path");
  require(markup_format_for("page.html", "text/plain") == markupv1::MARKUP_FORMAT_HTML,
          "the extension still outranks a plain-text type");
}

}  // namespace

int main() {
  try {
    verify_extension_less_names_route_by_bytes();
    verify_a_name_that_says_something_keeps_its_say();
    verify_plain_text_reads_as_markdown();
  } catch (const std::exception& error) {
    std::println(stderr, "collector_route_by_bytes_test: {}", error.what());
    return 1;
  }
  std::println("collector_route_by_bytes_test: ok");
  return 0;
}
