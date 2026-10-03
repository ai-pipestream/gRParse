// Picture image bytes for the exports that carry images (the DocLang XML in
// embedded mode, the DocLang archive): the bytes behind an ImageRef's data
// URI, and docling's fallback of cropping a picture out of its page image.
// Everything happens in memory; nothing here touches a filesystem. Internal
// to src/render.
#ifndef GRPARSE_RENDER_PICTURE_IMAGE_H
#define GRPARSE_RENDER_PICTURE_IMAGE_H

#include <optional>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// An encoded image and the file extension its format implies.
struct EncodedImage {
  std::string bytes;
  std::string extension;  // "png", "jpg", "webp", ...
};

// The bytes behind a "data:<mimetype>;base64,<payload>" URI. Absent for any
// other URI, an empty payload, or a payload that is not base64.
std::optional<EncodedImage> decode_image_data_uri(const std::string& uri);

// The image as an archive member: PNG, JPEG and WebP bytes go through
// unchanged (the formats the DocLang archive's content-types part declares);
// anything else is decoded and re-encoded as PNG. Absent when it does not
// decode.
std::optional<EncodedImage> archive_image(EncodedImage image);

// docling's DocItem.get_image fallback: the picture's first provenance box
// cropped out of its page's data-URI image, as PNG. The box is scaled from
// page units to image pixels by the page size, rounded to whole pixels and,
// unlike docling's crop (which pads outside the raster), clamped to the
// image. Absent when the provenance, page size or page image is missing, the
// page image does not decode, or the crop is empty.
std::optional<std::string> crop_picture_png(const ai::pipestream::document::v1::Document& document,
                                            const ai::pipestream::document::v1::PictureItem& picture);

// "data:image/png;base64,<bytes>".
std::string png_data_uri(const std::string& png);

}  // namespace grparse::render

#endif
