#include "grparse/barcode_decoder.h"

#include <stdexcept>

// ZXing is linked from its FetchContent source tree, which exports headers
// without the installed ZXing/ prefix.
#include <BarcodeFormat.h>
#include <ReadBarcode.h>

namespace grparse {

std::vector<BarcodeResult> decode_barcodes(const cv::Mat& image) {
  if (image.empty()) return {};
  ZXing::ImageFormat format = ZXing::ImageFormat::None;
  switch (image.type()) {
    case CV_8UC1:
      format = ZXing::ImageFormat::Lum;
      break;
    case CV_8UC3:
      format = ZXing::ImageFormat::BGR;
      break;
    default:
      throw std::invalid_argument("decode_barcodes needs an 8-bit gray or BGR image");
  }
  // The stride is taken from the Mat so region-of-interest views decode in
  // place, without a continuity copy.
  const ZXing::ImageView view(image.data, image.cols, image.rows, format,
                              static_cast<int>(image.step));
  const auto options = ZXing::ReaderOptions().setTryHarder(true).setTryInvert(true);
  std::vector<BarcodeResult> results;
  for (const auto& barcode : ZXing::ReadBarcodes(view, options)) {
    if (!barcode.isValid()) continue;
    // ZXing 3.x renamed its format display names with spaces ("QR Code",
    // "Data Matrix"). The format string is a consumer-visible identifier on
    // the wire, so the 2.x spellings stay the contract - and they are
    // exactly the space-free forms of the new names.
    std::string format_name = ZXing::ToString(barcode.format());
    std::erase(format_name, ' ');
    results.push_back({std::move(format_name), barcode.text()});
  }
  return results;
}

}  // namespace grparse
