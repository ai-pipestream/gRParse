#pragma once

#include <memory>
#include <vector>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/embedding.h"

namespace grparse {

// Shared post-chunking step. Does not change chunk boundaries/text/offsets.
// A null engine disables embeddings; construction validates loaded identity
// and resource limits before the server can advertise the capability.
class ChunkEmbedder {
 public:
  explicit ChunkEmbedder(std::shared_ptr<EmbeddingEngine> engine = {},
                         EmbeddingConfig config = {});

  grpc::Status validate(const ai::pipestream::parse::v1::EmbeddingOptions& options) const;
  // Checks cancellation between batches and after inference. On any failure,
  // chunks are untouched, including embeddings from earlier successful batches.
  grpc::Status embed(const ai::pipestream::parse::v1::EmbeddingOptions& options,
                     const EmbeddingCancelled& cancelled,
                     std::vector<ai::pipestream::parse::v1::Chunk>* chunks) const;
  ai::pipestream::parse::v1::EmbeddingCapabilities capabilities() const;

 private:
  std::shared_ptr<EmbeddingEngine> engine_;
  EmbeddingConfig config_;
  EmbeddingModelIdentity identity_;
  std::size_t batch_size_ = 0;
};

}  // namespace grparse
