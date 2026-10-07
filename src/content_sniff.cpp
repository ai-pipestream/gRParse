#include "grparse/content_sniff.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
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

// The archive's entry names, read from its central directory; nullopt when
// the bytes carry no central directory this can read (a truncated upload, a
// ZIP64 archive, a hand-built stub).
//
// A byte search for a part name is not enough on its own: an OOXML package
// may embed another package stored uncompressed (a deck carries the
// workbook behind each of its charts under ppt/embeddings/), and that inner
// package's own "xl/workbook.xml" sits in the outer archive's bytes.
std::optional<std::vector<string_view>> zip_central_names(string_view bytes) {
  constexpr size_t kEndRecord = 22;
  constexpr size_t kCentralHeader = 46;
  constexpr size_t kMaxEntries = 65535;
  if (bytes.size() < kEndRecord) return std::nullopt;
  // The end record sits in the last 22 bytes plus a comment of at most 64K.
  const size_t floor =
      bytes.size() > kEndRecord + 65535 ? bytes.size() - kEndRecord - 65535 : 0;
  size_t end = string_view::npos;
  for (size_t at = bytes.size() - kEndRecord + 1; at-- > floor;) {
    if (bytes.substr(at, 4) == "PK\x05\x06") {
      end = at;
      break;
    }
  }
  if (end == string_view::npos) return std::nullopt;
  const uint32_t entries = le16(bytes, end + 10);
  const uint32_t directory_size = le32(bytes, end + 12);
  const uint32_t directory_at = le32(bytes, end + 16);
  if (directory_at == 0xFFFFFFFFU || static_cast<size_t>(directory_at) + directory_size > end) {
    return std::nullopt;
  }
  std::vector<string_view> names;
  size_t at = directory_at;
  for (uint32_t i = 0; i < entries && i < kMaxEntries; i++) {
    if (at + kCentralHeader > end || bytes.substr(at, 4) != "PK\x01\x02") return std::nullopt;
    const size_t name_length = le16(bytes, at + 28);
    const size_t skip = name_length + le16(bytes, at + 30) + le16(bytes, at + 32);
    if (at + kCentralHeader + name_length > end) return std::nullopt;
    names.push_back(bytes.substr(at + kCentralHeader, name_length));
    at += kCentralHeader + skip;
  }
  return names;
}

// True when the archive lists an entry whose name starts with `entry`: an
// exact part name, or a prefix such as "Index/Slide". Without a readable
// central directory, a byte search of the archive's head and its tail
// stands in for the listing.
bool zip_names_entry(string_view bytes,
                     const std::optional<std::vector<string_view>>& names,
                     string_view entry) {
  if (names.has_value()) {
    return std::ranges::any_of(*names,
                               [entry](string_view name) { return name.starts_with(entry); });
  }
  const string_view head = bytes.substr(0, kZipDirectoryBytes);
  if (head.find(entry) != string_view::npos) return true;
  if (bytes.size() <= kZipDirectoryBytes) return false;
  const string_view tail = bytes.substr(bytes.size() - kZipDirectoryBytes);
  return tail.find(entry) != string_view::npos;
}

