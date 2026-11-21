#pragma once

// Which optimization profile serves a run, decided on shapes alone.  No
// TensorRT type appears here, so the rule is tested in every flavor.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "grparse_inference_model.h"

namespace grparse::trt {

// Throws std::invalid_argument naming `what` unless every profile lists
// `input_count` ranges whose min, opt and max have one rank and satisfy
// min <= opt <= max in every dimension.
void validate_profiles(std::span<const ShapeProfile> profiles, size_t input_count,
                       std::string_view what);

struct ProfileChoice {
  size_t profile = 0;
  // The most rows (dimension 0) one call may carry under that profile; a
  // run with more is split into calls of at most this many.
  int64_t max_rows = 1;
};

// The profile for inputs of these shapes (model input order): among the
// profiles whose ranges contain every dimension past the first, the one
// whose largest shapes hold the fewest elements per row, the earlier one on
// a tie.  The first dimension is rows: it must reach the profile's smallest
// batch and may exceed its largest, since rows split across calls.  Throws
// std::runtime_error naming `what` and the shapes when no profile fits: the
// profiles are the server's to get right, not the caller's.
ProfileChoice choose_profile(std::span<const ShapeProfile> profiles,
                             std::span<const std::vector<int64_t>> shapes, std::string_view what);

// "1x3x48x320" for messages and cache keys.
std::string shape_text(std::span<const int64_t> shape);

}  // namespace grparse::trt
