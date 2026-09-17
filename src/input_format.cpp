#include "grparse/input_format.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse {
namespace {

std::string lower_extension(const std::filesystem::path& filename) {
  std::string extension = filename.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

}  // namespace

std::optional<parsev1::InputFormat> input_format_for(
    std::string_view mimetype, const std::filesystem::path& filename) {
  const std::string ext = lower_extension(filename);

  if (mimetype == "application/pdf") return parsev1::INPUT_FORMAT_PDF;
  if (mimetype.starts_with("image/")) return parsev1::INPUT_FORMAT_IMAGE;
  if (mimetype == "application/vnd.openxmlformats-officedocument.wordprocessingml.document") {
    return parsev1::INPUT_FORMAT_DOCX;
  }
  if (mimetype == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") {
    return parsev1::INPUT_FORMAT_XLSX;
  }
  if (mimetype ==
      "application/vnd.openxmlformats-officedocument.presentationml.presentation") {
    return parsev1::INPUT_FORMAT_PPTX;
  }
  if (mimetype == "application/msword") return parsev1::INPUT_FORMAT_DOC;
  if (mimetype == "application/vnd.ms-excel") return parsev1::INPUT_FORMAT_XLS;
  if (mimetype == "application/vnd.ms-powerpoint") return parsev1::INPUT_FORMAT_PPT;
  if (mimetype == "application/vnd.oasis.opendocument.text") return parsev1::INPUT_FORMAT_ODT;
  if (mimetype == "application/vnd.oasis.opendocument.spreadsheet") {
    return parsev1::INPUT_FORMAT_ODS;
  }
  if (mimetype == "application/vnd.oasis.opendocument.presentation") {
    return parsev1::INPUT_FORMAT_ODP;
  }
  if (mimetype == "application/rtf" || mimetype == "text/rtf") return parsev1::INPUT_FORMAT_RTF;
  if (mimetype == "application/x-mimearchive" || mimetype == "multipart/related") {
    return parsev1::INPUT_FORMAT_MHTML;
  }
  if (mimetype == "application/epub+zip") return parsev1::INPUT_FORMAT_EPUB;
  if (mimetype == "message/rfc822" || mimetype == "application/vnd.ms-outlook") {
    return parsev1::INPUT_FORMAT_EMAIL;
  }
  if (mimetype == "text/html" || mimetype == "application/xhtml+xml") {
    return parsev1::INPUT_FORMAT_HTML;
  }
  if (mimetype == "text/markdown") return parsev1::INPUT_FORMAT_MD;
  if (mimetype == "text/csv") return parsev1::INPUT_FORMAT_CSV;
  if (mimetype == "application/json") return parsev1::INPUT_FORMAT_JSON_DOCLING;
  if (mimetype.starts_with("audio/")) return parsev1::INPUT_FORMAT_AUDIO;
  if (mimetype.starts_with("video/")) return parsev1::INPUT_FORMAT_VIDEO;
  if (mimetype == "application/vnd.apple.pages") return parsev1::INPUT_FORMAT_IWORK_PAGES;
  if (mimetype == "application/x-afp") return parsev1::INPUT_FORMAT_EBCDIC;
  if (mimetype == "text/asciidoc" || ext == ".adoc" || ext == ".asciidoc") {
    return parsev1::INPUT_FORMAT_ASCIIDOC;
  }
  if (mimetype == "text/vtt" || ext == ".vtt") return parsev1::INPUT_FORMAT_VTT;
  if (mimetype == "application/x-latex" || mimetype == "text/x-tex" || ext == ".tex" ||
      ext == ".latex") {
    return parsev1::INPUT_FORMAT_LATEX;
  }
  if (mimetype == "application/xml" || mimetype == "text/xml") {
    if (ext == ".xbrl") return parsev1::INPUT_FORMAT_XML_XBRL;
    // JATS / USPTO / DocLang are not distinguishable by MIME alone; leave
    // extension-based hints for the common cases and treat bare XML as JATS
    // only when the name says so.
    if (ext == ".nxml" || filename.string().find("jats") != std::string::npos) {
      return parsev1::INPUT_FORMAT_XML_JATS;
    }
    if (filename.string().find("uspto") != std::string::npos) {
      return parsev1::INPUT_FORMAT_XML_USPTO;
    }
    if (ext == ".doclang" || filename.string().find("doclang") != std::string::npos) {
      return parsev1::INPUT_FORMAT_XML_DOCLANG;
    }
    return parsev1::INPUT_FORMAT_XML_JATS;
  }
  if (ext == ".md" || ext == ".markdown") return parsev1::INPUT_FORMAT_MD;
  if (ext == ".html" || ext == ".htm") return parsev1::INPUT_FORMAT_HTML;
  if (ext == ".csv") return parsev1::INPUT_FORMAT_CSV;
  if (ext == ".pdf") return parsev1::INPUT_FORMAT_PDF;
  if (ext == ".docx") return parsev1::INPUT_FORMAT_DOCX;
  if (ext == ".doc") return parsev1::INPUT_FORMAT_DOC;
  if (ext == ".rtf") return parsev1::INPUT_FORMAT_RTF;
  if (ext == ".mht" || ext == ".mhtml") return parsev1::INPUT_FORMAT_MHTML;
  if (ext == ".epub") return parsev1::INPUT_FORMAT_EPUB;
  if (ext == ".eml" || ext == ".msg") return parsev1::INPUT_FORMAT_EMAIL;
  return std::nullopt;
}

bool from_formats_allows(const google::protobuf::RepeatedField<int>& from_formats,
                         parsev1::InputFormat detected) {
  if (from_formats.empty()) return true;
  for (const int raw : from_formats) {
    if (static_cast<parsev1::InputFormat>(raw) == detected) return true;
  }
  return false;
}

}  // namespace grparse
