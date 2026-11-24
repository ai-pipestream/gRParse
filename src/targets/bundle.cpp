#include "bundle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/json_util.h>

#include "grparse/image_ref.h"
#include "grparse/content_sniff.h"
#include "grparse/document_render.h"
#include "sha256.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse::targets {
namespace {

// What the manifest names as the producer of the bundle.  The version is the
// project() line in CMakeLists.txt, which is source, not build environment:
// the manifest is part of the bundle's bytes, so two servers built from this
// source must stamp the same string, and they still do.
#ifndef GRPARSE_VERSION
#define GRPARSE_VERSION "unversioned"
#endif
constexpr std::string_view kGenerator = "grparse/" GRPARSE_VERSION;


// What a member holding bytes of a type the bundle cannot name is called.
// It claims nothing about the bytes, which is the point.
constexpr std::string_view kUnknownTypeExtension = "bin";

// A message in its own wire form.  Deterministic serialization is what makes
// the bundle reproducible: without it map fields serialize in whatever order
// the hash table happens to hold them, and two runs over the same document
// disagree.
std::string serialize(const google::protobuf::MessageLite& message, std::string_view what) {
  std::string encoded;
  {
    google::protobuf::io::StringOutputStream sink(&encoded);
    google::protobuf::io::CodedOutputStream stream(&sink);
    stream.SetSerializationDeterministic(true);
    if (!message.SerializeToCodedStream(&stream)) {
      throw std::runtime_error("bundle could not serialize the " + std::string(what));
    }
  }
  return encoded;
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

// The image types a member is named for: a media type and the extension a
// file of that type carries.  The table is closed on purpose.  A type that
// is not in it gets no invented extension; its member is written as
// kUnknownTypeExtension and reported.
struct ImageType {
  std::string_view mimetype;
  std::string_view extension;
};
constexpr std::array<ImageType, 14> kImageTypes{{
    {"image/png", "png"},
    {"image/jpeg", "jpg"},
    {"image/jpg", "jpg"},
    {"image/tiff", "tif"},
    {"image/webp", "webp"},
    {"image/gif", "gif"},
    {"image/bmp", "bmp"},
    {"image/x-ms-bmp", "bmp"},
    {"image/svg+xml", "svg"},
    {"image/jp2", "jp2"},
    {"image/emf", "emf"},
    {"image/x-emf", "emf"},
    {"image/wmf", "wmf"},
    {"image/x-wmf", "wmf"},
}};

// The extension kImageTypes gives `mimetype`; empty when the table does not
// hold it.
std::string_view image_extension(std::string_view mimetype) {
  const auto found = std::ranges::find(kImageTypes, mimetype, &ImageType::mimetype);
  return found == kImageTypes.end() ? std::string_view() : found->extension;
}

// What one ImageRef gives the bundle: the bytes and the extension of the
// member to write, or the reason there is none.
struct ImageMember {
  std::string bytes;
  // Empty when no member is written.
  std::string extension;
  // UNSPECIFIED when the image is written under a type the bundle knows;
  // anything else is reported to the caller.
  parsev1::BundleImageIssueReason reason = parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNSPECIFIED;
  // The data URI's own media type, else the ImageRef's.
  std::string declared_mimetype;
};

// Reads the image behind `image`.  The bytes are ImageRef.data, or the
// payload of a "data:<type>;base64,<payload>" URI from a producer that still
// spells them that way; a reference (a path, an http or epub: URI) or an
// empty ref holds none, and a payload that does not decode cannot be
// written.  The extension is what the bytes' own signature says, else what
// the document declares (a data URI's media type before the ImageRef's),
// and never a guess: bytes neither can name are written as
// kUnknownTypeExtension.
ImageMember read_image(const docv1::ImageRef& image) {
  ImageMember member;
  HeldImage held = held_image(image);
  member.declared_mimetype = held.mimetype;
  switch (held.hold) {
    case ImageHold::kNothing:
    case ImageHold::kReference:
      member.reason = parsev1::BUNDLE_IMAGE_ISSUE_REASON_NOT_EMBEDDED;
      return member;
    case ImageHold::kUndecodable:
      member.reason = parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNDECODABLE;
      return member;
    case ImageHold::kBytes:
      break;
  }
  member.bytes = std::move(held.bytes);
  const std::string referenced_type = media_type_name(image.mimetype());
  for (const std::string& mimetype :
       {sniff_mimetype(member.bytes), held.mimetype, referenced_type}) {
    if (const std::string_view extension = image_extension(mimetype); !extension.empty()) {
      member.extension = extension;
      return member;
    }
  }
  member.extension = kUnknownTypeExtension;
  member.reason = parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNKNOWN_TYPE;
  return member;
}

// Writes `image` as the member `directory`/`stem`_NNNN.<extension> and
// returns the issue to report for it, with its reason, declared type and
// member path filled in, or nullptr when the image was written under a type
// the bundle knows.  The caller names the page or picture on the issue.
parsev1::BundleImageIssue* add_image(const docv1::ImageRef& image, std::string_view directory,
                                     std::string_view stem, int number,
                                     std::vector<BundleFile>* files,
                                     parsev1::TargetResult* result) {
  ImageMember member = read_image(image);
  std::string path;
  if (!member.extension.empty()) {
    path = numbered(directory, stem, number, member.extension);
    files->push_back({path, std::move(member.bytes)});
  }
  if (member.reason == parsev1::BUNDLE_IMAGE_ISSUE_REASON_UNSPECIFIED) return nullptr;
  parsev1::BundleImageIssue* issue = result->add_image_issues();
  issue->set_reason(member.reason);
  issue->set_declared_mimetype(member.declared_mimetype);
  if (!path.empty()) issue->set_path(path);
  return issue;
}

// The bundle's own account of the conversion, the member response.pb: what
// the response carries beside the members that already hold the document
// and its exports.  Left out are the clock (processing_time, timings,
// profiling), which would make two runs over one document disagree byte for
// byte, and the target result's archive and objects, which do not exist
// until this bundle is delivered.
parsev1::ConvertDocumentResponse bundled_response(
    const parsev1::ConvertDocumentResponse& converted, const parsev1::TargetResult& result) {
  parsev1::ConvertDocumentResponse response;
  response.mutable_document()->set_filename(converted.document().filename());
  *response.mutable_errors() = converted.errors();
  response.set_status(converted.status());
  if (converted.has_status_raw()) response.set_status_raw(converted.status_raw());
  if (converted.has_confidence()) *response.mutable_confidence() = converted.confidence();
  *response.mutable_chunks() = converted.chunks();
  *response.mutable_structure_findings() = converted.structure_findings();
  *response.mutable_text_offsets() = converted.text_offsets();
  *response.mutable_option_notices() = converted.option_notices();
  // Set even when empty: a present target_result with no issue says every
  // image was written, where an absent one would say nothing.
  *response.mutable_target_result()->mutable_image_issues() = result.image_issues();
  return response;
}

// The manifest of `files`, which the caller has sorted: every member with its
// digest and size, the generator and the schema version.  No timestamp:
// everything that could make two identical conversions disagree byte for
// byte is deliberately absent.
parsev1::BundleManifest manifest_of(const std::vector<BundleFile>& files) {
  parsev1::BundleManifest manifest;
  for (const auto& file : files) {
    parsev1::BundleMember* member = manifest.add_files();
    member->set_path(file.path);
    member->set_sha256(sha256_hex(file.bytes));
    member->set_size_bytes(file.bytes.size());
  }
  manifest.set_generator(std::string(kGenerator));
  manifest.set_schema_version(kManifestSchemaVersion);
  return manifest;
}

// manifest.json: the manifest in protobuf's own JSON mapping, for people.
// The printer does the escaping and the structure, so the text is JSON
// whatever a path holds, and it can only say what manifest.pb says.  The
// three options keep the bytes manifest.json has always had: the proto's
// field names as keys (which are also in sorted order, being the field
// order), a member of no bytes still stating "size_bytes":0, and a size
// written as a JSON number instead of the quoted form the mapping gives
// 64-bit integers.  No whitespace and no trailing newline.
std::string render_manifest_json(const parsev1::BundleManifest& manifest) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  options.always_print_fields_with_no_presence = true;
  options.unquote_int64_if_possible = true;
  std::string json;
  const auto status = google::protobuf::util::MessageToJsonString(manifest, &json, options);
  if (!status.ok()) {
    throw std::runtime_error("bundle could not render the manifest as JSON: " +
                             std::string(status.message()));
  }
  return json;
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
  if (exports.has_dclx()) {
    files->push_back({"exports/document.dclx", exports.dclx()});
  }
}

}  // namespace

