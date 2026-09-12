#include "grparse/embedding.h"
#include "tensor_backend.h"
#include "../targets/sha256.h"
#include "../chunking/token_counter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tokenizers_cpp.h>
#include <simdutf.h>

namespace grparse {
namespace {
constexpr std::size_t kDimensions = 384;
constexpr std::size_t kMaxTokens = 256;
constexpr const char* kRevision = "826711e54e001c83835913827a843d8dd0a1def9";

std::string verified_file(const std::filesystem::path& file, std::size_t size,
                          std::string_view digest) {
  if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    throw std::invalid_argument("embedding model artifact size exceeds stream limit");
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::invalid_argument("cannot read embedding model artifact");
  // Bound allocation and reads even if an artifact is replaced or grows.
  // The bytes we hash are the same owned buffer passed to the parser below.
  std::string bytes(size, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(size));
  if (input.bad()) throw std::invalid_argument("cannot read embedding model artifact");
  if (input.gcount() != static_cast<std::streamsize>(size) ||
      input.peek() != std::char_traits<char>::eof())
    throw std::invalid_argument("embedding model artifact has an unexpected size");
  if (input.bad()) throw std::invalid_argument("cannot read embedding model artifact");
  if (targets::sha256_hex(bytes) != digest)
    throw std::invalid_argument("embedding model artifact failed SHA256 verification");
  return bytes;
}

class LocalEmbeddingEngine final : public EmbeddingEngine {
 public:
  explicit LocalEmbeddingEngine(const EmbeddingConfig& config)
      : batch_size_(std::min<std::size_t>(config.max_batch_size, 16)),
        batch_bytes_(config.max_batch_bytes) {
    if (batch_size_ == 0 || batch_bytes_ == 0 || config.model_dir.empty())
      throw std::invalid_argument("embedding model directory and positive batch limits are required");
    const auto tokenizer_bytes = verified_file(config.model_dir / "tokenizer.json", 466247,
        "be50c3628f2bf5bb5e3a7f17b1f74611b2561a3a27eeab05e5aa30f411572037");
    std::string tokenizer_json;
    // Published tokenizer.json carries a 128-token truncation/padding
    // policy. Disable both before counting so oversized text is rejected,
    // never silently embedded only in part.
    if (!chunking::strip_hf_tokenizer_json(tokenizer_bytes, "verified embedding tokenizer", &tokenizer_json).ok())
      throw std::invalid_argument("cannot configure embedding tokenizer");
    tokenizer_ = tokenizers::Tokenizer::FromBlobJSON(tokenizer_json);
    if (!tokenizer_ || tokenizer_->TokenToId("[CLS]") != 101 ||
        tokenizer_->TokenToId("[SEP]") != 102 || tokenizer_->TokenToId("[PAD]") != 0)
      throw std::invalid_argument("embedding tokenizer does not match MiniLM");
    const auto model = verified_file(config.model_dir / "model.onnx", 90405214,
        "6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452");
    identity_ = {"sentence-transformers/all-MiniLM-L6-v2", kRevision, "",
                 "attention-mask-mean", kDimensions, kMaxTokens, true};
    switch (config.backend) {
      case EmbeddingBackend::cpu:
        identity_.backend = "cpu";
        backend_ = embedding::detail::make_cpu_backend(model);
        break;
      case EmbeddingBackend::openvino:
#ifdef GRPARSE_EMBED_OPENVINO
        identity_.backend = "openvino";
        backend_ = embedding::detail::make_intel_backend(model, config.device);
        break;
#else
        throw std::invalid_argument("this image was built without native OpenVINO embeddings");
#endif
      case EmbeddingBackend::tensorrt:
#ifdef GRPARSE_EMBED_TENSORRT
        identity_.backend = "tensorrt";
        backend_ = embedding::detail::make_nvidia_backend(model, config.gpu_index);
        break;
#else
        throw std::invalid_argument("this image was built without native TensorRT embeddings");
#endif
      default:
        throw std::invalid_argument("invalid local embedding backend");
    }
  }

  EmbeddingModelIdentity identity() const override { return identity_; }
  std::size_t max_batch_size() const override { return batch_size_; }

