#include "grparse/content_sniff.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "grparse/confluence_storage.h"

namespace grparse {
namespace {

using std::string_view;

// How much of a text body the content probes read; markers past this point
// do not change what the leading kilobytes already said.
constexpr size_t kTextProbeBytes = 8192;
// The tail of a zip archive that holds its central directory, where every
// entry name is listed whatever the entry order.
constexpr size_t kZipDirectoryBytes = 65536;

bool starts_with(string_view bytes, string_view prefix) {
  return bytes.substr(0, prefix.size()) == prefix;
}

bool starts_with_nocase(string_view bytes, string_view prefix) {
  if (bytes.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(bytes[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

bool contains_nocase(string_view haystack, string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
    if (starts_with_nocase(haystack.substr(i), needle)) return true;
  }
  return false;
}

string_view strip_bom_and_space(string_view bytes) {
  if (starts_with(bytes, "\xEF\xBB\xBF")) bytes.remove_prefix(3);
  while (!bytes.empty() &&
         std::isspace(static_cast<unsigned char>(bytes.front())) != 0) {
    bytes.remove_prefix(1);
  }
  return bytes;
}

uint32_t le16(string_view bytes, size_t at) {
  return static_cast<uint8_t>(bytes[at]) |
         (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 8);
}

uint32_t le32(string_view bytes, size_t at) {
  return le16(bytes, at) | (le16(bytes, at + 2) << 16);
}

// The stored content of a zip whose first local entry is the uncompressed
// "mimetype" file, the OpenDocument and EPUB convention; empty otherwise.
std::string zip_mimetype_entry(string_view bytes) {
  constexpr size_t kLocalHeader = 30;
  if (bytes.size() < kLocalHeader) return {};
  const uint32_t method = le16(bytes, 8);
  const uint32_t size = le32(bytes, 18);
  const uint32_t name_length = le16(bytes, 26);
  const uint32_t extra_length = le16(bytes, 28);
  if (method != 0 || name_length != 8 || size == 0 || size > 256) return {};
  const size_t data_at = kLocalHeader + name_length + extra_length;
  if (bytes.size() < data_at + size) return {};
  if (bytes.substr(kLocalHeader, 8) != "mimetype") return {};
  std::string mime(bytes.substr(data_at, size));
  while (!mime.empty() &&
         std::isspace(static_cast<unsigned char>(mime.back())) != 0) {
    mime.pop_back();
  }
  for (const char c : mime) {
    if (std::isprint(static_cast<unsigned char>(c)) == 0) return {};
  }
  return mime;
}

// True when an entry name appears in the archive's head or in the central
// directory at its tail.
bool zip_names_entry(string_view bytes, string_view entry) {
  const string_view head = bytes.substr(0, kZipDirectoryBytes);
  if (head.find(entry) != string_view::npos) return true;
  if (bytes.size() <= kZipDirectoryBytes) return false;
  const string_view tail = bytes.substr(bytes.size() - kZipDirectoryBytes);
  return tail.find(entry) != string_view::npos;
}

std::string sniff_zip(string_view bytes) {
  const std::string entry = zip_mimetype_entry(bytes);
  if (!entry.empty()) return entry;
  if (zip_names_entry(bytes, "word/document.xml")) {
    return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  }
  if (zip_names_entry(bytes, "xl/workbook.xml")) {
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  }
  if (zip_names_entry(bytes, "ppt/presentation.xml")) {
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  }
  if (zip_names_entry(bytes, "META-INF/container.xml")) {
    return "application/epub+zip";
  }
  return "application/zip";
}

// One row of the magic-number ladder: `magic` must sit at `offset`, an
// optional `also` must hold over the whole head, and the row's `mimetype` is
// what the match declares. `narrow` refines a container signature that names
// a family rather than a type (a zip says which family through its own
// entries); an empty answer from it leaves `mimetype` standing.
struct BinarySignature {
  string_view magic;
  size_t offset = 0;
  string_view mimetype;
  bool (*also)(string_view bytes) = nullptr;
  std::string (*narrow)(string_view bytes) = nullptr;
};

bool riff_container(string_view bytes) { return starts_with(bytes, "RIFF"); }

// A bitmap's file header is 14 bytes; a shorter body carrying "BM" is not one.
bool bitmap_header_complete(string_view bytes) { return bytes.size() >= 14; }

// The ladder, in precedence order: the first row whose signature sits where
// it must is the answer, so a row never shadows one above it. Lengths are
// explicit where a signature carries a NUL a bare literal would cut.
constexpr std::array<BinarySignature, 14> kBinarySignatures = {{
    {string_view("PK\x03\x04", 4), 0, "application/zip", nullptr, sniff_zip},
    {"%PDF-", 0, "application/pdf"},
    {string_view("\x89PNG\r\n\x1A\n", 8), 0, "image/png"},
    {string_view("\xFF\xD8\xFF", 3), 0, "image/jpeg"},
    {"GIF87a", 0, "image/gif"},
    {"GIF89a", 0, "image/gif"},
    {string_view("II*\0", 4), 0, "image/tiff"},
    {string_view("MM\0*", 4), 0, "image/tiff"},
    // The type tag sits behind the RIFF header's length word; both halves
    // are required, so the row matches the tag and asks for the container.
    {"WEBP", 8, "image/webp", riff_container},
    {"BM", 0, "image/bmp", bitmap_header_complete},
    {string_view("\x1F\x8B", 2), 0, "application/gzip"},
    {"WARC/", 0, "application/warc"},
    {"{\\rtf", 0, "application/rtf"},
    {"%!PS", 0, "application/postscript"},
}};

// True when `signature`'s bytes sit exactly where its row puts them.
bool signature_present(const BinarySignature& signature, string_view bytes) {
  if (bytes.size() < signature.offset + signature.magic.size()) return false;
  if (bytes.substr(signature.offset, signature.magic.size()) != signature.magic) return false;
  return signature.also == nullptr || signature.also(bytes);
}

std::string sniff_binary(string_view bytes) {
  for (const BinarySignature& signature : kBinarySignatures) {
    if (!signature_present(signature, bytes)) continue;
    if (signature.narrow != nullptr) {
      if (std::string narrowed = signature.narrow(bytes); !narrowed.empty()) return narrowed;
    }
    return std::string(signature.mimetype);
  }
  return {};
}

// True for a body that is valid UTF-8 (ASCII included) with no NUL bytes,
// over the probe window; a truncated final sequence is tolerated.
bool looks_like_text(string_view probe) {
  size_t i = 0;
  while (i < probe.size()) {
    const auto byte = static_cast<unsigned char>(probe[i]);
    // Control bytes other than the whitespace family are not text.
    if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' &&
        byte != '\f' && byte != '\v') {
      return false;
    }
    size_t length = 1;
    if (byte >= 0xF0) length = 4;
    else if (byte >= 0xE0) length = 3;
    else if (byte >= 0xC0) length = 2;
    else if (byte >= 0x80) return false;
    if (i + length > probe.size()) return probe.size() == kTextProbeBytes;
    for (size_t k = 1; k < length; k++) {
      if ((static_cast<unsigned char>(probe[i + k]) & 0xC0) != 0x80) return false;
    }
    i += length;
  }
  return true;
}

std::vector<string_view> lines_of(string_view text, size_t limit) {
  std::vector<string_view> lines;
  size_t start = 0;
  while (start < text.size() && lines.size() < limit) {
    size_t end = text.find('\n', start);
    if (end == string_view::npos) end = text.size();
    string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

// "Name: value" with a token name, the RFC 822 header line shape.
bool header_line(string_view line) {
  size_t colon = line.find(':');
  if (colon == string_view::npos || colon == 0) return false;
  if (colon + 1 < line.size() && line[colon + 1] != ' ' && line[colon + 1] != '\t') {
    return false;
  }
  for (const char c : line.substr(0, colon)) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-') return false;
  }
  return std::isalpha(static_cast<unsigned char>(line.front())) != 0;
}

bool looks_like_mail(string_view probe) {
  static constexpr std::array<string_view, 8> kMailHeaders = {
      "from:", "to:", "subject:", "received:", "message-id:",
      "mime-version:", "return-path:", "delivered-to:"};
  int header_lines = 0;
  int mail_headers = 0;
  for (string_view line : lines_of(probe, 40)) {
    if (line.empty()) break;
    if (line.front() == ' ' || line.front() == '\t') continue;  // folded
    if (!header_line(line)) return false;
    header_lines++;
    for (string_view name : kMailHeaders) {
      if (starts_with_nocase(line, name)) mail_headers++;
    }
  }
  return header_lines >= 2 && mail_headers >= 1;
}

bool markdown_heading(string_view line) {
  size_t hashes = 0;
  while (hashes < line.size() && line[hashes] == '#') hashes++;
  return hashes >= 1 && hashes <= 6 && hashes < line.size() && line[hashes] == ' ';
}

bool markdown_list_line(string_view line) {
  while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
  if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
      line[1] == ' ') {
    return true;
  }
  size_t digits = 0;
  while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])) != 0) {
    digits++;
  }
  return digits >= 1 && digits + 1 < line.size() && line[digits] == '.' &&
         line[digits + 1] == ' ';
}

