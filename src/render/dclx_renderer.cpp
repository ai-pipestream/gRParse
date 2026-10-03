// DocLang OPC archive (`.dclx`) export; semantics documented on the
// declaration in include/grparse/document_render.h.
//
// The member layout follows docling-core's save_as_doclang_archive and the
// doclang packager it calls: document.xml, pages/<page_no>.<ext>,
// assets/image_<NNNNNN>_<hash>.<ext>, and the packager's own
// [Content_Types].xml and _rels/.rels. Where gRParse differs it is on
// purpose:
//   - Diskless. docling stages every member in a temporary directory and
//     zips the directory; here the members are byte strings handed straight
//     to the deterministic ZIP writer.
//   - Deterministic. docling's zip carries each staged file's mtime; this
//     archive carries the writer's fixed timestamp and sorts members by path
//     (the same order docling's sorted directory walk gives), so the same
//     document always packs to the same bytes.
//   - No re-encode. docling re-saves every image through PIL as PNG; PNG,
//     JPEG and WebP bytes are stored here as they came (all three are
//     declared in the content-types part), so a JPEG photo is not inflated
//     into a PNG and no pixel is touched. Other formats are re-encoded as
//     PNG.
//   - The hash in an asset name is the SHA-256 of the stored bytes, where
//     docling hashes the decoded pixel buffer. Either way it only keeps the
//     name unique; readers follow the uri.
#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../targets/sha256.h"
#include "../targets/zip_writer.h"
#include "doclang_markup.h"
#include "grparse/document_render.h"
#include "picture_image.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

// The OPC parts the doclang packager writes, byte for byte, so a docling
// reader sees the furniture it wrote itself.
constexpr char kContentTypes[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
    "  <Default Extension=\"rels\" "
    "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    "  <Default Extension=\"png\" ContentType=\"image/png\"/>\n"
    "  <Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>\n"
    "  <Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>\n"
    "  <Default Extension=\"webp\" ContentType=\"image/webp\"/>\n"
    "  <Override PartName=\"/document.xml\" "
    "ContentType=\"application/vnd.doclang.document+xml\"/>\n"
    "</Types>\n";

constexpr char kRels[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    "  <Relationship Id=\"rId1\"\n"
    "    Type=\"http://doclang.ai/ns/package/2026/relationships/document\"\n"
    "    Target=\"document.xml\"/>\n"
    "</Relationships>\n";

// docling's asset name: the picture's position in body order, six digits,
// then the image hash.
std::string asset_path(int index, const render::EncodedImage& image) {
  char number[16];
  std::snprintf(number, sizeof(number), "%06d", index);
  return std::string("assets/image_") + number + "_" + targets::sha256_hex(image.bytes) + "." +
         image.extension;
}

}  // namespace

std::string render_dclx(const docv1::Document& document) {
  return render_dclx(document, DoclangOptions{});
}

std::string render_dclx(const docv1::Document& document, const DoclangOptions& options) {
  using Mode = DoclangOptions::ImageMode;
  // docling-core's save_as_doclang_archive default.
  const Mode mode = options.image_mode.value_or(Mode::kReferenced);
  if (mode == Mode::kEmbedded) {
    throw std::invalid_argument(
        "the DocLang archive does not support image_export_mode EMBEDDED: it keeps images "
        "outside the markup; use REFERENCED or PLACEHOLDER");
  }

  std::vector<targets::BundleFile> members;
  render::PictureUri picture_uri;
  int picture_index = 0;
  if (mode == Mode::kReferenced) {
    picture_uri = [&](const docv1::PictureItem& picture) -> std::string {
      // docling counts every picture it walks, stored or not.
      const int index = picture_index++;
      std::optional<render::EncodedImage> image;
      if (picture.has_image() && !picture.image().uri().empty()) {
        const std::string& uri = picture.image().uri();
        auto decoded = render::decode_image_data_uri(uri);
        // An image that cannot be loaded keeps the uri it has, as in
        // docling (an http or relative reference stays a reference).
        if (!decoded.has_value()) return uri;
        image = render::archive_image(std::move(*decoded));
        if (!image.has_value()) return uri;
      } else {
        auto png = render::crop_picture_png(document, picture);
        if (!png.has_value()) return std::string();
        image = render::EncodedImage{std::move(*png), "png"};
      }
      std::string path = asset_path(index, *image);
      members.push_back({path, std::move(image->bytes)});
      return path;
    };
  }
  std::string markup =
      render::render_doclang_markup(document, options.include_namespace, picture_uri);
  markup.push_back('\n');  // docling writes the serialization plus a newline

  members.push_back({"[Content_Types].xml", kContentTypes});
  members.push_back({"_rels/.rels", kRels});
  members.push_back({"document.xml", std::move(markup)});

  // Page images in both modes. std::map: the proto map iterates in hash
  // order, and the archive must not.
  std::map<int, const docv1::PageItem*> pages;
  for (const auto& [page_no, page] : document.pages()) pages.emplace(page_no, &page);
  for (const auto& [page_no, page] : pages) {
    // The packager accepts positive page numbers only.
    if (page_no < 1 || !page->has_image()) continue;
    auto decoded = render::decode_image_data_uri(page->image().uri());
    if (!decoded.has_value()) continue;
    auto image = render::archive_image(std::move(*decoded));
    if (!image.has_value()) continue;
    members.push_back({"pages/" + std::to_string(page_no) + "." + image->extension,
                       std::move(image->bytes)});
  }

  // Path order, as docling's sorted directory walk adds them; it is also
  // what makes the archive a function of the document alone.
  std::ranges::sort(members, {}, &targets::BundleFile::path);
  return targets::write_zip(members);
}

}  // namespace grparse