std::string sniff_zip(string_view bytes) {
  const std::string entry = zip_mimetype_entry(bytes);
  if (!entry.empty()) return entry;
  const auto names = zip_central_names(bytes);
  const auto names_entry = [&](string_view part) { return zip_names_entry(bytes, names, part); };
  if (names_entry("word/document.xml")) {
    return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  }
  if (names_entry("xl/workbook.xml")) {
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  }
  if (names_entry("ppt/presentation.xml")) {
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  }
  if (names_entry("META-INF/container.xml")) {
    return "application/epub+zip";
  }
  // The iWork '13+ container: every app's document opens at
  // Index/Document.iwa, and only a Keynote deck carries slide archives.
  // Pages and Numbers cannot be told apart by entry names (both hold
  // CalculationEngine.iwa and a Tables/ directory); the name decides them
  // (resolve_mimetype).
  if (names_entry("Index/Document.iwa") &&
      (names_entry("Index/MasterSlide") || names_entry("Index/Slide"))) {
    return "application/vnd.apple.keynote";
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
  // Two of the headers mail carries, not one: a message copied out of a mail
  // client into a text file keeps a "From:" line over a "Sent:" line, which
  // is a display block, not a header block a mail transfer agent wrote.
  return header_lines >= 2 && mail_headers >= 2;
}

// A saved web archive: an rfc822 header block whose Content-Type names the
// multipart/related aggregate. Read before the mail rule, which the same
// header block would otherwise satisfy.
bool looks_like_mhtml(string_view probe) {
  if (!looks_like_mail(probe)) return false;
  for (string_view line : lines_of(probe, 40)) {
    if (line.empty()) break;
    if (!starts_with_nocase(line, "content-type:")) continue;
    std::string value(line.substr(std::string_view("content-type:").size()));
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    return value.find("multipart/related") != std::string::npos;
  }
  return false;
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

// True for a body in a single-byte encoding (Latin-1, Windows-1252): not
// UTF-8, but free of NUL and of control bytes other than the whitespace
// family, with high bytes no more than a quarter of the probe. Binary
// formats fail on their control bytes long before the ratio matters.
bool looks_like_single_byte_text(string_view probe) {
  size_t high = 0;
  for (const char c : probe) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f') ||
        byte == 0x7F) {
      return false;
    }
    if (byte >= 0x80) high++;
  }
  return high * 4 <= probe.size();
}

// The field count of each delimited record in `text`, a quoted field (with
// doubled quotes inside it) counting as one field whatever delimiters and
// line breaks it holds. Blank lines are skipped; a final record the probe
// cut short (`truncated`) or whose quote never closes is dropped.
std::vector<size_t> delimited_records(string_view text, char delimiter, bool truncated) {
  std::vector<size_t> records;
  size_t fields = 1;
  bool quoted = false;
  bool blank = true;
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '"') {
      blank = false;
      if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
        i++;
      } else {
        quoted = !quoted;
      }
    } else if (quoted) {
      continue;
    } else if (c == delimiter) {
      blank = false;
      fields++;
    } else if (c == '\n') {
      if (!blank) records.push_back(fields);
      fields = 1;
      blank = true;
    } else if (c != '\r') {
      blank = false;
    }
  }
  if (!blank && !quoted && !truncated) records.push_back(fields);
  return records;
}

// Comma (or semicolon) separated values: every record with the same field
// count, over at least three records of two or more fields, or a header and
// one row of four or more. Prose with commas almost never keeps one count.
bool looks_like_csv(string_view probe, bool truncated) {
  for (const char delimiter : {',', ';'}) {
    const std::vector<size_t> records = delimited_records(probe, delimiter, truncated);
    if (records.size() < 2) continue;
    const size_t fields = records.front();
    const bool enough = records.size() >= 3 ? fields >= 2 : fields >= 4;
    if (enough && std::ranges::all_of(records, [&](size_t count) { return count == fields; })) {
      return true;
    }
  }
  return false;
}

std::string sniff_text(string_view bytes) {
  const string_view probe = bytes.substr(0, kTextProbeBytes);
  if (!looks_like_text(probe) && !looks_like_single_byte_text(probe)) return {};
  const string_view text = strip_bom_and_space(probe);
  if (text.empty()) return {};
  if (const std::string markup = sniff_markup(text); !markup.empty()) return markup;
  if (looks_like_mhtml(text)) return "multipart/related";
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
  if (looks_like_csv(text, bytes.size() > kTextProbeBytes)) return "text/csv";
  return "text/plain";
}

