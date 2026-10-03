// Picture image bytes for the image-carrying exports; semantics documented
// in picture_image.h.
#include "picture_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/base64.h"
#include "grparse/document_geometry.h"
#include "grparse/page_previews.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {
namespace {

constexpr std::string_view kDataUriPrefix = "data:";
constexpr std::string_view kBase64Marker = ";base64,";

std::string extension_for(std::string_view mimetype) {
  if (mimetype == "image/png") return "png";
  if (mimetype == "image/jpeg" || mimetype == "image/jpg") return "jpg";
  if (mimetype == "image/webp") return "webp";
  if (mimetype == "image/tiff") return "tif";
  if (mimetype == "image/gif") return "gif";
  if (mimetype == "image/bmp") return "bmp";
  // Unknown by name: archive_image decodes and re-encodes it as PNG.
  return "bin";
}

cv::Mat decode(const std::string& bytes, int flags) {
  if (bytes.empty()) return {};
  const cv::Mat buffer(1, static_cast<int>(bytes.size()), CV_8UC1,
                       const_cast<char*>(bytes.data()));
  try {
    return cv::imdecode(buffer, flags);
  } catch (const cv::Exception&) {
    return {};
  }
}

std::optional<std::string> encode_png(const cv::Mat& image) {
  std::vector<unsigned char> png;
  try {
    // The same fixed compression level as the page previews, so a crop of
    // the same pixels always encodes to the same bytes.
    if (!cv::imencode(".png", image, png, kPngEncodeParams)) return std::nullopt;
  } catch (const cv::Exception&) {
    return std::nullopt;
  }
  return std::string(png.begin(), png.end());
}

}  // namespace

std::optional<EncodedImage> decode_image_data_uri(const std::string& uri) {
  if (!uri.starts_with(kDataUriPrefix)) return std::nullopt;
  const size_t marker = uri.find(kBase64Marker);
  if (marker == std::string::npos) return std::nullopt;
  EncodedImage image;
  try {
    image.bytes = decode_base64(uri.substr(marker + kBase64Marker.size()));
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  if (image.bytes.empty()) return std::nullopt;
  std::string mimetype = uri.substr(kDataUriPrefix.size(), marker - kDataUriPrefix.size());
  std::ranges::transform(mimetype, mimetype.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  image.extension = extension_for(mimetype);
  return image;
}

std::optional<EncodedImage> archive_image(EncodedImage image) {
  if (image.extension == "png" || image.extension == "jpg" || image.extension == "webp") {
    return image;
  }
  const cv::Mat pixels = decode(image.bytes, cv::IMREAD_UNCHANGED);
  if (pixels.empty()) return std::nullopt;
  auto png = encode_png(pixels);
  if (!png.has_value()) return std::nullopt;
  return EncodedImage{std::move(*png), "png"};
}

std::optional<std::string> crop_picture_png(const docv1::Document& document,
                                            const docv1::PictureItem& picture) {
  if (picture.prov_size() == 0) return std::nullopt;
  const auto& prov = picture.prov(0);
  if (!prov.has_bbox()) return std::nullopt;
  const auto page = document.pages().find(prov.page_no());
  if (page == document.pages().end()) return std::nullopt;
  const docv1::PageItem& item = page->second;
  if (!item.has_size() || !(item.size().width() > 0) || !(item.size().height() > 0) ||
      !item.has_image()) {
    return std::nullopt;
  }
  const auto encoded = decode_image_data_uri(item.image().uri());
  if (!encoded.has_value()) return std::nullopt;
  const cv::Mat raster = decode(encoded->bytes, cv::IMREAD_UNCHANGED);
  if (raster.empty()) return std::nullopt;

  const TopDownBox box = top_down_box(prov.bbox(), item.size().height());
  const double scale_x = raster.cols / item.size().width();
  const double scale_y = raster.rows / item.size().height();
  const auto clamp_to = [](double value, int limit) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, limit);
  };
  const int left = clamp_to(box.left * scale_x, raster.cols);
  const int right = clamp_to(box.right * scale_x, raster.cols);
  const int top = clamp_to(box.top * scale_y, raster.rows);
  const int bottom = clamp_to(box.bottom * scale_y, raster.rows);
  if (right <= left || bottom <= top) return std::nullopt;
  // clone(): encode the crop's own pixels, not a view whose stride is the
  // page's.
  return encode_png(raster(cv::Rect(left, top, right - left, bottom - top)).clone());
}

std::string png_data_uri(const std::string& png) {
  return "data:image/png;base64," + encode_base64(png.data(), png.size());
}

}  // namespace grparse::render
