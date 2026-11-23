#include "shape_profiles.h"

#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace grparse {

ShapeProfile static_profile(std::vector<std::vector<int64_t>> shapes) {
  ShapeProfile profile;
  profile.inputs.reserve(shapes.size());
  for (std::vector<int64_t>& shape : shapes) {
    profile.inputs.push_back(ShapeRange{shape, shape, std::move(shape)});
  }
  return profile;
}

namespace trt {
namespace {

bool range_is_ordered(const ShapeRange& range) {
  if (range.min.size() != range.opt.size() || range.opt.size() != range.max.size()) return false;
  for (size_t dim = 0; dim < range.min.size(); ++dim) {
    if (range.min[dim] < 0 || range.min[dim] > range.opt[dim] || range.opt[dim] > range.max[dim]) {
      return false;
    }
  }
  return true;
}

// Whether the range holds the shape in every dimension past the first, and
// the shape has at least the range's smallest row count.
bool covers(const ShapeRange& range, const std::vector<int64_t>& shape) {
  if (range.max.size() != shape.size()) return false;
  for (size_t dim = 0; dim < shape.size(); ++dim) {
    if (shape[dim] < range.min[dim]) return false;
    if (dim != 0 && shape[dim] > range.max[dim]) return false;
  }
  return true;
}

// Elements one row of the profile's largest shapes holds, over all inputs.
double row_volume(const ShapeProfile& profile) {
  double total = 0.0;
  for (const ShapeRange& range : profile.inputs) {
    double volume = 1.0;
    for (size_t dim = 1; dim < range.max.size(); ++dim) volume *= static_cast<double>(range.max[dim]);
    total += volume;
  }
  return total;
}

}  // namespace

std::string shape_text(std::span<const int64_t> shape) {
  std::string text;
  for (size_t dim = 0; dim < shape.size(); ++dim) {
    if (dim != 0) text += 'x';
    text += std::to_string(shape[dim]);
  }
  return text;
}

void validate_profiles(std::span<const ShapeProfile> profiles, size_t input_count,
                       std::string_view what) {
  for (size_t index = 0; index < profiles.size(); ++index) {
    const ShapeProfile& profile = profiles[index];
    if (profile.inputs.size() != input_count) {
      throw std::invalid_argument(std::format(
          "gRParse {}: optimization profile {} lists {} input{}, the model has {}", what, index,
          profile.inputs.size(), profile.inputs.size() == 1 ? "" : "s", input_count));
    }
    for (const ShapeRange& range : profile.inputs) {
      if (!range_is_ordered(range)) {
        throw std::invalid_argument(std::format(
            "gRParse {}: optimization profile {} needs min <= opt <= max of one rank (got {} {} {})",
            what, index, shape_text(range.min), shape_text(range.opt), shape_text(range.max)));
      }
    }
  }
}

ProfileChoice choose_profile(std::span<const ShapeProfile> profiles,
                             std::span<const std::vector<int64_t>> shapes, std::string_view what) {
  double best_volume = std::numeric_limits<double>::infinity();
  ProfileChoice best;
  bool found = false;
  for (size_t index = 0; index < profiles.size(); ++index) {
    const ShapeProfile& profile = profiles[index];
    if (profile.inputs.size() != shapes.size()) continue;
    bool fits = true;
    for (size_t input = 0; input < shapes.size() && fits; ++input) {
      fits = covers(profile.inputs[input], shapes[input]);
    }
    if (!fits) continue;
    const double volume = row_volume(profile);
    if (volume >= best_volume) continue;
    best_volume = volume;
    const std::vector<int64_t>& largest = profile.inputs.front().max;
    best = ProfileChoice{index, largest.empty() ? 1 : largest.front()};
    found = true;
  }
  if (found) return best;
  std::string listed;
  for (const std::vector<int64_t>& shape : shapes) {
    if (!listed.empty()) listed += ", ";
    listed += shape_text(shape);
  }
  throw std::runtime_error(std::format(
      "gRParse {}: no TensorRT optimization profile covers input shape{} {}", what,
      shapes.size() == 1 ? "" : "s", listed));
}

}  // namespace trt
}  // namespace grparse
