#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

// Decodes every barcode and QR code visible in the image with ZXing (pure
// CPU, no model).  Accepts the pipeline's BGR rasters and grayscale images;
// an empty image or an image with nothing decodable returns an empty vector.
// Results keep ZXing's detection order.
std::vector<BarcodeResult> decode_barcodes(const cv::Mat& image);

}  // namespace grparse
