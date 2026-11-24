// What an ImageRef holds, read one way everywhere.
//
// A producer that has an image's bytes fills ImageRef.data; `uri` is for a
// reference to an image held elsewhere. Older producers spelled the bytes as
// a base64 data: URI in `uri`, and readers must keep accepting that. Every
// reader of an image goes through held_image(), so none parses a data: URI
// on its own and none misses the bytes when a producer moves to `data`.
//
// gRParse itself never writes the old spelling, and a collector's Document
// is brought to the new one as it is merged (hold_image_bytes), so a
// Document gRParse answers with spells held bytes one way. What is left
// for held_image() to read from a URI is a Document a caller hands in.
#ifndef GRPARSE_IMAGE_REF_H
#define GRPARSE_IMAGE_REF_H

#include <string>
#include <string_view>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

enum class ImageHold {
  // Neither bytes nor a uri.
  kNothing,
  // `uri` names an image held elsewhere (a path, an http or epub: URI).
  kReference,
  // The encoded image: `data`, or the payload of a base64 data: URI.
  kBytes,
  // A data: URI whose payload is empty, is not base64, or does not decode.
  // `data` is never undecodable: it is the bytes.
  kUndecodable,
};

struct HeldImage {
  ImageHold hold = ImageHold::kNothing;
  // The encoded image; filled for kBytes only.
  std::string bytes;
  // The media type the document declares for the image, lower case, blanks
  // trimmed and parameters dropped: a data: URI's own type before
  // ImageRef.mimetype. Empty when neither states one. Filled for every
  // hold, so a reader can say what an undecodable image claimed to be.
  std::string mimetype;
};

// `data` wins when both it and a data: URI are present: it is the field the
// schema says holds the bytes.
HeldImage held_image(const ai::pipestream::document::v1::ImageRef& image);

// The image as one URI, for an output format that has nothing but a URI to
// carry an image in (Markdown, HTML, the Docling JSON dialect): `uri` as it
// is when set, else `data` as a base64 data: URI under the declared media
// type ("application/octet-stream" when the ref states none). Empty when
// the ref holds nothing.
std::string image_uri(const ai::pipestream::document::v1::ImageRef& image);

// A declared media type in the form HeldImage.mimetype uses.
std::string media_type_name(std::string_view declared);

// Moves the bytes of a base64 data: URI out of `uri` and into `data`, where
// the schema says held bytes go: `data` takes the decoded payload,
// `mimetype` takes the media type the URI stated (when it stated one), and
// `uri` is cleared. A ref that already holds bytes, a reference, and a
// data: URI that does not decode are left exactly as they are. True when
// the ref changed.
bool hold_bytes(ai::pipestream::document::v1::ImageRef* image);

// hold_bytes over every image a Document carries: pictures, tables,
// key-value and form items, code items and pages. Returns how many moved.
// Run where a Document from another producer enters gRParse, so that
// nothing downstream meets a data: URI gRParse could have read.
int hold_image_bytes(ai::pipestream::document::v1::Document* document);

}  // namespace grparse

#endif
