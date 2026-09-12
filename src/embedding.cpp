#include "grparse/chunk_embeddings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace grparse {
namespace {
namespace parsev1 = ai::pipestream::parse::v1;

grpc::Status cancelled_status() {
  return {grpc::StatusCode::CANCELLED, "chunk embedding cancelled"};
}

parsev1::EmbeddingModel model_proto(const EmbeddingModelIdentity& identity) {
  parsev1::EmbeddingModel model;
  model.set_model_id(identity.model_id);
  model.set_revision(identity.revision);
  model.set_backend(identity.backend);
  model.set_dimensions(static_cast<std::uint32_t>(identity.dimensions));
  model.set_pooling(identity.pooling);
  model.set_normalized(identity.normalized);
  model.set_max_input_tokens(static_cast<std::uint32_t>(identity.max_input_tokens));
  return model;
}

// Compute the length before allocating contextual text. Subtraction keeps the
// byte-bound check safe even when the headings alone exceed the batch budget.
bool text_fits(const parsev1::Chunk& chunk, bool contextualized, std::size_t limit,
               std::size_t* bytes) {
  if (chunk.text().size() > limit) return false;
  *bytes = chunk.text().size();
  if (contextualized) {
    for (const auto& heading : chunk.headings()) {
      if (heading.size() >= limit - *bytes) return false;
      *bytes += heading.size() + 1;
    }
  }
  return true;
}

std::string embedding_text(const parsev1::Chunk& chunk, bool contextualized,
                           std::size_t bytes) {
  std::string text;
  text.reserve(bytes);
  if (contextualized) {
    for (const auto& heading : chunk.headings()) {
      text.append(heading);
      text.push_back('\n');
    }
  }
  text.append(chunk.text());
  return text;
}
}  // namespace

ChunkEmbedder::ChunkEmbedder(std::shared_ptr<EmbeddingEngine> engine, EmbeddingConfig config)
    : engine_(std::move(engine)), config_(std::move(config)) {
  if (config_.max_batch_size == 0 ||
      config_.max_batch_size > std::numeric_limits<std::uint32_t>::max() ||
      config_.max_batch_bytes == 0 || config_.max_response_bytes == 0) {
    throw std::invalid_argument("embedding resource limits must be positive and batch size fit uint32");
  }
  if (!engine_) {
    if (config_.backend != EmbeddingBackend::disabled) {
      throw std::invalid_argument("configured embedding backend did not load an engine");
    }
    return;
  }
  identity_ = engine_->identity();
  if (identity_.model_id != "sentence-transformers/all-MiniLM-L6-v2" ||
      identity_.revision.empty() || identity_.dimensions != 384 ||
      identity_.max_input_tokens != 256 || identity_.pooling != "attention-mask-mean" ||
      !identity_.normalized ||
      (identity_.backend != "cpu" && identity_.backend != "openvino" &&
       identity_.backend != "tensorrt")) {
    throw std::invalid_argument("embedding engine does not implement the supported MiniLM contract");
  }
  if ((config_.backend == EmbeddingBackend::cpu && identity_.backend != "cpu") ||
      (config_.backend == EmbeddingBackend::openvino && identity_.backend != "openvino") ||
      (config_.backend == EmbeddingBackend::tensorrt && identity_.backend != "tensorrt")) {
    throw std::invalid_argument("embedding engine differs from the explicitly selected backend");
  }
  batch_size_ = std::min(config_.max_batch_size, engine_->max_batch_size());
  if (batch_size_ == 0) throw std::invalid_argument("embedding engine batch size must be positive");
}

grpc::Status ChunkEmbedder::validate(const parsev1::EmbeddingOptions& options) const {
  if (!options.enabled()) {
    if (options.has_model_id() || options.has_batch_size() ||
        options.text_mode() != parsev1::EMBEDDING_TEXT_MODE_UNSPECIFIED) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "embedding options require enabled=true"};
    }
    return grpc::Status::OK;
  }
  if (options.text_mode() != parsev1::EMBEDDING_TEXT_MODE_UNSPECIFIED &&
      options.text_mode() != parsev1::EMBEDDING_TEXT_MODE_TEXT &&
      options.text_mode() != parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "unknown embedding text_mode"};
  }
  if (options.has_model_id() &&
      options.model_id() != "sentence-transformers/all-MiniLM-L6-v2") {
    return {grpc::StatusCode::INVALID_ARGUMENT, "unsupported embedding model_id"};
  }
  if (options.has_batch_size() && options.batch_size() == 0) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "embedding batch_size must be positive"};
  }
  if (!engine_) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "local embeddings are disabled on this server"};
  }
  if (options.has_batch_size() && options.batch_size() > batch_size_) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "embedding batch_size exceeds server limit"};
  }
  return grpc::Status::OK;
}

