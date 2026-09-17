#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse/input_format.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;

void require_format(std::string_view mime, const std::string& name,
                    parsev1::InputFormat expected, const std::string& what) {
  const auto got = grparse::input_format_for(mime, name);
  require(got.has_value(), what + ": expected a mapped InputFormat");
  if (*got != expected) {
    throw std::runtime_error(what + ": expected " + parsev1::InputFormat_Name(expected) +
                             ", got " + parsev1::InputFormat_Name(*got));
  }
}

void verify_common_mime_mappings() {
  require_format("application/pdf", "a.pdf", parsev1::INPUT_FORMAT_PDF, "pdf");
  require_format("image/png", "a.png", parsev1::INPUT_FORMAT_IMAGE, "png");
  require_format("application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                 "a.docx", parsev1::INPUT_FORMAT_DOCX, "docx");
  require_format("application/msword", "a.doc", parsev1::INPUT_FORMAT_DOC, "doc");
  require_format("application/rtf", "a.rtf", parsev1::INPUT_FORMAT_RTF, "rtf");
  require_format("application/x-mimearchive", "a.mhtml", parsev1::INPUT_FORMAT_MHTML,
                 "mhtml");
  require_format("application/epub+zip", "a.epub", parsev1::INPUT_FORMAT_EPUB, "epub");
  require_format("message/rfc822", "a.eml", parsev1::INPUT_FORMAT_EMAIL, "eml");
  require_format("text/markdown", "a.md", parsev1::INPUT_FORMAT_MD, "md");
  require_format("text/html", "a.html", parsev1::INPUT_FORMAT_HTML, "html");
  require_format("text/csv", "a.csv", parsev1::INPUT_FORMAT_CSV, "csv");
  require_format("audio/mpeg", "a.mp3", parsev1::INPUT_FORMAT_AUDIO, "mp3");
  require_format("video/mp4", "a.mp4", parsev1::INPUT_FORMAT_VIDEO, "mp4");
}

void verify_allowlist() {
  google::protobuf::RepeatedField<int> empty;
  require(grparse::from_formats_allows(empty, parsev1::INPUT_FORMAT_PDF),
          "empty from_formats allows everything");

  google::protobuf::RepeatedField<int> pdf_only;
  pdf_only.Add(parsev1::INPUT_FORMAT_PDF);
  require(grparse::from_formats_allows(pdf_only, parsev1::INPUT_FORMAT_PDF),
          "pdf allowlist accepts pdf");
  require(!grparse::from_formats_allows(pdf_only, parsev1::INPUT_FORMAT_MD),
          "pdf allowlist rejects md");
}

}  // namespace

int main() {
  try {
    verify_common_mime_mappings();
    verify_allowlist();
  } catch (const std::exception& ex) {
    std::println(stderr, "{}", ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
