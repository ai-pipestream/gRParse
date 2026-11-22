#pragma once

// The TensorRT optimization profiles of the three RapidOCR nets.  Pure in
// their arguments and free of TensorRT types, so they are tested in every
// flavor.
//
// The nets take their inputs at their exact sizes, as they do on ONNX
// Runtime's CUDA provider, so each profile is a range, not one padded bucket
// per surveyed size.  Measured on an RTX 4080 with TensorRT 11.2 (#364): a
// profile tuned for one size ran the detector no faster than one range
// profile tuned for a typical page (9.7 against 10.1 ms at 1664 x 2144, 4.4
// against 4.0 ms at 1088 x 1472), and the recognizer within 4 percent,
// while every profile added about 35 s to an engine build that runs at each
// start without a cache: nine detector profiles built in 339 s, one in 38.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "grparse_inference_model.h"

namespace grparse::trt {

// The narrowest and widest line (pixels at height 48) the recognition
// profiles serve.  A narrower crop is padded with zeros, a wider one resized
// down (patches/rapidocr-rec-width-limits.patch).
inline constexpr int kRecMinWidth = 16;
inline constexpr int kRecMaxWidth = 8192;

// The detector's largest input side for a configured GRPARSE_OCR_MAX_SIDE
// and GRPARSE_OCR_PADDING: RapidOCR resizes the padded page so its longest
// side is at most their sum, rounded down to a multiple of 32.
int detector_side_limit(int max_side_len, int padding);

// One profile for every detector input, 32 x 32 up to a square of
// `side_limit`, tuned for an upright letter page at 200 DPI (1664 x 2144,
// clipped to the limit): the most common full-page read of the #358 survey.
std::vector<ShapeProfile> detector_profiles(int side_limit);

// Two profiles: batches of up to `batch` lines up to 2560 wide (the widest
// line of the #358 survey was 2524), tuned for a full batch at 576, and
// single lines from there to kRecMaxWidth.
std::vector<ShapeProfile> recognizer_profiles(size_t batch);

// The direction net's one shape, for batches of up to `batch` crops.
std::vector<ShapeProfile> direction_profiles(size_t batch);

}  // namespace grparse::trt