grpc::Status ChunkEmbedder::embed(const parsev1::EmbeddingOptions& options,
                                 const EmbeddingCancelled& cancelled,
                                 std::vector<parsev1::Chunk>* chunks) const {
  const auto status = validate(options);
  if (!status.ok() || !options.enabled()) return status;
  if (chunks == nullptr) return {grpc::StatusCode::INTERNAL, "missing chunks for embedding"};
  // An empty callback is convenient for non-RPC callers, but backends always
  // receive a callable predicate.
  const EmbeddingCancelled is_cancelled = cancelled ? cancelled : [] { return false; };
  if (is_cancelled()) return cancelled_status();
  const std::size_t batch_limit = options.has_batch_size() ? options.batch_size() : batch_size_;
  const bool contextualized = options.text_mode() == parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED;
  const auto text_mode = contextualized ? parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED
                                        : parsev1::EMBEDDING_TEXT_MODE_TEXT;
  const auto model = model_proto(identity_);
  std::vector<parsev1::ChunkEmbedding> pending;
  std::size_t response_bytes = 0;
  try {
    for (std::size_t start = 0; start < chunks->size();) {
      if (is_cancelled()) return cancelled_status();
      std::vector<std::string> texts;
      std::size_t batch_bytes = 0;
      for (std::size_t index = start; index < chunks->size() && texts.size() < batch_limit;
           ++index) {
        std::size_t bytes = 0;
        if (!text_fits((*chunks)[index], contextualized, config_.max_batch_bytes, &bytes)) {
          return {grpc::StatusCode::RESOURCE_EXHAUSTED, "chunk embedding input exceeds batch byte limit"};
        }
        if (bytes > config_.max_batch_bytes - batch_bytes) break;
        texts.push_back(embedding_text((*chunks)[index], contextualized, bytes));
        batch_bytes += bytes;
      }
      // Reject an output that cannot fit even before paying for inference.
      const std::size_t vector_bytes = texts.size() * identity_.dimensions * sizeof(float);
      const std::size_t remaining = config_.max_response_bytes - response_bytes;
      if (batch_bytes > remaining || vector_bytes > remaining - batch_bytes) {
        return {grpc::StatusCode::RESOURCE_EXHAUSTED, "chunk embeddings exceed response byte limit"};
      }
      EmbeddingBatchResult batch;
      const auto embedded = engine_->embed(texts, is_cancelled, &batch);
      if (is_cancelled()) return cancelled_status();
      if (!embedded.ok()) return embedded;
      if (batch.model != identity_ || batch.vectors.size() != texts.size()) {
        return {grpc::StatusCode::INTERNAL, "embedding backend returned inconsistent model or batch size"};
      }
      for (std::size_t index = 0; index < texts.size(); ++index) {
        const auto& values = batch.vectors[index];
        if (values.size() != identity_.dimensions) {
          return {grpc::StatusCode::INTERNAL, "embedding backend returned incorrect dimensions"};
        }
        double norm_squared = 0;
        for (float value : values) {
          if (!std::isfinite(value)) {
            return {grpc::StatusCode::INTERNAL, "embedding backend returned non-finite values"};
          }
          norm_squared += static_cast<double>(value) * value;
        }
        if (std::abs(norm_squared - 1.0) > 1e-3) {
          return {grpc::StatusCode::INTERNAL, "embedding backend returned non-normalized values"};
        }
        parsev1::ChunkEmbedding embedding;
        *embedding.mutable_model() = model;
        embedding.set_embedded_text(std::move(texts[index]));
        embedding.set_text_mode(text_mode);
        for (float value : values) embedding.add_values(value);
        const auto bytes = embedding.ByteSizeLong();
        if (bytes > config_.max_response_bytes - response_bytes) {
          return {grpc::StatusCode::RESOURCE_EXHAUSTED, "chunk embeddings exceed response byte limit"};
        }
        response_bytes += bytes;
        pending.push_back(std::move(embedding));
      }
      start += texts.size();
    }
    if (is_cancelled()) return cancelled_status();
    // Allocate all destination submessages before moving results so even an
    // allocation failure cannot attach a successfully embedded prefix.
    std::vector<parsev1::Chunk> completed = *chunks;
    for (std::size_t index = 0; index < completed.size(); ++index) {
      *completed[index].mutable_embedding() = std::move(pending[index]);
    }
    if (is_cancelled()) return cancelled_status();
    chunks->swap(completed);
  } catch (const std::bad_alloc&) {
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "insufficient memory for chunk embeddings"};
  } catch (const std::exception&) {
    // Backend exception strings may contain input text or artifact paths.
    return {grpc::StatusCode::INTERNAL, "chunk embedding inference failed"};
  } catch (...) {
    return {grpc::StatusCode::INTERNAL, "chunk embedding inference failed"};
  }
  return grpc::Status::OK;
}

parsev1::EmbeddingCapabilities ChunkEmbedder::capabilities() const {
  parsev1::EmbeddingCapabilities capabilities;
  if (!engine_) return capabilities;
  capabilities.set_available(true);
  *capabilities.mutable_model() = model_proto(identity_);
  capabilities.set_max_batch_size(static_cast<std::uint32_t>(batch_size_));
  capabilities.set_max_batch_bytes(config_.max_batch_bytes);
  capabilities.set_max_response_bytes(config_.max_response_bytes);
  capabilities.add_text_modes(parsev1::EMBEDDING_TEXT_MODE_TEXT);
  capabilities.add_text_modes(parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED);
  return capabilities;
}

}  // namespace grparse
