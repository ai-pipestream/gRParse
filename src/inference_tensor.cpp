#include "grparse_inference_model.h"

// The tensor helpers of compat/grparse_inference_model.h.  The native
// OpenVINO and TensorRT branches define the same four functions in
// src/inference_model.cpp; whichever lands second drops this file.

namespace grparse {

size_t tensor_element_size(TensorElement element) {
  switch (element) {
    case TensorElement::kFloat32: return sizeof(float);
    case TensorElement::kInt64: return sizeof(int64_t);
    case TensorElement::kUInt8: return sizeof(uint8_t);
  }
  return 0;
}

size_t Tensor::element_count() const {
  size_t count = 1;
  for (const int64_t dim : shape) count *= static_cast<size_t>(dim);
  return count;
}

const float* Tensor::floats() const { return reinterpret_cast<const float*>(bytes.data()); }

const int64_t* Tensor::int64s() const { return reinterpret_cast<const int64_t*>(bytes.data()); }

}  // namespace grparse
