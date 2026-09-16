#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace grparse::embedding::detail {

// All backends consume the same padded tokens and produce unpooled FP32
// hidden states. Keeping preprocessing outside the vendor runtimes gives
// CPU, Intel and NVIDIA the same attention mask and pooling semantics.
struct TokenBatch {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::vector<int64_t> ids;
  std::vector<int64_t> mask;
  std::vector<int64_t> types;
};

class TensorBackend {
 public:
  virtual ~TensorBackend() = default;
  virtual std::vector<float> infer(const TokenBatch& tokens) = 0;
};

// Self-contained, verified ONNX bytes, borrowed only during the synchronous
// factory call. Backends finish parsing/compilation before returning and own
// their resulting native model state; none may reopen the source artifact.
std::unique_ptr<TensorBackend> make_cpu_backend(std::string_view model);
#ifdef GRPARSE_EMBED_OPENVINO
std::unique_ptr<TensorBackend> make_intel_backend(std::string_view model, const std::string& device);
#endif
#ifdef GRPARSE_EMBED_TENSORRT
std::unique_ptr<TensorBackend> make_nvidia_backend(std::string_view model, int gpu_index);
#endif

}  // namespace grparse::embedding::detail
