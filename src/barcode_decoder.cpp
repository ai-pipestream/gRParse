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
    results.push_back({ZXing::ToString(barcode.format()), barcode.text()});
  }
  return results;
}

}  // namespace grparse
