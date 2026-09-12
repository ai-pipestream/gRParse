// Native acceptance tier for the pinned all-MiniLM-L6-v2 artifacts.
// GRPARSE_EMBEDDING_TEST_BACKEND selects cpu (default), openvino, or tensorrt.
// GPU comparisons use the C++ CPU engine, which shares preprocessing; an
// independent tokenizer/model reference remains a separate requirement.
// Set GRPARSE_EMBEDDING_TEST_MODELS to the downloaded model directory. An
// absent directory skips (77), unless GRPARSE_EMBEDDING_TEST_REQUIRE=1.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/embedding.h"
#include "support/check.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSkipExitCode = 77;
constexpr char kModelId[] = "sentence-transformers/all-MiniLM-L6-v2";
constexpr char kRevision[] = "826711e54e001c83835913827a843d8dd0a1def9";

using grparse_test::require;

bool required() {
  const char* value = std::getenv("GRPARSE_EMBEDDING_TEST_REQUIRE");
  return value != nullptr && std::string(value) == "1";
}

double norm(const std::vector<float>& values) {
  double squared = 0;
  for (float value : values) squared += static_cast<double>(value) * value;
  return std::sqrt(squared);
}

double cosine(const std::vector<float>& left, const std::vector<float>& right) {
  require(left.size() == right.size(), "cosine inputs have matching dimensions");
  double dot = 0;
  double left_squared = 0;
  double right_squared = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    dot += static_cast<double>(left[index]) * right[index];
    left_squared += static_cast<double>(left[index]) * left[index];
    right_squared += static_cast<double>(right[index]) * right[index];
  }
  return dot / std::sqrt(left_squared * right_squared);
}

grparse::EmbeddingBatchResult embed(const std::shared_ptr<grparse::EmbeddingEngine>& engine,
                                    const std::vector<std::string>& texts) {
  grparse::EmbeddingBatchResult result;
  const grpc::Status status = engine->embed(texts, {}, &result);
  require(status.ok(), engine->identity().backend + " embedding failed: " + status.error_message());
  require(result.model == engine->identity(), "each result preserves the loaded model identity");
  require(result.vectors.size() == texts.size(), "every input receives exactly one vector");
  for (const auto& vector : result.vectors) {
    require(vector.size() == 384, "each vector has 384 dimensions");
    for (float value : vector) require(std::isfinite(value), "vector coordinates are finite");
    require(std::abs(norm(vector) - 1.0) <= 1e-5, "each vector is L2 normalized within 1e-5");
  }
  return result;
}

void verify_identity_and_short_batch(const std::shared_ptr<grparse::EmbeddingEngine>& engine,
                                     const std::string& backend) {
  const auto identity = engine->identity();
  require(identity.model_id == kModelId && identity.revision == kRevision &&
              identity.backend == backend && identity.dimensions == 384 &&
              identity.pooling == "attention-mask-mean" &&
              identity.max_input_tokens == 256 && identity.normalized,
          "the factory reports the pinned MiniLM identity and explicitly selected native backend");
  require(engine->max_batch_size() >= 5, "the engine accepts the short acceptance batch");

  const std::vector<std::string> texts{
      "", "plain English", "Héllo, 世界", "repeated input", "repeated input"};
  const auto result = embed(engine, texts);
  require(result.vectors[3] == result.vectors[4],
          "repeated text in one batch produces the same vector");
}

void verify_determinism_and_padding_invariance(
    const std::shared_ptr<grparse::EmbeddingEngine>& engine) {
  const std::vector<std::string> batch{
      "A longer neighboring sentence forces padding on the shorter input below.",
      "padding invariant text", "third"};
  const auto first = embed(engine, batch);
  const auto second = embed(engine, batch);
  require(first.vectors == second.vectors, "repeated inference is deterministic");

  const auto alone = embed(engine, {"padding invariant text"});
  require(cosine(alone.vectors.front(), first.vectors[1]) >= 0.99999,
          "padding from neighboring batch inputs does not materially change an embedding");
}