// ---- OLE compound files -----------------------------------------------------
// Enough of [MS-CFB] to walk a compound file's directory from its root and
// read the head of a stream: the FAT (through the header's DIFAT entries and
// the DIFAT chain), the directory chain, the mini FAT and the mini stream
// that holds every stream under the cutoff. Every read is bounds-checked and
// every chain walk is capped at the number of sectors the file holds, so a
// truncated or looping file ends the walk instead of the process.
namespace cfb {

constexpr string_view kSignature = "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1";
constexpr size_t kHeaderBytes = 512;
constexpr size_t kDirectoryEntryBytes = 128;
constexpr size_t kHeaderDifatEntries = 109;
constexpr uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr uint32_t kNoStream = 0xFFFFFFFF;
constexpr uint32_t kMaxRegularSector = 0xFFFFFFFA;
constexpr uint8_t kStorageObject = 1;
constexpr uint8_t kStreamObject = 2;
constexpr uint8_t kRootStorage = 5;

uint16_t u16(string_view bytes, size_t at) {
  if (at + 2 > bytes.size()) return 0;
  return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at]) |
                               (static_cast<uint8_t>(bytes[at + 1]) << 8));
}

uint32_t u32(string_view bytes, size_t at) {
  if (at + 4 > bytes.size()) return 0;
  return static_cast<uint32_t>(u16(bytes, at)) | (static_cast<uint32_t>(u16(bytes, at + 2)) << 16);
}

// One directory entry, with its UTF-16LE name narrowed to the ASCII Office
// uses for the names that matter here (a non-ASCII unit becomes '?').
struct Entry {
  std::string name;
  uint8_t type = 0;
  uint32_t left = kNoStream;
  uint32_t right = kNoStream;
  uint32_t child = kNoStream;
  uint32_t start = kEndOfChain;
  uint64_t size = 0;
};

class File {
 public:
  // Parses the header and the FAT; `ok()` is false for anything that is not
  // a compound file this reader can walk.
  explicit File(string_view bytes) : bytes_(bytes) {
    if (!starts_with(bytes, kSignature) || bytes.size() < kHeaderBytes) return;
    const unsigned sector_shift = u16(bytes, 30);
    const unsigned mini_shift = u16(bytes, 32);
    if ((sector_shift != 9 && sector_shift != 12) || mini_shift != 6) return;
    sector_bytes_ = size_t{1} << sector_shift;
    mini_cutoff_ = u32(bytes, 56);
    sector_count_ = bytes.size() / sector_bytes_;  // the header counts as one
    if (!read_fat()) return;
    const std::vector<uint32_t> directory = chain(u32(bytes, 48));
    if (directory.empty()) return;
    directory_ = directory;
    mini_fat_ = chain_values(u32(bytes, 60), u32(bytes, 64));
    const std::optional<Entry> root = entry(0);
    if (!root.has_value() || root->type != kRootStorage) return;
    mini_stream_ = chain(root->start);
    mini_stream_bytes_ = root->size;
    root_ = *root;
    ok_ = true;
  }

  bool ok() const { return ok_; }

  // The entries directly under the root storage, in tree order.
  std::vector<Entry> root_children() const {
    std::vector<Entry> children;
    if (!ok_) return children;
    std::vector<uint32_t> pending{root_.child};
    size_t visited = 0;
    const size_t capacity = directory_.size() * (sector_bytes_ / kDirectoryEntryBytes);
    while (!pending.empty() && visited < capacity) {
      const uint32_t id = pending.back();
      pending.pop_back();
      if (id == kNoStream) continue;
      const std::optional<Entry> node = entry(id);
      if (!node.has_value()) continue;
      ++visited;
      pending.push_back(node->left);
      pending.push_back(node->right);
      children.push_back(*node);
    }
    return children;
  }

