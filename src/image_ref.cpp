#include "grparse/image_ref.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <string_view>

#include "grparse/base64.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

constexpr std::string_view kDataUriPrefix = "data:";
constexpr std::string_view kBase64Suffix = ";base64";

}  // namespace

std::string media_type_name(std::string_view declared) {
  declared = declared.substr(0, declared.find(';'));
  const auto blank = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!declared.empty() && blank(declared.front())) declared.remove_prefix(1);
  while (!declared.empty() && blank(declared.back())) declared.remove_suffix(1);
  std::string name(declared);
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

HeldImage held_image(const docv1::ImageRef& image) {
  HeldImage held;
  held.mimetype = media_type_name(image.mimetype());
  if (image.has_data() && !image.data().empty()) {
    held.hold = ImageHold::kBytes;
    held.bytes = image.data();
    return held;
  }
  const std::string& uri = image.uri();
  if (uri.empty()) return held;
  if (!uri.starts_with(kDataUriPrefix)) {
    held.hold = ImageHold::kReference;
    return held;
  }
  // data:[<media type>][;base64],<payload>: the header ends at the first
  // comma, so a payload that happens to hold ";base64," is never the marker.
  const size_t comma = uri.find(',', kDataUriPrefix.size());
  std::string_view header = std::string_view(uri).substr(
      kDataUriPrefix.size(),
      comma == std::string::npos ? std::string::npos : comma - kDataUriPrefix.size());
  const bool base64 = comma != std::string::npos && header.ends_with(kBase64Suffix);
  if (base64) header.remove_suffix(kBase64Suffix.size());
  if (std::string embedded = media_type_name(header); !embedded.empty()) {
    held.mimetype = std::move(embedded);
  }
  // A payload that is not base64 (the percent-encoded form, which nothing
  // in the fleet writes), malformed base64 and an empty payload all leave
  // no bytes.
  if (base64) {
    try {
      held.bytes = decode_base64(uri.substr(comma + 1));
    } catch (const std::exception&) {
      held.bytes.clear();
    }
  }
  held.hold = held.bytes.empty() ? ImageHold::kUndecodable : ImageHold::kBytes;
  return held;
}

std::string image_uri(const docv1::ImageRef& image) {
  if (!image.uri().empty()) return image.uri();
  if (!image.has_data() || image.data().empty()) return {};
  std::string mimetype = media_type_name(image.mimetype());
  if (mimetype.empty()) mimetype = "application/octet-stream";
  return "data:" + mimetype + ";base64," +
         encode_base64(image.data().data(), image.data().size());
}

}  // namespace grparse