void verify_token_limit_and_cancellation_are_atomic(
    const std::shared_ptr<grparse::EmbeddingEngine>& engine) {
  std::string too_long;
  for (int index = 0; index < 300; ++index) too_long += "hello ";
  grparse::EmbeddingBatchResult rejected;
  const grpc::Status too_long_status = engine->embed({too_long}, {}, &rejected);
  require(too_long_status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "more than 256 tokens is rejected without truncation");
  require(rejected.vectors.empty(), "a rejected oversized input has no partial vector result");

  grparse::EmbeddingBatchResult cancelled;
  cancelled.model = engine->identity();
  cancelled.vectors = {{42.0F}};
  const grparse::EmbeddingBatchResult before = cancelled;
  const grpc::Status cancelled_status = engine->embed({"cancelled"}, [] { return true; }, &cancelled);
  require(cancelled_status.error_code() == grpc::StatusCode::CANCELLED,
          "a pre-cancelled request returns CANCELLED");
  require(cancelled.model == before.model && cancelled.vectors == before.vectors,
          "cancellation does not publish a partial result");
}

const std::vector<std::string>& retrieval_texts() {
  static const std::vector<std::string> texts{
      "A cat sleeps on the warm windowsill.",
      "A kitten rests beside a sunny window.",
      "The database transaction committed successfully."};
  return texts;
}

void verify_retrieval_ordering(const std::shared_ptr<grparse::EmbeddingEngine>& engine) {
  const auto result = embed(engine, retrieval_texts());
  const auto& query = result.vectors[0];
  require(cosine(query, query) > cosine(query, result.vectors[1]) &&
              cosine(query, result.vectors[1]) > cosine(query, result.vectors[2]),
          "retrieval ranks exact text above related text above unrelated text");
}

void verify_cpu_agreement(const std::shared_ptr<grparse::EmbeddingEngine>& selected,
                          const std::shared_ptr<grparse::EmbeddingEngine>& cpu) {
  auto comparable_identity = selected->identity();
  comparable_identity.backend = "cpu";
  require(comparable_identity == cpu->identity(),
          "GPU and CPU identities agree on artifacts and preprocessing");
  std::vector<std::string> texts{
      "", "short", "Héllo, 世界", "repeated input", "repeated input"};
  texts.insert(texts.end(), retrieval_texts().begin(), retrieval_texts().end());
  const auto expected = embed(cpu, texts);
  const auto actual = embed(selected, texts);
  for (std::size_t index = 0; index < texts.size(); ++index) {
    require(cosine(expected.vectors[index], actual.vectors[index]) >= 0.9999,
            "native GPU/CPU cosine must be at least 0.9999 for input " + std::to_string(index));
  }
}

int run(const fs::path& model_dir) {
  const char* configured = std::getenv("GRPARSE_EMBEDDING_TEST_BACKEND");
  const std::string backend = configured == nullptr ? "cpu" : configured;
  grparse::EmbeddingConfig config;
  if (backend == "cpu") config.backend = grparse::EmbeddingBackend::cpu;
  else if (backend == "openvino") config.backend = grparse::EmbeddingBackend::openvino;
  else if (backend == "tensorrt") config.backend = grparse::EmbeddingBackend::tensorrt;
  else throw std::invalid_argument("GRPARSE_EMBEDDING_TEST_BACKEND must be cpu, openvino, or tensorrt");
  config.model_dir = model_dir;
  config.max_batch_size = 8;
  config.max_batch_bytes = 1024 * 1024;
  const auto engine = grparse::make_embedding_engine(config);
  require(engine != nullptr, "an explicitly selected factory returns an engine");
  verify_identity_and_short_batch(engine, backend);
  verify_determinism_and_padding_invariance(engine);
  verify_retrieval_ordering(engine);
  verify_token_limit_and_cancellation_are_atomic(engine);
  if (backend != "cpu") {
    config.backend = grparse::EmbeddingBackend::cpu;
    const auto cpu = grparse::make_embedding_engine(config);
    require(cpu != nullptr, "CPU comparison engine loads from the same model directory");
    verify_identity_and_short_batch(cpu, "cpu");
    verify_determinism_and_padding_invariance(cpu);
    verify_retrieval_ordering(cpu);
    verify_cpu_agreement(engine, cpu);
  }
  std::println("embedding-model-test: {} passed", backend);
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  try {
    const char* configured = std::getenv("GRPARSE_EMBEDDING_TEST_MODELS");
    if (configured == nullptr || *configured == '\0') {
      std::println(stderr, "embedding-model-test: GRPARSE_EMBEDDING_TEST_MODELS is unset");
      return required() ? EXIT_FAILURE : kSkipExitCode;
    }
    const fs::path model_dir(configured);
    if (!fs::is_directory(model_dir)) {
      std::println(stderr, "embedding-model-test: model directory is unavailable: {}",
                   model_dir.string());
      return required() ? EXIT_FAILURE : kSkipExitCode;
    }
    return run(model_dir);
  } catch (const std::exception& error) {
    std::println(stderr, "embedding-model-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
