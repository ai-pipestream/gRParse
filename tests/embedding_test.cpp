#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/embeddings/failure.h"
#include "grparse/chunk_embeddings.h"
#include "grparse/prometheus_metrics.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

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

// What a vendor runtime says when it fails: a status, and text that names a
// path on the server and quotes what it was given. None of it may reach the
// caller; all of it belongs in the server's log.
constexpr char kVendorError[] =
    "OpenVINO embedding operation failed: status -17 (GENERAL_ERROR): device lost\n"
    "while reading /models/embeddings/model.onnx for input \"quarterly figures\"";

// Runs `body` with stderr redirected into a pipe and returns what it wrote.
// A failure report is one line, well under a pipe's buffer.
template <typename Body>
std::string captured_stderr(Body body) {
  int ends[2];
  require(::pipe(ends) == 0, "a pipe for the capture");
  std::fflush(stderr);
  const int saved = ::dup(STDERR_FILENO);
  ::dup2(ends[1], STDERR_FILENO);
  ::close(ends[1]);
  body();
  std::fflush(stderr);
  ::dup2(saved, STDERR_FILENO);
  ::close(saved);
  std::string out;
  char buffer[4096];
  for (ssize_t got; (got = ::read(ends[0], buffer, sizeof buffer)) > 0;) {
    out.append(buffer, static_cast<size_t>(got));
  }
  ::close(ends[0]);
  return out;
}

uint64_t failures(std::string_view backend, grparse::EmbeddingFailureStage stage) {
  return grparse::pipeline_telemetry().embedding(backend).failed(stage);
}

class FakeEngine final : public grparse::EmbeddingEngine {
 public:
  enum class Reply { kNormal, kWrongCount, kWrongDimensions, kNonFinite, kFailure, kThrows };

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
    if (reply_ == Reply::kThrows) throw std::runtime_error(kVendorError);
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

// An engine that throws fails the RPC with words that say nothing of why,
// which is right for the caller and useless for the operator unless the
// reason goes somewhere: one stderr line with the vendor's own text, and a
// count under the backend and the stage that failed.
void verify_a_throwing_engine_is_logged_and_counted_and_stays_generic() {
  grparse::reset_pipeline_telemetry();
  auto engine = std::make_shared<FakeEngine>(FakeEngine::Reply::kThrows);
  auto subject = embedder(engine);
  auto value = chunks({"one"});
  const std::string before = bytes(value);
  grpc::Status status;
  const std::string log =
      captured_stderr([&] { status = subject.embed(enabled_options(), {}, &value); });

  require(status.error_code() == grpc::StatusCode::INTERNAL,
          "a throwing engine is an internal embedding failure");
  require_equal(status.error_message(), "chunk embedding inference failed",
                "the caller is told only that the embedding failed");
  require_unchanged(value, before, "a throwing engine cannot expose a partial response");
  require(log.contains("backend=cpu") && log.contains("stage=inference"),
          "the log line names the backend and the stage: " + log);
  require(log.contains("status -17 (GENERAL_ERROR): device lost") &&
              log.contains("/models/embeddings/model.onnx"),
          "the log line carries the vendor's status and text: " + log);
  require(std::ranges::count(log, '\n') == 1 && log.ends_with('\n'),
          "one failure is one log line, whatever the vendor wrote: " + log);
  require_equal(failures("cpu", grparse::EmbeddingFailureStage::kInference), uint64_t{1},
                "the failure is counted under its backend and stage");
}

// A reply the embedder refuses (a NaN here) is a failure of the backend's
// output: it is counted as one and the log says what was wrong with it.
void verify_a_refused_engine_reply_is_logged_and_counted() {
  grparse::reset_pipeline_telemetry();
  auto engine = std::make_shared<FakeEngine>(FakeEngine::Reply::kNonFinite);
  auto subject = embedder(engine);
  auto value = chunks({"one"});
  grpc::Status status;
  const std::string log =
      captured_stderr([&] { status = subject.embed(enabled_options(), {}, &value); });

  require(status.error_code() == grpc::StatusCode::INTERNAL, "a NaN vector fails the embedding");
  require(log.contains("backend=cpu") && log.contains("stage=output") &&
              log.contains("non-finite"),
          "the log line says the backend returned non-finite values: " + log);
  require_equal(failures("cpu", grparse::EmbeddingFailureStage::kOutput), uint64_t{1},
                "the refused reply is counted as an output failure");
  require_equal(failures("cpu", grparse::EmbeddingFailureStage::kInference), uint64_t{0},
                "and as nothing else");
}

// The report the local engine answers a vendor exception with: the status
// holds the caller's words alone, the log line holds the vendor's, and the
// count lands on the engine's own backend.
void verify_a_failure_report_keeps_the_vendor_text_on_the_server() {
  grparse::reset_pipeline_telemetry();
  grpc::Status status;
  const std::string log = captured_stderr([&] {
    status = grparse::embedding::detail::report_failure(
        "openvino", grparse::EmbeddingFailureStage::kInference, kVendorError,
        "local embedding inference failed");
  });
  require(status.error_code() == grpc::StatusCode::INTERNAL, "the report is an INTERNAL status");
  require_equal(status.error_message(), "local embedding inference failed",
                "the status carries the caller's words and none of the vendor's");
  require(status.error_details().empty(), "and no detail payload either");
  require_equal(log,
                "gRParse embedding failure: backend=openvino stage=inference: OpenVINO "
                "embedding operation failed: status -17 (GENERAL_ERROR): device lost while "
                "reading /models/embeddings/model.onnx for input \"quarterly figures\"\n",
                "the log line");
  require_equal(failures("openvino", grparse::EmbeddingFailureStage::kInference), uint64_t{1},
                "the failure is counted under the engine's backend");

  // A runtime that echoes a whole batch cannot flood the log.
  const std::string flood(100'000, 'x');
  const std::string cut = captured_stderr([&] {
    grparse::embedding::detail::report_failure(
        "cpu", grparse::EmbeddingFailureStage::kTokenize, flood, "local embedding inference failed");
  });
  require(cut.size() < 4096 && cut.contains("stage=tokenize") && cut.ends_with(" [cut]\n"),
          "a long vendor message is cut: " + std::to_string(cut.size()) + " bytes");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("embedding-test", "passed", {
      verify_batches_are_synchronous_and_ordered,
      verify_identity_and_contextualized_provenance,
      verify_invalid_options_and_disabled_engine,
      verify_disabled_is_identical_and_never_calls_engine,
      verify_cancellation_and_bad_engine_replies_are_atomic,
      verify_a_throwing_engine_is_logged_and_counted_and_stays_generic,
      verify_a_refused_engine_reply_is_logged_and_counted,
      verify_a_failure_report_keeps_the_vendor_text_on_the_server,
  });
}