bool markdown_table_rule(string_view line) {
  if (line.size() < 3 || line.front() != '|') return false;
  bool dash = false;
  for (const char c : line) {
    if (c == '-') dash = true;
    else if (c != '|' && c != ':' && c != ' ') return false;
  }
  return dash;
}

bool markdown_link(string_view line) {
  size_t close = line.find("](");
  if (close == string_view::npos) return false;
  size_t open = line.rfind('[', close);
  return open != string_view::npos && line.find(')', close) != string_view::npos;
}

bool looks_like_markdown(string_view probe) {
  const std::vector<string_view> lines = lines_of(probe, 400);
  if (!lines.empty() && lines.front() == "---") return true;  // front matter
  int list_lines = 0;
  for (string_view line : lines) {
    if (markdown_heading(line) || starts_with(line, "```") ||
        markdown_table_rule(line) || markdown_link(line)) {
      return true;
    }
    if (markdown_list_line(line)) list_lines++;
  }
  return list_lines >= 2;
}

std::string sniff_markup(string_view text) {
  if (starts_with_nocase(text, "<!doctype html") || starts_with_nocase(text, "<html")) {
    return "text/html";
  }
  if (starts_with(text, "<?xml")) {
    const string_view rest = text.substr(5);
    if (contains_nocase(rest, "<html")) return "application/xhtml+xml";
    if (contains_nocase(rest, "<svg")) return "image/svg+xml";
    return "application/xml";
  }
  if (starts_with_nocase(text, "<svg")) return "image/svg+xml";
  if (text.front() == '<' && (contains_nocase(text, "<html") ||
                              contains_nocase(text, "<body") ||
                              contains_nocase(text, "<head"))) {
    return "text/html";
  }
  return {};
}