  grpc::Status embed(const std::vector<std::string>& texts,
                     const EmbeddingCancelled& cancelled,
                     EmbeddingBatchResult* result) override {
    if (!result) return {grpc::StatusCode::INVALID_ARGUMENT, "embedding result is required"};
    const auto is_cancelled = [&] { return cancelled && cancelled(); };
    const auto cancellation = [] { return grpc::Status(grpc::StatusCode::CANCELLED, "embedding cancelled"); };
    if (is_cancelled()) return cancellation();
    if (texts.size() > batch_size_)
      return {grpc::StatusCode::INVALID_ARGUMENT, "embedding batch exceeds configured size"};
    std::size_t bytes = 0;
    for (const auto& text : texts) {
      if (!simdutf::validate_utf8(text.data(), text.size()))
        return {grpc::StatusCode::INVALID_ARGUMENT, "embedding text must be valid UTF-8"};
      if (text.size() > batch_bytes_ - bytes)
        return {grpc::StatusCode::RESOURCE_EXHAUSTED, "embedding batch exceeds byte limit"};
      bytes += text.size();
    }
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(10))) {
      if (is_cancelled()) return cancellation();
    }
    if (is_cancelled()) return cancellation();
    try {
      EmbeddingBatchResult pending{identity_, {}};
      if (texts.empty()) { *result = std::move(pending); return grpc::Status::OK; }
      std::vector<std::vector<int32_t>> encoded;
      std::size_t width = 2;
      for (const auto& text : texts) {
        if (is_cancelled()) return cancellation();
        // tokenizers-cpp Encode omits special tokens. Add exactly the
        // MiniLM single-sequence CLS/SEP pair, preserving its 256-token cap.
        auto ids = tokenizer_->Encode(text);
        if (ids.size() > kMaxTokens - 2)
          return {grpc::StatusCode::INVALID_ARGUMENT, "embedding text exceeds 256 tokens; use smaller chunks"};
        ids.insert(ids.begin(), 101);
        ids.push_back(102);
        width = std::max(width, ids.size());
        encoded.push_back(std::move(ids));
      }
      embedding::detail::TokenBatch tokens;
      tokens.rows = texts.size();
      tokens.columns = width;
      tokens.ids.assign(tokens.rows * width, 0);
      tokens.mask.assign(tokens.ids.size(), 0);
      tokens.types.assign(tokens.ids.size(), 0);
      for (std::size_t row = 0; row < encoded.size(); ++row) {
        std::copy(encoded[row].begin(), encoded[row].end(), tokens.ids.begin() + row * width);
        std::fill_n(tokens.mask.begin() + row * width, encoded[row].size(), 1);
      }
      if (is_cancelled()) return cancellation();
      auto hidden = backend_->infer(tokens);
      if (is_cancelled()) return cancellation();
      if (hidden.size() != tokens.rows * width * kDimensions)
        return {grpc::StatusCode::INTERNAL, "embedding hidden-state size mismatch"};
      for (std::size_t row = 0; row < tokens.rows; ++row) {
        std::vector<float> vector(kDimensions);
        double norm = 0;
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
          double sum = 0;
          for (std::size_t token = 0; token < encoded[row].size(); ++token)
            sum += hidden[(row * width + token) * kDimensions + dim];
          vector[dim] = static_cast<float>(sum / encoded[row].size());
          norm += static_cast<double>(vector[dim]) * vector[dim];
        }
        if (!std::isfinite(norm) || norm <= 0)
          return {grpc::StatusCode::INTERNAL, "embedding model returned invalid vector"};
        const double scale = std::sqrt(norm);
        for (float& value : vector) value = static_cast<float>(value / scale);
        pending.vectors.push_back(std::move(vector));
      }
      if (is_cancelled()) return cancellation();
      *result = std::move(pending);
      return grpc::Status::OK;
    } catch (const std::exception&) {
      // Vendor errors can include caller text or filesystem details.
      return {grpc::StatusCode::INTERNAL, "local embedding inference failed"};
    }
  }

 private:
  std::size_t batch_size_;
  std::size_t batch_bytes_;
  EmbeddingModelIdentity identity_;
  std::unique_ptr<tokenizers::Tokenizer> tokenizer_;
  std::unique_ptr<embedding::detail::TensorBackend> backend_;
  std::timed_mutex mutex_;
};
}  // namespace

std::shared_ptr<EmbeddingEngine> make_embedding_engine(const EmbeddingConfig& config) {
  if (config.backend == EmbeddingBackend::disabled) return {};
  return std::make_shared<LocalEmbeddingEngine>(config);
}
}  // namespace grparse