  // The first `limit` bytes of a stream (fewer when the stream is shorter),
  // from the mini stream when the stream is under the cutoff.
  std::string head(const Entry& stream, size_t limit) const {
    std::string out;
    if (!ok_ || stream.type != kStreamObject) return out;
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(stream.size, limit));
    if (wanted == 0) return out;
    if (stream.size < mini_cutoff_) {
      constexpr size_t kMiniBytes = 64;
      uint32_t mini = stream.start;
      size_t steps = 0;
      while (mini <= kMaxRegularSector && out.size() < wanted && steps++ < mini_fat_.size()) {
        const uint64_t offset = uint64_t{mini} * kMiniBytes;
        if (offset + kMiniBytes > mini_stream_bytes_) break;
        const size_t index = static_cast<size_t>(offset / sector_bytes_);
        if (index >= mini_stream_.size()) break;
        const string_view sector = sector_bytes(mini_stream_[index]);
        const size_t within = static_cast<size_t>(offset % sector_bytes_);
        if (sector.size() < within + kMiniBytes) break;
        out.append(sector.substr(within, std::min(kMiniBytes, wanted - out.size())));
        if (mini >= mini_fat_.size()) break;
        mini = mini_fat_[mini];
      }
      return out;
    }
    for (const uint32_t sector_id : chain(stream.start)) {
      const string_view sector = sector_bytes(sector_id);
      if (sector.empty()) break;
      out.append(sector.substr(0, std::min(sector.size(), wanted - out.size())));
      if (out.size() >= wanted) break;
    }
    return out;
  }

 private:
  string_view sector_bytes(uint32_t id) const {
    if (id > kMaxRegularSector) return {};
    const uint64_t offset = (uint64_t{id} + 1) * sector_bytes_;
    if (offset + sector_bytes_ > bytes_.size()) return {};
    return bytes_.substr(static_cast<size_t>(offset), sector_bytes_);
  }

  bool read_fat() {
    std::vector<uint32_t> fat_sectors;
    const size_t declared = u32(bytes_, 44);
    for (size_t i = 0; i < kHeaderDifatEntries && fat_sectors.size() < declared; ++i) {
      const uint32_t id = u32(bytes_, 76 + 4 * i);
      if (id > kMaxRegularSector) break;
      fat_sectors.push_back(id);
    }
    uint32_t difat = u32(bytes_, 68);
    const size_t per_difat = sector_bytes_ / 4 - 1;
    size_t difat_steps = 0;
    while (difat <= kMaxRegularSector && fat_sectors.size() < declared &&
           difat_steps++ < sector_count_) {
      const string_view sector = sector_bytes(difat);
      if (sector.empty()) break;
      for (size_t i = 0; i < per_difat && fat_sectors.size() < declared; ++i) {
        const uint32_t id = u32(sector, 4 * i);
        if (id > kMaxRegularSector) break;
        fat_sectors.push_back(id);
      }
      difat = u32(sector, 4 * per_difat);
    }
    for (const uint32_t id : fat_sectors) {
      const string_view sector = sector_bytes(id);
      if (sector.empty()) return false;
      for (size_t i = 0; i < sector_bytes_ / 4; ++i) fat_.push_back(u32(sector, 4 * i));
    }
    return !fat_.empty();
  }

  // The sector ids of a chain, in order, capped at the file's sector count.
  std::vector<uint32_t> chain(uint32_t start) const {
    std::vector<uint32_t> ids;
    uint32_t id = start;
    while (id <= kMaxRegularSector && ids.size() < sector_count_) {
      if (id >= fat_.size()) break;
      ids.push_back(id);
      id = fat_[id];
    }
    return ids;
  }

  // The 32-bit entries of the sectors on a chain (the mini FAT).
  std::vector<uint32_t> chain_values(uint32_t start, size_t declared_sectors) const {
    std::vector<uint32_t> values;
    for (const uint32_t id : chain(start)) {
      if (values.size() / (sector_bytes_ / 4) >= declared_sectors) break;
      const string_view sector = sector_bytes(id);
      if (sector.empty()) break;
      for (size_t i = 0; i < sector_bytes_ / 4; ++i) values.push_back(u32(sector, 4 * i));
    }
    return values;
  }

  std::optional<Entry> entry(uint32_t id) const {
    const size_t per_sector = sector_bytes_ / kDirectoryEntryBytes;
    const size_t index = id / per_sector;
    if (index >= directory_.size()) return std::nullopt;
    const string_view sector = sector_bytes(directory_[index]);
    if (sector.empty()) return std::nullopt;
    const string_view raw = sector.substr((id % per_sector) * kDirectoryEntryBytes,
                                          kDirectoryEntryBytes);
    Entry out;
    const size_t name_bytes = std::min<size_t>(u16(raw, 64), 64);
    for (size_t i = 0; i + 1 < name_bytes; i += 2) {
      const uint16_t unit = u16(raw, i);
      if (unit == 0) break;
      out.name.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    out.type = static_cast<uint8_t>(raw[66]);
    out.left = u32(raw, 68);
    out.right = u32(raw, 72);
    out.child = u32(raw, 76);
    out.start = u32(raw, 116);
    // Version 3 files use the low 32 bits only; the high word is unreliable.
    out.size = sector_bytes_ == 512 ? u32(raw, 120)
                                    : (uint64_t{u32(raw, 124)} << 32) | u32(raw, 120);
    return out;
  }

  string_view bytes_;
  bool ok_ = false;
  size_t sector_bytes_ = 512;
  size_t sector_count_ = 0;
  uint32_t mini_cutoff_ = 4096;
  std::vector<uint32_t> fat_;
  std::vector<uint32_t> mini_fat_;
  std::vector<uint32_t> directory_;
  std::vector<uint32_t> mini_stream_;
  uint64_t mini_stream_bytes_ = 0;
  Entry root_;
};

