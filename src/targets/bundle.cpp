#include "bundle.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "grparse/base64.h"
#include "grparse/document_render.h"
#include "sha256.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse::targets {
namespace {

// What the manifest names as the producer of the bundle.  A literal rather
// than anything derived at build time: the manifest is part of the bundle's
// bytes, so two servers built from this source must stamp the same string.
constexpr std::string_view kGenerator = "grparse/0.1.0";

constexpr std::string_view kDataUriPrefix = "data:";
constexpr std::string_view kBase64Marker = ";base64,";

// The Document in its own wire form.  Deterministic serialization is what
// makes the bundle reproducible: without it map fields serialize in whatever
// order the hash table happens to hold them, and two runs over the same
// document disagree.
std::string serialize_document(const docv1::Document& document) {
  std::string encoded;
  {
    google::protobuf::io::StringOutputStream sink(&encoded);
    google::protobuf::io::CodedOutputStream stream(&sink);
    stream.SetSerializationDeterministic(true);
    if (!document.SerializeToCodedStream(&stream)) {
      throw std::runtime_error("bundle could not serialize the document");
    }
  }
  return encoded;
}

// The bytes behind a "data:<mimetype>;base64,<payload>" URI, plus the file
// extension its mimetype implies.  Anything else (an http URI, an unset
// image, a payload that is not base64) contributes no file rather than a
// broken one.
bool decode_data_uri(const std::string& uri, std::string* bytes, std::string* extension) {
  if (!uri.starts_with(kDataUriPrefix)) return false;
  const size_t marker = uri.find(kBase64Marker);
  if (marker == std::string::npos) return false;
  const std::string mimetype = uri.substr(kDataUriPrefix.size(), marker - kDataUriPrefix.size());
  try {
    *bytes = decode_base64(uri.substr(marker + kBase64Marker.size()));
  } catch (const std::exception&) {
    return false;
  }
  if (mimetype == "image/jpeg") {
    *extension = "jpg";
  } else if (mimetype == "image/tiff") {
    *extension = "tif";
  } else if (mimetype == "image/webp") {
    *extension = "webp";
  } else {
    // PNG is what every in-process renderer emits, and it is the sane
    // fallback for a mimetype this build does not know by name.
    *extension = "png";
  }
  return true;
}

// Zero-padded to four digits, widening rather than truncating past 9999 so a
// long document keeps sorting its members in page order.
std::string numbered(std::string_view directory, std::string_view stem, int number,
                     std::string_view extension) {
  std::string digits = std::to_string(number);
  if (digits.size() < 4) digits.insert(0, 4 - digits.size(), '0');
  return std::string(directory) + "/" + std::string(stem) + "_" + digits + "." +
         std::string(extension);
}

// JSON string escaping for the manifest.  The manifest holds generated file
// names and hex digests, but escaping is not conditional on that: a document
// name reaching a path would otherwise be able to break the JSON.
std::string json_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (byte < 0x20) {
          static constexpr char kDigits[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(kDigits[byte >> 4]);
          escaped.push_back(kDigits[byte & 0x0f]);
        } else {
          escaped.push_back(character);
        }
        break;
    }
  }
  return escaped;
}

// The manifest: object keys in sorted order, no whitespace, no timestamp, no
// trailing newline.  Everything that could make two identical conversions
// disagree byte for byte is deliberately absent.
std::string render_manifest(const std::vector<BundleFile>& files) {
  std::string manifest = "{\"files\":[";
  bool first = true;
  for (const auto& file : files) {
    if (!first) manifest += ',';
    first = false;
    manifest += "{\"path\":\"";
    manifest += json_escape(file.path);
    manifest += "\",\"sha256\":\"";
    manifest += sha256_hex(file.bytes);
    manifest += "\",\"size_bytes\":";
    manifest += std::to_string(file.bytes.size());
    manifest += '}';
  }
  manifest += "],\"generator\":\"";
  manifest += json_escape(kGenerator);
  manifest += "\",\"schema_version\":";
  manifest += std::to_string(kManifestSchemaVersion);
  manifest += '}';
  return manifest;
}

