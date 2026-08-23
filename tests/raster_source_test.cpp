// Exercises the raster branch of open_in_memory_document: the OpenCV decode
// of PNG/JPEG request bytes that backs image (non-PDF) ingest.  Images are
// encoded in memory, so no binary fixture is needed.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "grparse/in_memory_document.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::shared_ptr<const std::string> encode(const cv::Mat& image, const std::string& extension) {
  std::vector<unsigned char> buffer;
  require(cv::imencode(extension, image, buffer), "test image must encode");
  return std::make_shared<const std::string>(buffer.begin(), buffer.end());
}

void verify_png_round_trip() {
  cv::Mat image(30, 40, CV_8UC3, cv::Scalar(10, 20, 30));
  image.at<cv::Vec3b>(5, 7) = {200, 100, 50};

  const auto source = grparse::open_in_memory_document(encode(image, ".png"), /*pdf=*/false);
  require(source->page_count() == 1, "a raster input is exactly one page");
  require(!source->extract_digital_page(1).has_value(),
          "raster inputs have no digital text layer");

  const cv::Mat decoded = source->render_page(1);
  require(decoded.cols == 40 && decoded.rows == 30, "decoded size matches the source image");
  require(decoded.type() == CV_8UC3, "raster pages decode as 8-bit BGR");
  require(decoded.at<cv::Vec3b>(5, 7) == cv::Vec3b(200, 100, 50),
          "PNG decode must be lossless");

  // Rendering is stateless: a second render of the same page succeeds.
  require(!source->render_page(1).empty(), "render is repeatable");
}

void verify_grayscale_normalizes_to_bgr() {
  const cv::Mat gray(16, 16, CV_8UC1, cv::Scalar(128));
  const auto source = grparse::open_in_memory_document(encode(gray, ".png"), /*pdf=*/false);
  require(source->render_page(1).type() == CV_8UC3,
          "grayscale inputs normalize to BGR for the pipeline");
}

void verify_jpeg_decodes() {
  const cv::Mat image(24, 32, CV_8UC3, cv::Scalar(60, 120, 180));
  const auto source = grparse::open_in_memory_document(encode(image, ".jpg"), /*pdf=*/false);
  const cv::Mat decoded = source->render_page(1);
  require(decoded.cols == 32 && decoded.rows == 24, "JPEG decodes at its native size");
}

void verify_page_range() {
  const cv::Mat image(8, 8, CV_8UC3, cv::Scalar(0, 0, 0));
  const auto source = grparse::open_in_memory_document(encode(image, ".png"), /*pdf=*/false);
  for (const int bad_page : {0, 2, -1}) {
    try {
      source->render_page(bad_page);
      require(false, "out-of-range page must throw");
    } catch (const grparse::InvalidDocument&) {
    }
  }
}

void verify_invalid_bytes() {
  try {
    grparse::open_in_memory_document(std::make_shared<const std::string>(), /*pdf=*/false);
    require(false, "empty bytes must throw");
  } catch (const grparse::InvalidDocument&) {
  }

  // Bytes whose container magic is not one of the advertised raster formats
  // are rejected at admission, before any decoder (and its temp-file/GDAL
  // paths) can run.
  try {
    grparse::open_in_memory_document(std::make_shared<const std::string>("not an image"),
                                     /*pdf=*/false);
    require(false, "unsupported-format bytes must throw at open");
  } catch (const grparse::InvalidDocument&) {
  }
}

// The raster door admits only PNG, JPEG, and TIFF; every other container that
// cv::imdecode would otherwise sniff (WebP, BMP, GIF, and GDAL-backed formats)
// is rejected before a decoder runs.
void verify_only_advertised_formats_are_admitted() {
  const cv::Mat image(8, 8, CV_8UC3, cv::Scalar(0, 0, 0));
  for (const char* extension : {".png", ".jpg", ".tif"}) {
    const auto source = grparse::open_in_memory_document(encode(image, extension), /*pdf=*/false);
    require(!source->render_page(1).empty(), std::string("advertised format decodes: ") + extension);
  }

  for (const char* extension : {".webp", ".bmp"}) {
    std::vector<unsigned char> buffer;
    if (!cv::imencode(extension, image, buffer)) continue;  // codec not built here
    try {
      grparse::open_in_memory_document(
          std::make_shared<const std::string>(buffer.begin(), buffer.end()), /*pdf=*/false);
      require(false, std::string("unadvertised format must be rejected: ") + extension);
    } catch (const grparse::InvalidDocument&) {
    }
  }
}

}  // namespace

int main() {
  try {
    verify_png_round_trip();
    verify_grayscale_normalizes_to_bgr();
    verify_jpeg_decodes();
    verify_page_range();
    verify_invalid_bytes();
    verify_only_advertised_formats_are_admitted();
  } catch (const std::exception& error) {
    std::println(stderr, "raster_source_test failed: {}", error.what());
    return EXIT_FAILURE;
  }
  std::println("raster_source_test passed");
  return EXIT_SUCCESS;
}
