#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "grparse/chunk_embeddings.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;

constexpr char kModelId[] = "sentence-transformers/all-MiniLM-L6-v2";

grparse::EmbeddingModelIdentity supported_identity() {
  return {
      .model_id = kModelId,
      .revision = "weights-and-tokenizer-sha256",
      .backend = "cpu",
      .pooling = "attention-mask-mean",
      .dimensions = 384,
      .max_input_tokens = 256,
      .normalized = true,
  };
}

std::vector<float> unit_vector(std::size_t dimensions = 384) {
  std::vector<float> values(dimensions, 0.0F);
  if (!values.empty()) values.front() = 1.0F;
  return values;
}

class FakeEngine final : public grparse::EmbeddingEngine {
 public:
  enum class Reply { kNormal, kWrongCount, kWrongDimensions, kNonFinite, kFailure };

  explicit FakeEngine(Reply reply = Reply::kNormal,
                      grparse::EmbeddingModelIdentity identity = supported_identity(),
                      std::size_t max_batch_size = 2)
      : reply_(reply), identity_(std::move(identity)), max_batch_size_(max_batch_size) {}

  grparse::EmbeddingModelIdentity identity() const override { return identity_; }
  std::size_t max_batch_size() const override { return max_batch_size_; }

  grpc::Status embed(const std::vector<std::string>& texts,
                     const grparse::EmbeddingCancelled& cancelled,
                     grparse::EmbeddingBatchResult* result) override {
    ++calls;
    batches.push_back(texts);
    saw_cancellation_callback = static_cast<bool>(cancelled);
    if (reply_ == Reply::kFailure) return {grpc::StatusCode::INTERNAL, "engine failed"};
    result->model = identity_;
    for (std::size_t index = 0; index < texts.size(); ++index) {
      result->vectors.push_back(unit_vector());
    }
    if (reply_ == Reply::kWrongCount && !result->vectors.empty()) {
      result->vectors.pop_back();
    }
    if (reply_ == Reply::kWrongDimensions && !result->vectors.empty()) {
      result->vectors.front() = unit_vector(383);
    }
    if (reply_ == Reply::kNonFinite && !result->vectors.empty()) {
      result->vectors.front().front() = std::numeric_limits<float>::quiet_NaN();
    }
    return grpc::Status::OK;
  }

  int calls = 0;
  bool saw_cancellation_callback = false;
  std::vector<std::vector<std::string>> batches;

 private:
  Reply reply_;
  grparse::EmbeddingModelIdentity identity_;
  std::size_t max_batch_size_;
};

std::vector<parsev1::Chunk> chunks(std::initializer_list<std::string> texts) {
  std::vector<parsev1::Chunk> result;
  int index = 0;
  for (const auto& text : texts) {
    auto& chunk = result.emplace_back();
    chunk.set_chunk_index(index++);
    chunk.set_filename("fixture.txt");
    chunk.set_text(text);
  }
  return result;
}

parsev1::EmbeddingOptions enabled_options() {
  parsev1::EmbeddingOptions options;
  options.set_enabled(true);
  return options;
}

std::string bytes(const std::vector<parsev1::Chunk>& value) {
  std::string result;
  for (const auto& chunk : value) {
    const std::string one = chunk.SerializeAsString();
    result.append(std::to_string(one.size()));
    result.push_back(':');
    result.append(one);
  }
  return result;
}

grparse::ChunkEmbedder embedder(const std::shared_ptr<FakeEngine>& engine,
                                std::size_t server_batch_size = 2) {
  grparse::EmbeddingConfig config;
  config.max_batch_size = server_batch_size;
  config.max_batch_bytes = 4096;
  config.max_response_bytes = 1024 * 1024;
  return grparse::ChunkEmbedder(engine, config);
}

void require_unchanged(const std::vector<parsev1::Chunk>& actual, const std::string& before,
                       const std::string& what) {
  require(bytes(actual) == before, what);
  for (const auto& chunk : actual) require(!chunk.has_embedding(), what + " must attach no vector");
}

void verify_batches_are_synchronous_and_ordered() {
  auto engine = std::make_shared<FakeEngine>();
  auto subject = embedder(engine);
  auto value = chunks({"zero", "one", "two", "three", "four"});
  auto options = enabled_options();
  options.set_batch_size(2);

  const grpc::Status status = subject.embed(options, {}, &value);
  require(status.ok(), "batched embedding succeeds: " + status.error_message());
  require(engine->calls == 3, "five chunks with a batch size of two make three synchronous calls");
  require(engine->batches == std::vector<std::vector<std::string>>({
                                {"zero", "one"}, {"two", "three"}, {"four"}}),
          "each engine call receives its contiguous input-order batch");
  for (int index = 0; index < static_cast<int>(value.size()); ++index) {
    require(value[index].embedding().embedded_text() == value[index].text(),
            "each vector remains attached to its source chunk");
  }
}

