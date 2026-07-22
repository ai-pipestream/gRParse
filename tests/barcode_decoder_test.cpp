#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "grparse/barcode_decoder.h"

namespace {

namespace fs = std::filesystem;

// The committed fixture encodes this payload (see tests/data/qr_code.png).
constexpr const char* kQrPayload = "https://github.com/krickert/gRParse/e3";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

cv::Mat load_fixture() {
  const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
  const fs::path image_path =
      fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "qr_code.png";
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "QR fixture must load: " + image_path.string());
  return image;
}

void verify_qr_fixture_decodes(const cv::Mat& fixture) {
  const auto results = grparse::decode_barcodes(fixture);
  require(results.size() == 1, "the fixture holds exactly one code");
  require(results.front().format == "QRCode",
          "format must be QRCode (got " + results.front().format + ")");
  require(results.front().text == kQrPayload,
          "payload mismatch (got " + results.front().text + ")");
}

void verify_grayscale_decodes(const cv::Mat& fixture) {
  cv::Mat gray;
  cv::cvtColor(fixture, gray, cv::COLOR_BGR2GRAY);
  const auto results = grparse::decode_barcodes(gray);
  require(results.size() == 1 && results.front().text == kQrPayload,
          "grayscale input must decode the same payload");
}

// Region crops are Mat views with the parent's row stride; decoding must
// honor the stride instead of assuming continuity.
void verify_roi_view_decodes(const cv::Mat& fixture) {
  cv::Mat page(fixture.rows + 120, fixture.cols + 160, CV_8UC3, cv::Scalar(255, 255, 255));
  fixture.copyTo(page(cv::Rect(80, 60, fixture.cols, fixture.rows)));
  const cv::Mat view = page(cv::Rect(80, 60, fixture.cols, fixture.rows));
  require(!view.isContinuous(), "the test view must be non-continuous to prove stride handling");
  const auto results = grparse::decode_barcodes(view);
  require(results.size() == 1 && results.front().text == kQrPayload,
          "a non-continuous ROI view must decode in place");
}

void verify_blank_and_empty_images() {
  const cv::Mat blank(200, 200, CV_8UC3, cv::Scalar(255, 255, 255));
  require(grparse::decode_barcodes(blank).empty(), "a blank image has no codes");
  require(grparse::decode_barcodes(cv::Mat()).empty(), "an empty image has no codes");
  bool threw = false;
  try {
    grparse::decode_barcodes(cv::Mat(10, 10, CV_32FC1));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "unsupported pixel types must be rejected");
}

void verify_determinism(const cv::Mat& fixture) {
  const auto first = grparse::decode_barcodes(fixture);
  for (int run = 0; run < 5; ++run) {
    const auto again = grparse::decode_barcodes(fixture);
    require(again.size() == first.size() && again.front().text == first.front().text,
            "decoding is deterministic");
  }
}

}  // namespace

int main() {
  try {
    const cv::Mat fixture = load_fixture();
    verify_qr_fixture_decodes(fixture);
    verify_grayscale_decodes(fixture);
    verify_roi_view_decodes(fixture);
    verify_blank_and_empty_images();
    verify_determinism(fixture);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "barcode-decoder-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