const Entry* stream_named(const std::vector<Entry>& entries, string_view name) {
  for (const Entry& entry : entries) {
    if (entry.type == kStreamObject && entry.name == name) return &entry;
  }
  return nullptr;
}

// [MS-DOC] 2.5.1 FibBase: wIdent at 0 is 0xA5EC for a Word binary file;
// the flags word at 10 has fEncrypted at bit 8 (0x0100). An encrypted
// document's FIB is in the clear; everything after it is not.
bool word_fib_encrypted(const std::string& head) {
  if (head.size() < 12) return false;
  if (u16(head, 0) != 0xA5EC) return false;
  return (u16(head, 10) & 0x0100) != 0;
}

// [MS-XLS] 2.4.117 FilePass: an encrypted workbook's stream opens with a BOF
// record (0x0809) followed, within the file's first records, by FILEPASS
// (0x002F). The record walk stops at EOF (0x000A) or a record that runs off
// the head read here.
bool biff_filepass(const std::string& head) {
  if (head.size() < 4 || u16(head, 0) != 0x0809) return false;
  size_t at = 0;
  for (int records = 0; records < 64 && at + 4 <= head.size(); ++records) {
    const uint16_t type = u16(head, at);
    const uint16_t length = u16(head, at + 2);
    if (type == 0x002F) return true;
    if (type == 0x000A) return false;
    at += 4 + size_t{length};
  }
  return false;
}

// [MS-PPT] 2.3.2 CurrentUserAtom: headerToken at 12 is 0xE391C05F for a
// plain file and 0xF3D1C4DF for an encrypted one.
bool powerpoint_encrypted(const std::string& current_user) {
  return current_user.size() >= 16 && u32(current_user, 12) == 0xF3D1C4DF;
}

}  // namespace cfb

}  // namespace

