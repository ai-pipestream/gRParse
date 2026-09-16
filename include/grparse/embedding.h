#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

namespace grparse {

using EmbeddingCancelled = std::function<bool()>;

// Immutable identity of the loaded artifacts and preprocessing, not an alias
// supplied by the request. All backends serving the same model must agree on
// every field except backend. revision identifies weights AND tokenizer.
struct EmbeddingModelIdentity {
  std::string model_id;
  std::string revision;
  std::string backend;  // "cpu", "openvino", or "tensorrt"; no silent fallback
  std::string pooling;  // e.g. "attention-mask-mean"
  std::size_t dimensions = 0;
  std::size_t max_input_tokens = 0;  // includes special tokens
  bool normalized = true;

  bool operator==(const EmbeddingModelIdentity&) const = default;
};

struct EmbeddingBatchResult {
  EmbeddingModelIdentity model;
  // Exactly one finite vector per input, in input order. No partial success.
  std::vector<std::vector<float>> vectors;
};

class EmbeddingEngine {
 public:
  virtual ~EmbeddingEngine() = default;
  virtual EmbeddingModelIdentity identity() const = 0;
  virtual std::size_t max_batch_size() const = 0;

  // Thread-safe: implementations must pool or serialize native contexts with
  // cancellation-aware waits. Do not retain inputs or the callback. Return
  // CANCELLED when cancelled, INVALID_ARGUMENT for text exceeding the model's
  // token limit (never silently truncate), INTERNAL for inference failures.
  // Tokenization, pooling and normalization must match across native backends.
  // Check cancellation before/after inference even if a device call cannot be
  // interrupted. Populate result only on success; do not expose text in errors.
  virtual grpc::Status embed(const std::vector<std::string>& texts,
                             const EmbeddingCancelled& cancelled,
                             EmbeddingBatchResult* result) = 0;
};

enum class EmbeddingBackend { disabled, cpu, openvino, tensorrt };

struct EmbeddingConfig {
  EmbeddingBackend backend = EmbeddingBackend::disabled;
  std::filesystem::path model_dir;
  std::string device = "GPU";  // native OpenVINO device
  int gpu_index = 0;           // native TensorRT device
  std::size_t max_batch_size = 32;
  std::size_t max_batch_bytes = 1024 * 1024;
  std::size_t max_response_bytes = 64 * 1024 * 1024;
};

// Backend owner implements this factory. Disabled returns null without loading
// artifacts. An unavailable explicitly selected backend or bad artifact fails
// startup; it must never fall back to another backend or a synthetic vector.
std::shared_ptr<EmbeddingEngine> make_embedding_engine(const EmbeddingConfig& config);

}  // namespace grparse
