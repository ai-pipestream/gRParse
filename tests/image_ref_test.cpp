#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/base64.h"
#include "grparse/image_ref.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse::ImageHold;
using grparse_test::require;

const std::string kBytes("\x89PNG\r\n\x1a\n-not-a-real-image-", 26);

std::string data_uri(const std::string& header, const std::string& bytes) {
  return "data:" + header + "," + grparse::encode_base64(bytes.data(), bytes.size());
}

void verify_data_field_is_the_bytes() {
  docv1::ImageRef image;
  image.set_mimetype(" Image/PNG; charset=binary ");
  image.set_data(kBytes);
  const grparse::HeldImage held = grparse::held_image(image);
  require(held.hold == ImageHold::kBytes && held.bytes == kBytes,
          "the data field is the image, byte for byte");
  require(held.mimetype == "image/png", "the declared type is normalized: " + held.mimetype);
}

void verify_data_uri_is_still_read() {
  docv1::ImageRef image;
  image.set_mimetype("image/jpeg");
  image.set_uri(data_uri("image/png;base64", kBytes));
  const grparse::HeldImage held = grparse::held_image(image);
  require(held.hold == ImageHold::kBytes && held.bytes == kBytes,
          "an older producer's data: URI still yields the bytes");
  require(held.mimetype == "image/png",
          "the URI's own media type is the nearer statement: " + held.mimetype);

  docv1::ImageRef untyped;
  untyped.set_mimetype("image/jpeg");
  untyped.set_uri(data_uri(";base64", kBytes));
  require(grparse::held_image(untyped).mimetype == "image/jpeg",
          "a data: URI with no type falls back to the ref's own");
}

void verify_data_wins_over_a_data_uri() {
  docv1::ImageRef image;
  image.set_data(kBytes);
  image.set_uri(data_uri("image/png;base64", "other bytes"));
  require(grparse::held_image(image).bytes == kBytes,
          "the field the schema names for the bytes is the one read");
}

void verify_references_and_nothing() {
  docv1::ImageRef nothing;
  require(grparse::held_image(nothing).hold == ImageHold::kNothing, "an empty ref holds nothing");

  docv1::ImageRef empty_data;
  empty_data.set_data("");
  require(grparse::held_image(empty_data).hold == ImageHold::kNothing,
          "an empty data field is not an image");

  for (const char* uri : {"https://example.test/a.png", "images/a.png", "epub:OEBPS/a.png"}) {
    docv1::ImageRef reference;
    reference.set_uri(uri);
    const grparse::HeldImage held = grparse::held_image(reference);
    require(held.hold == ImageHold::kReference && held.bytes.empty(),
            std::string(uri) + " is a reference, not bytes");
  }
}

void verify_undecodable_data_uris() {
  for (const char* uri : {"data:image/png;base64,@@@", "data:image/png;base64,",
                          "data:image/png,percent%20encoded", "data:image/png;base64"}) {
    docv1::ImageRef image;
    image.set_uri(uri);
    const grparse::HeldImage held = grparse::held_image(image);
    require(held.hold == ImageHold::kUndecodable && held.bytes.empty(),
            std::string(uri) + " holds no readable bytes and says so");
  }
  // The header ends at the first comma: a payload that spells ";base64,"
  // further in is not the marker.
  docv1::ImageRef tricky;
  tricky.set_uri("data:text/plain,a;base64,QUJD");
  require(grparse::held_image(tricky).hold == ImageHold::kUndecodable,
          "a marker inside the payload is not the header's");
}

void verify_image_uri() {
  docv1::ImageRef reference;
  reference.set_uri("images/a.png");
  require(grparse::image_uri(reference) == "images/a.png", "a uri is given as it is");

  docv1::ImageRef held;
  held.set_mimetype("image/png");
  held.set_data(kBytes);
  require(grparse::image_uri(held) == data_uri("image/png;base64", kBytes),
          "bytes become a data: URI under their declared type");

  docv1::ImageRef untyped;
  untyped.set_data(kBytes);
  require(grparse::image_uri(untyped).starts_with("data:application/octet-stream;base64,"),
          "bytes with no declared type are not given a guessed one");

  docv1::ImageRef both;
  both.set_uri("images/a.png");
  both.set_data(kBytes);
  require(grparse::image_uri(both) == "images/a.png",
          "a ref that names where the image lives keeps that name");

  require(grparse::image_uri(docv1::ImageRef()).empty(), "nothing stays nothing");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("image-ref-test", "all checks passed", {
      verify_data_field_is_the_bytes,
      verify_data_uri_is_still_read,
      verify_data_wins_over_a_data_uri,
      verify_references_and_nothing,
      verify_undecodable_data_uris,
      verify_image_uri,
  });
}