std::optional<EncryptedOfficeDocument> encrypted_office_document(string_view bytes) {
  const cfb::File file(bytes);
  if (!file.ok()) return std::nullopt;
  const std::vector<cfb::Entry> root = file.root_children();
  if (cfb::stream_named(root, "EncryptedPackage") != nullptr) {
    return EncryptedOfficeDocument{"Office Open XML package",
                                   "EncryptedPackage stream under the root"};
  }
  if (const cfb::Entry* word = cfb::stream_named(root, "WordDocument"); word != nullptr) {
    if (cfb::word_fib_encrypted(file.head(*word, 12))) {
      return EncryptedOfficeDocument{"Word document", "fEncrypted set in the WordDocument FIB"};
    }
  }
  for (const string_view name : {"Workbook", "Book"}) {
    if (const cfb::Entry* workbook = cfb::stream_named(root, name); workbook != nullptr) {
      if (cfb::biff_filepass(file.head(*workbook, 4096))) {
        return EncryptedOfficeDocument{"Excel workbook",
                                       std::string("FILEPASS record in the ") + std::string(name) +
                                           " stream"};
      }
    }
  }
  if (cfb::stream_named(root, "PowerPoint Document") != nullptr) {
    if (const cfb::Entry* user = cfb::stream_named(root, "Current User"); user != nullptr) {
      if (cfb::powerpoint_encrypted(file.head(*user, 16))) {
        return EncryptedOfficeDocument{"PowerPoint presentation",
                                       "encrypted header token in the Current User stream"};
      }
    }
  }
  return std::nullopt;
}

namespace {

// The Office format a compound file holds, read off its root streams; empty
// for any other compound file (an Outlook .msg, a thumbnail cache). An
// encrypted Office Open XML package hides which of docx, xlsx or pptx it is
// until it is decrypted, so it gets the one type that says so.
std::string ole_office_mimetype(string_view bytes) {
  const cfb::File file(bytes);
  if (!file.ok()) return {};
  const std::vector<cfb::Entry> root = file.root_children();
  if (cfb::stream_named(root, "EncryptedPackage") != nullptr) {
    return std::string(kEncryptedOfficePackageMimetype);
  }
  if (cfb::stream_named(root, "WordDocument") != nullptr) return "application/msword";
  if (cfb::stream_named(root, "Workbook") != nullptr || cfb::stream_named(root, "Book") != nullptr) {
    return "application/vnd.ms-excel";
  }
  if (cfb::stream_named(root, "PowerPoint Document") != nullptr) {
    return "application/vnd.ms-powerpoint";
  }
  return {};
}

}  // namespace

std::string sniff_mimetype(string_view bytes) {
  if (bytes.empty()) return {};
  if (const std::string binary = sniff_binary(bytes); !binary.empty()) return binary;
  if (starts_with(bytes, cfb::kSignature)) return ole_office_mimetype(bytes);
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
      {".mht", "application/x-mimearchive"},
      {".mhtml", "application/x-mimearchive"},
      {".pages", "application/vnd.apple.pages"},
      {".numbers", "application/vnd.apple.numbers"},
      {".key", "application/vnd.apple.keynote"},
      {".afp", "application/x-afp"},
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
  // "text/csv" is a guess from field counts alone, so any text name, .txt
  // included, outranks it.
  if (sniffed == "text/csv" && by_name.starts_with("text/")) {
    return {std::move(by_name), "extension"};
  }
  // Likewise a zip the container scan could not place: an iWork name
  // (.pages, .numbers) says which app's document it is, which the entries
  // of a Pages and a Numbers container cannot (sniff_zip).
  if (sniffed == "application/zip" && by_name.starts_with("application/vnd.apple.")) {
    return {std::move(by_name), "extension"};
  }
  // And an encrypted Office Open XML package: its bytes hide which of docx,
  // xlsx or pptx it is, and its name says.
  if (sniffed == kEncryptedOfficePackageMimetype && by_name.contains("openxmlformats")) {
    return {std::move(by_name), "extension"};
  }
  if (!sniffed.empty()) return {std::move(sniffed), "magic"};
  if (by_name != kOctetStream) return {std::move(by_name), "extension"};
  return {std::string(kOctetStream), "fallback"};
}

}  // namespace grparse
