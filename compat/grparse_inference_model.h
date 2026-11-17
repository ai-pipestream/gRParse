#pragma once

// One model, one typed run call, whatever runs it.  This file is the part of
// the interface the native backends share: the tensor types, the device
// error and InferenceModel itself.  The native OpenVINO and TensorRT branches
// carry the same declarations plus their own factories; when they land, their
// header replaces this one and src/inference_tensor.cpp goes away (its four
// definitions live in their src/inference_model.cpp).  The Hailo backend
// (src/hailo/) only depends on what is declared here.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace grparse {

enum class TensorElement { kFloat32, kInt64, kUInt8 };

size_t tensor_element_size(TensorElement element);

// An input's element type and static shape, without data (prewarm sets).
struct TensorSpec {
  TensorElement element = TensorElement::kFloat32;
  std::vector<int64_t> shape;
};

// A borrowed input: the caller keeps `data` alive for the run call.
struct TensorView {
  TensorElement element = TensorElement::kFloat32;
  const void* data = nullptr;
  std::vector<int64_t> shape;
};

// An owned output, copied off the device.
struct Tensor {
  TensorElement element = TensorElement::kFloat32;
  std::vector<int64_t> shape;
  std::vector<std::byte> bytes;

  size_t element_count() const;
  const float* floats() const;
  const int64_t* int64s() const;
};

// Thrown when the device fails an inference: the request fails with
// UNAVAILABLE naming the device.
class InferenceDeviceError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class InferenceModel {
 public:
  virtual ~InferenceModel() = default;
  // Inputs in the model's input order; outputs in its output order.
  virtual std::vector<Tensor> run(std::span<const TensorView> inputs) = 0;
  // Runs once every listed input set, so no first-use setup happens under
  // load.
  virtual void prewarm(std::span<const std::vector<TensorSpec>> input_sets) = 0;
  virtual const std::vector<std::string>& input_names() const = 0;
  virtual const std::vector<std::string>& output_names() const = 0;
  size_t input_count() const { return input_names().size(); }
  size_t output_count() const { return output_names().size(); }
  virtual std::string_view backend_name() const = 0;
};

}  // namespace grparse