// The exports the bundle carries, each under the name of the output format
// that produced it.  Only the fields the request actually rendered are set,
// so an unasked-for format contributes no empty file.
void add_exports(const parsev1::DocumentExports& exports, std::vector<BundleFile>* files) {
  const std::array<std::pair<std::string_view, const std::string*>, 12> members{{
      {"exports/canonical_json.json", exports.has_canonical_json() ? &exports.canonical_json() : nullptr},
      {"exports/doclang.xml", exports.has_doclang() ? &exports.doclang() : nullptr},
      {"exports/doctags.txt", exports.has_doctags() ? &exports.doctags() : nullptr},
      {"exports/gdocs.json", exports.has_gdocs_json() ? &exports.gdocs_json() : nullptr},
      {"exports/html.html", exports.has_html() ? &exports.html() : nullptr},
      {"exports/html_split_page.html", exports.has_html_split_page() ? &exports.html_split_page() : nullptr},
      {"exports/json.json", exports.has_json() ? &exports.json() : nullptr},
      {"exports/latex.tex", exports.has_latex() ? &exports.latex() : nullptr},
      {"exports/markdown.md", exports.has_md() ? &exports.md() : nullptr},
      {"exports/text.txt", exports.has_text() ? &exports.text() : nullptr},
      {"exports/vtt.vtt", exports.has_vtt() ? &exports.vtt() : nullptr},
      {"exports/yaml.yaml", exports.has_yaml() ? &exports.yaml() : nullptr},
  }};
  for (const auto& [path, value] : members) {
    if (value != nullptr) files->push_back({std::string(path), *value});
  }
}

}  // namespace

std::vector<BundleFile> build_bundle(const docv1::Document& document,
                                     const parsev1::DocumentExports& exports) {
  std::vector<BundleFile> files;
  files.push_back({"document.pb", serialize_document(document)});
  // The canonical JSON is the bundle's readable form of the same document.
  // Reusing the export when the request already asked for it keeps a fold
  // that walks the whole document from running twice.
  files.push_back({"document.json", exports.has_canonical_json()
                                        ? exports.canonical_json()
                                        : render_canonical_json(document)});
  add_exports(exports, &files);

  // std::map, not the proto map: page images must land in page order for the
  // bundle to be reproducible, and a proto map iterates in hash order.
  const std::map<int32_t, const docv1::PageItem*> pages = [&document] {
    std::map<int32_t, const docv1::PageItem*> ordered;
    for (const auto& [page_no, page] : document.pages()) ordered.emplace(page_no, &page);
    return ordered;
  }();
  for (const auto& [page_no, page] : pages) {
    if (!page->has_image()) continue;
    std::string bytes;
    std::string extension;
    if (!decode_data_uri(page->image().uri(), &bytes, &extension)) continue;
    files.push_back({numbered("pages", "page", page_no, extension), std::move(bytes)});
  }

  // Pictures are numbered by their position in the document's arena, which
  // is the order every reference into it already assumes.
  for (int index = 0; index < document.pictures_size(); ++index) {
    const auto& picture = document.pictures(index);
    if (!picture.has_image()) continue;
    std::string bytes;
    std::string extension;
    if (!decode_data_uri(picture.image().uri(), &bytes, &extension)) continue;
    files.push_back({numbered("pictures", "pic", index + 1, extension), std::move(bytes)});
  }

  std::ranges::sort(files, {}, &BundleFile::path);
  // The manifest describes every other member, so it is hashed last and
  // never describes itself.  Sorted order puts it after "exports/" and
  // "document.*" and before "pages/", which is where a reader finds it.
  BundleFile manifest{"manifest.json", render_manifest(files)};
  files.insert(std::ranges::upper_bound(files, manifest.path, {}, &BundleFile::path),
               std::move(manifest));
  return files;
}

}  // namespace grparse::targets