void verify_identity_and_contextualized_provenance() {
  auto engine = std::make_shared<FakeEngine>();
  auto subject = embedder(engine);
  auto value = chunks({"Body"});
  value.front().add_headings("Title");
  value.front().add_headings("Section");
  auto options = enabled_options();
  options.set_text_mode(parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED);

  const grpc::Status status = subject.embed(options, {}, &value);
  require(status.ok(), "contextualized embedding succeeds");
  require(engine->batches == std::vector<std::vector<std::string>>({{"Title\nSection\nBody"}}),
          "the engine sees the documented contextualized text");
  const auto& embedding = value.front().embedding();
  require(embedding.embedded_text() == "Title\nSection\nBody",
          "the response preserves the exact tokenizer input");
  require(embedding.text_mode() == parsev1::EMBEDDING_TEXT_MODE_CONTEXTUALIZED,
          "the response records text provenance mode");
  const auto& model = embedding.model();
  require(model.model_id() == kModelId && model.revision() == "weights-and-tokenizer-sha256" &&
              model.backend() == "cpu" && model.pooling() == "attention-mask-mean" &&
              model.dimensions() == 384 && model.max_input_tokens() == 256 && model.normalized(),
          "the response carries immutable model provenance from the engine identity");
  require(embedding.values_size() == 384 && embedding.values(0) == 1.0F,
          "the complete engine vector reaches the source chunk");
}

void verify_invalid_options_and_disabled_engine() {
  auto engine = std::make_shared<FakeEngine>();
  auto subject = embedder(engine);

  parsev1::EmbeddingOptions disabled_with_model;
  disabled_with_model.set_model_id("anything");
  require(subject.validate(disabled_with_model).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "disabled embedding options reject accompanying fields");

  auto unknown_model = enabled_options();
  unknown_model.set_model_id("other/model");
  require(subject.validate(unknown_model).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "an unsupported model assertion is invalid");

  auto zero_batch = enabled_options();
  zero_batch.set_batch_size(0);
  require(subject.validate(zero_batch).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a zero request batch size is invalid");

  auto too_large = enabled_options();
  too_large.set_batch_size(3);
  require(subject.validate(too_large).error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a request batch size above the advertised limit is invalid");

  grparse::EmbeddingConfig config;
  config.max_batch_size = 1;
  config.max_batch_bytes = 1;
  config.max_response_bytes = 1;
  grparse::ChunkEmbedder disabled({}, config);
  require(disabled.validate(enabled_options()).error_code() == grpc::StatusCode::FAILED_PRECONDITION,
          "enabled requests fail clearly when no local engine is loaded");

  bool rejected = false;
  try {
    grparse::EmbeddingConfig bad = config;
    bad.max_batch_size = 0;
    grparse::ChunkEmbedder invalid(engine, bad);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "invalid server resource configuration is rejected at construction");
}

void verify_disabled_is_identical_and_never_calls_engine() {
  auto engine = std::make_shared<FakeEngine>();
  auto subject = embedder(engine);
  auto value = chunks({"one", "two"});
  const std::string before = bytes(value);
  parsev1::EmbeddingOptions disabled;

  const grpc::Status status = subject.embed(disabled, {}, &value);
  require(status.ok(), "the disabled default succeeds");
  require(engine->calls == 0, "disabled embedding never calls the engine");
  require_unchanged(value, before, "disabled embedding leaves chunk responses byte-identical");
}

void verify_cancellation_and_bad_engine_replies_are_atomic() {
  {
    auto engine = std::make_shared<FakeEngine>();
    auto subject = embedder(engine);
    auto value = chunks({"one", "two", "three"});
    const std::string before = bytes(value);
    const grpc::Status status = subject.embed(enabled_options(),
                                              [&] { return engine->calls >= 1; }, &value);
    require(status.error_code() == grpc::StatusCode::CANCELLED,
            "cancellation between synchronous batches returns CANCELLED");
    require(engine->calls == 1 && engine->saw_cancellation_callback,
            "the first batch receives cancellation and the second is not started");
    require_unchanged(value, before, "cancellation cannot expose an embedded prefix");
  }

  for (const auto reply : {FakeEngine::Reply::kWrongCount, FakeEngine::Reply::kWrongDimensions,
                           FakeEngine::Reply::kNonFinite, FakeEngine::Reply::kFailure}) {
    auto engine = std::make_shared<FakeEngine>(reply);
    auto subject = embedder(engine);
    auto value = chunks({"one", "two", "three"});
    const std::string before = bytes(value);
    const grpc::Status status = subject.embed(enabled_options(), {}, &value);
    require(status.error_code() == grpc::StatusCode::INTERNAL,
            "an invalid engine reply becomes an internal embedding failure");
    require_unchanged(value, before, "an invalid engine reply cannot expose a partial response");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("embedding-test", "passed", {
      verify_batches_are_synchronous_and_ordered,
      verify_identity_and_contextualized_provenance,
      verify_invalid_options_and_disabled_engine,
      verify_disabled_is_identical_and_never_calls_engine,
      verify_cancellation_and_bad_engine_replies_are_atomic,
  });
}