std::vector<BundleFile> build_bundle(const parsev1::ConvertDocumentResponse& converted,
                                     parsev1::TargetResult* result) {
  const docv1::Document& document = converted.document().doc();
  const parsev1::DocumentExports& exports = converted.document().exports();
  // The list is this bundle's alone: response.pb repeats it, and an issue
  // left over from an earlier bundle would be a claim about the wrong one.
  result->clear_image_issues();
  std::vector<BundleFile> files;
  files.push_back({"document.pb", serialize(document, "document")});
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
    if (auto* issue = add_image(page->image(), "pages", "page", page_no, &files, result)) {
      issue->set_page_no(page_no);
    }
  }

  // Pictures are numbered by their position in the document's arena, which
  // is the order every reference into it already assumes.
  for (int index = 0; index < document.pictures_size(); ++index) {
    const auto& picture = document.pictures(index);
    if (!picture.has_image()) continue;
    if (auto* issue =
            add_image(picture.image(), "pictures", "pic", index + 1, &files, result)) {
      issue->set_picture_index(index);
    }
  }

  // After the images, so the account names the ones the bundle left out.
  files.push_back({"response.pb", serialize(bundled_response(converted, *result), "response")});

  std::ranges::sort(files, {}, &BundleFile::path);
  // The manifest describes every other member, so it is built last and
  // describes neither of its own two forms: manifest.pb, the typed message a
  // program reads, and manifest.json, the same message rendered for people.
  // Sorted order puts both after "exports/" and "document.*" and before
  // "pages/", which is where a reader finds them.
  const parsev1::BundleManifest manifest = manifest_of(files);
  for (BundleFile form : {BundleFile{"manifest.json", render_manifest_json(manifest)},
                          BundleFile{"manifest.pb", serialize(manifest, "manifest")}}) {
    files.insert(std::ranges::upper_bound(files, form.path, {}, &BundleFile::path),
                 std::move(form));
  }
  return files;
}

}  // namespace grparse::targets