std::string sniff_text(string_view bytes) {
  const string_view probe = bytes.substr(0, kTextProbeBytes);
  if (!looks_like_text(probe)) return {};
  const string_view text = strip_bom_and_space(probe);
  if (text.empty()) return {};
  if (const std::string markup = sniff_markup(text); !markup.empty()) return markup;
  if (looks_like_mail(text)) return "message/rfc822";
  if (text.front() == '{' || text.front() == '[') {
    const string_view whole = strip_bom_and_space(bytes);
    size_t end = whole.find_last_not_of(" \t\r\n");
    if (end != string_view::npos &&
        ((whole.front() == '{' && whole[end] == '}') ||
         (whole.front() == '[' && whole[end] == ']'))) {
      return "application/json";
    }
  }
  if (looks_like_markdown(text)) return "text/markdown";
  return "text/plain";
}

}  // namespace

std::string sniff_mimetype(string_view bytes) {
  if (bytes.empty()) return {};
  if (const std::string binary = sniff_binary(bytes); !binary.empty()) return binary;
  if (starts_with(bytes, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1")) return {};
  return sniff_text(bytes);
}

std::string extension_mimetype(const std::filesystem::path& filename) {
  // The wiki storage dialect names itself by suffix, and its own content
  // type is what the document's origin must carry: a ".storage.xhtml" body
  // is not the plain XHTML its final extension would otherwise claim.
  if (confluence_storage_format(filename.string(), std::string())) {
    return kConfluenceStorageMimetype;
  }
  static const std::map<std::string, std::string> kByExtension = {
      {".pdf", "application/pdf"},
      {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},
      {".tif", "image/tiff"},
      {".tiff", "image/tiff"},
      {".png", "image/png"},
      {".gif", "image/gif"},
      {".webp", "image/webp"},
      {".bmp", "image/bmp"},
      {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
      {".odt", "application/vnd.oasis.opendocument.text"},
      {".ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {".odp", "application/vnd.oasis.opendocument.presentation"},
      {".doc", "application/msword"},
      {".xls", "application/vnd.ms-excel"},
      {".ppt", "application/vnd.ms-powerpoint"},
      {".csv", "text/csv"},
      {".rtf", "application/rtf"},
      {".epub", "application/epub+zip"},
      {".eml", "message/rfc822"},
      {".msg", "application/vnd.ms-outlook"},
      {".xml", "application/xml"},
      {".nxml", "application/xml"},
      {".xbrl", "application/xml"},
      {".html", "text/html"},
      {".htm", "text/html"},
      {".xhtml", "application/xhtml+xml"},
      {".md", "text/markdown"},
      {".markdown", "text/markdown"},
      {".txt", "text/plain"},
      {".json", "application/json"},
      {".warc", "application/warc"},
      {".mp3", "audio/mpeg"},
      {".wav", "audio/wav"},
      {".m4a", "audio/mp4"},
      {".flac", "audio/flac"},
      {".ogg", "audio/ogg"},
      {".oga", "audio/ogg"},
      {".opus", "audio/ogg"},
      {".mp4", "video/mp4"},
      {".m4v", "video/mp4"},
      {".mkv", "video/x-matroska"},
      {".webm", "video/webm"},
      {".mov", "video/quicktime"},
  };
  std::string extension = filename.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  const auto found = kByExtension.find(extension);
  // An extension nothing above recognizes must not masquerade as anything.
  return found != kByExtension.end() ? found->second : "application/octet-stream";
}

MimetypeResolution resolve_mimetype(string_view declared_content_type,
                                    string_view bytes,
                                    const std::filesystem::path& filename) {
  constexpr string_view kOctetStream = "application/octet-stream";
  // A declared type is trimmed of its parameters ("text/html; charset=utf-8")
  // because the origin names a type, not a transport header.
  string_view declared = declared_content_type;
  if (const size_t semicolon = declared.find(';'); semicolon != string_view::npos) {
    declared = declared.substr(0, semicolon);
  }
  while (!declared.empty() && std::isspace(static_cast<unsigned char>(declared.back())) != 0) {
    declared.remove_suffix(1);
  }
  if (!declared.empty() && declared != kOctetStream) {
    return {std::string(declared), "declared"};
  }
  // The wiki storage dialect is named by its suffix and looks like any
  // markup fragment to a sniff, so its name is its declaration.
  if (confluence_storage_format(filename.string(), std::string())) {
    return {kConfluenceStorageMimetype, "extension"};
  }
  std::string sniffed = sniff_mimetype(bytes);
  std::string by_name = extension_mimetype(filename);
  // The bytes outrank the name, except that "text/plain" is what the text
  // ladder says when it recognises nothing in particular: a name that names
  // a specific text format (.csv, .md, .vtt) knows more than that.
  if (sniffed == "text/plain" && by_name != kOctetStream && by_name != "text/plain" &&
      by_name.starts_with("text/")) {
    return {std::move(by_name), "extension"};
  }
  if (!sniffed.empty()) return {std::move(sniffed), "magic"};
  if (by_name != kOctetStream) return {std::move(by_name), "extension"};
  return {std::string(kOctetStream), "fallback"};
}

}  // namespace grparse
