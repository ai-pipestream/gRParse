// Independent integration reference for the pinned MiniLM artifacts.
// Uses the upstream tokenizer C postprocessor, a separate ORT session, and
// explicit masked pooling. No production tokenization/inference/pooling helper
// is called to calculate expectations. ORT and the tokenizer library themselves
// remain shared dependencies; the optional official OV IR adds a second graph
// and runtime. This is not an upstream golden-vector or PyTorch-export oracle.
// Recipe and example inputs:
// https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/blob/826711e54e001c83835913827a843d8dd0a1def9/README.md
// Artifact hashes match models/embeddings/MANIFEST at that same revision.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <onnxruntime_cxx_api.h>
#include <tokenizers_c.h>
#ifdef GRPARSE_REFERENCE_OPENVINO
#include <openvino/c/openvino.h>
#endif

#include "grparse/embedding.h"
#include "support/check.h"
#include "../src/targets/sha256.h"

namespace {
namespace fs = std::filesystem;
using grparse_test::require;
constexpr std::size_t kDimensions = 384;
constexpr std::size_t kMaxBatch = 4;
constexpr std::string_view kRevision = "826711e54e001c83835913827a843d8dd0a1def9";
using Vectors = std::vector<std::vector<float>>;

struct Artifact {
  const char* name;
  std::size_t size;
  const char* sha256;
};
constexpr Artifact kTokenizer{"tokenizer.json", 466247,
    "be50c3628f2bf5bb5e3a7f17b1f74611b2561a3a27eeab05e5aa30f411572037"};
constexpr Artifact kOnnx{"model.onnx", 90405214,
    "6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452"};
#ifdef GRPARSE_REFERENCE_OPENVINO
constexpr Artifact kXml{"openvino_model.xml", 211315,
    "f87dd1482b2a745f8c699b81ddd9cbcad666a193be4693abcea44b7ac8c67c1e"};
constexpr Artifact kBin{"openvino_model.bin", 90265744,
    "8b86cab4722e2aefab310cf96d4d5a9eb3b187f7d9670a082afc55c7fa0d392a"};
#endif

bool enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && std::string_view(value) == "1";
}

std::string verified_bytes(const fs::path& root, const Artifact& artifact) {
  const auto path = root / artifact.name;
  require(fs::file_size(path) == artifact.size, std::string(artifact.name) + " size mismatch");
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), std::string(artifact.name) + " is unreadable");
  std::string bytes(artifact.size, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  require(input.gcount() == static_cast<std::streamsize>(bytes.size()) &&
              input.peek() == std::char_traits<char>::eof() && !input.bad(),
          std::string(artifact.name) + " could not be read completely");
  require(grparse::targets::sha256_hex(bytes) == artifact.sha256,
          std::string(artifact.name) + " SHA256 mismatch");
  return bytes;
}

struct Tokens {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::vector<int64_t> ids;
  std::vector<int64_t> mask;
  std::vector<int64_t> types;
};

class ReferenceTokenizer {
 public:
  explicit ReferenceTokenizer(const std::string& raw) {
    // Deliberately independent of load_hf_tokenizer_json's cursor parser.
    google::protobuf::Struct json;
    require(google::protobuf::util::JsonStringToMessage(raw, &json).ok(), "reference tokenizer JSON parse");
    json.mutable_fields()->erase("padding");
    json.mutable_fields()->erase("truncation");
    require(json.fields().contains("post_processor"), "reference needs the model's postprocessor");
    std::string configured;
    require(google::protobuf::util::MessageToJsonString(json, &configured).ok(), "reference tokenizer JSON encode");
    handle_ = tokenizers_new_from_str(configured.data(), configured.size());
    require(handle_ != nullptr, "reference tokenizer creation");
  }
  ~ReferenceTokenizer() { if (handle_) tokenizers_free(handle_); }
  ReferenceTokenizer(const ReferenceTokenizer&) = delete;
  ReferenceTokenizer& operator=(const ReferenceTokenizer&) = delete;

  std::vector<int64_t> encode(const std::string& text) const {
    struct Encoding {
      TokenizerEncodeResult value{};
      ~Encoding() { tokenizers_free_encode_results(&value, 1); }
    } encoded;
    // The upstream postprocessor inserts special tokens; no hard-coded CLS/SEP.
    tokenizers_encode(handle_, text.data(), text.size(), 1, &encoded.value);
    require(encoded.value.len >= 2, "postprocessor must add special tokens");
    return {encoded.value.token_ids, encoded.value.token_ids + encoded.value.len};
  }

  Tokens batch(const std::vector<std::string>& texts) const {
    require(!texts.empty() && texts.size() <= kMaxBatch, "reference batch bound");
    std::vector<std::vector<int64_t>> rows;
    Tokens result;
    result.rows = texts.size();
    for (const auto& text : texts) {
      require(text.size() <= 8192, "reference input byte bound");
      rows.push_back(encode(text));
      require(rows.back().size() <= 256, "reference token budget exceeded");
      result.columns = std::max(result.columns, rows.back().size());
    }
    int32_t pad = -1;
    tokenizers_token_to_id(handle_, "[PAD]", 5, &pad);
    require(pad >= 0, "reference PAD token missing");
    result.ids.assign(result.rows * result.columns, pad);
    result.mask.assign(result.ids.size(), 0);
    result.types.assign(result.ids.size(), 0);
    for (std::size_t row = 0; row < result.rows; ++row) {
      for (std::size_t column = 0; column < rows[row].size(); ++column) {
        result.ids[row * result.columns + column] = rows[row][column];
        result.mask[row * result.columns + column] = 1;
      }
    }
    return result;
  }

 private:
  TokenizerHandle handle_ = nullptr;
};

// Model-card equation: sum(hidden * expanded_mask) / sum(expanded_mask),
// followed by L2 normalization. Walk the actual mask, not encoded lengths.
Vectors pool(const Tokens& tokens, const std::vector<float>& hidden) {
  require(hidden.size() == tokens.rows * tokens.columns * kDimensions, "reference hidden shape");
  Vectors result(tokens.rows, std::vector<float>(kDimensions));
  for (std::size_t row = 0; row < tokens.rows; ++row) {
    std::array<double, kDimensions> summed{};
    double count = 0;
    for (std::size_t column = 0; column < tokens.columns; ++column) {
      const auto index = row * tokens.columns + column;
      const double mask = static_cast<double>(tokens.mask[index]);
      count += mask;
      for (std::size_t dim = 0; dim < kDimensions; ++dim) {
        const float value = hidden[index * kDimensions + dim];
        require(std::isfinite(value), "reference hidden value is not finite");
        summed[dim] += static_cast<double>(value) * mask;
      }
    }
    require(count > 0, "reference mask is empty");
    double squared_norm = 0;
    for (auto& value : summed) {
      value /= count;
      squared_norm += value * value;
    }
    require(std::isfinite(squared_norm) && squared_norm > 0, "reference pooled norm");
    const double length = std::sqrt(squared_norm);
    for (std::size_t dim = 0; dim < kDimensions; ++dim)
      result[row][dim] = static_cast<float>(summed[dim] / length);
  }
  return result;
}

void verify_pool_equation() {
  // Analytic unit case, NOT a fabricated MiniLM golden vector: the masked
  // sentinel must not contribute, and a 3-4 vector normalizes to 0.6-0.8.
  Tokens tokens;
  tokens.rows = 1;
  tokens.columns = 3;
  tokens.mask = {1, 0, 1};
  std::vector<float> hidden(3 * kDimensions, 0);
  hidden[0] = 6;
  hidden[2 * kDimensions + 1] = 8;
  std::fill_n(hidden.begin() + kDimensions, kDimensions, 1000.0f);
  const auto actual = pool(tokens, hidden);
  require(std::abs(actual[0][0] - 0.6f) < 1e-7f &&
              std::abs(actual[0][1] - 0.8f) < 1e-7f, "reference masked pooling equation");
  for (std::size_t dim = 2; dim < kDimensions; ++dim)
    require(actual[0][dim] == 0, "masked sentinel leaked into reference pooling");
}

class OrtReference {
 public:
  explicit OrtReference(const std::string& model)
      : env_(ORT_LOGGING_LEVEL_WARNING, "embedding-independent-reference"), session_(nullptr) {
    env_.DisableTelemetryEvents();
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(2);
    options.SetInterOpNumThreads(1);
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    options.AddConfigEntry("session.inter_op.allow_spinning", "0");
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL);
    session_ = Ort::Session(env_, model.data(), model.size(), options);
    require(session_.GetInputCount() == 3 && session_.GetOutputCount() == 1, "reference ONNX ports");
  }
  Vectors run(const Tokens& tokens) {
    const std::array<int64_t, 2> shape{static_cast<int64_t>(tokens.rows), static_cast<int64_t>(tokens.columns)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<Ort::Value, 3> inputs{
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.ids.data()), tokens.ids.size(), shape.data(), 2),
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.mask.data()), tokens.mask.size(), shape.data(), 2),
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.types.data()), tokens.types.size(), shape.data(), 2)};
    constexpr std::array<const char*, 3> names{"input_ids", "attention_mask", "token_type_ids"};
    Ort::AllocatorWithDefaultOptions allocator;
    auto name = session_.GetOutputNameAllocated(0, allocator);
    const char* outputs[]{name.get()};
    auto output = session_.Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), outputs, 1);
    const auto info = output[0].GetTensorTypeAndShapeInfo();
    require(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
                info.GetShape() == std::vector<int64_t>{shape[0], shape[1], 384}, "reference ORT hidden output");
    const float* data = output[0].GetTensorData<float>();
    return pool(tokens, {data, data + info.GetElementCount()});
  }
 private:
  Ort::Env env_;
  Ort::Session session_;
};

#ifdef GRPARSE_REFERENCE_OPENVINO
void check_ov(ov_status_e status) {
  require(status == OK, "OpenVINO IR CPU reference failed, status=" + std::to_string(static_cast<int>(status)));
}
template <typename T, void (*Free)(T*)>
using OvHandle = std::unique_ptr<T, decltype(Free)>;

class OvReference {
 public:
  OvReference(const std::string& xml, const std::string& weights) {
    ov_core_t* core = nullptr;
    check_ov(ov_core_create(&core));
    core_.reset(core);
    int64_t size = static_cast<int64_t>(weights.size());
    ov_shape_t shape{1, &size};
    ov_tensor_t* raw_weights = nullptr;
    check_ov(ov_tensor_create_from_host_ptr(U8, shape, const_cast<char*>(weights.data()), &raw_weights));
    OvHandle<ov_tensor_t, ov_tensor_free> tensor(raw_weights, ov_tensor_free);
    ov_model_t* model = nullptr;
    check_ov(ov_core_read_model_from_memory_buffer(core_.get(), xml.data(), xml.size(), tensor.get(), &model));
    OvHandle<ov_model_t, ov_model_free> network(model, ov_model_free);
    ov_compiled_model_t* compiled = nullptr;
    // Explicit CPU is a test oracle, never a production GPU fallback.
    check_ov(ov_core_compile_model(core_.get(), network.get(), "CPU", 8, &compiled,
        "INFERENCE_PRECISION_HINT", "f32", "PERFORMANCE_HINT", "LATENCY",
        "INFERENCE_NUM_THREADS", "2", "NUM_STREAMS", "1"));
    compiled_.reset(compiled);
    ov_infer_request_t* request = nullptr;
    check_ov(ov_compiled_model_create_infer_request(compiled_.get(), &request));
    request_.reset(request);
  }
  Vectors run(const Tokens& tokens) {
    std::array<int64_t, 2> dims{static_cast<int64_t>(tokens.rows), static_cast<int64_t>(tokens.columns)};
    const ov_shape_t shape{2, dims.data()};
    constexpr std::array<const char*, 3> names{"input_ids", "attention_mask", "token_type_ids"};
    const std::array<const std::vector<int64_t>*, 3> values{&tokens.ids, &tokens.mask, &tokens.types};
    for (std::size_t i = 0; i < names.size(); ++i) {
      ov_tensor_t* raw = nullptr;
      check_ov(ov_tensor_create_from_host_ptr(I64, shape, const_cast<int64_t*>(values[i]->data()), &raw));
      OvHandle<ov_tensor_t, ov_tensor_free> input(raw, ov_tensor_free);
      check_ov(ov_infer_request_set_tensor(request_.get(), names[i], input.get()));
    }
    check_ov(ov_infer_request_infer(request_.get()));
    ov_tensor_t* raw = nullptr;
    check_ov(ov_infer_request_get_output_tensor_by_index(request_.get(), 0, &raw));
    OvHandle<ov_tensor_t, ov_tensor_free> output(raw, ov_tensor_free);
    ov_element_type_e type = UNDEFINED;
    check_ov(ov_tensor_get_element_type(output.get(), &type));
    ov_shape_t actual{};
    check_ov(ov_tensor_get_shape(output.get(), &actual));
    const bool matches = actual.rank == 3 && actual.dims[0] == dims[0] &&
        actual.dims[1] == dims[1] && actual.dims[2] == 384;
    ov_shape_free(&actual);
    require(type == F32 && matches, "reference OV IR hidden output");
    void* data = nullptr;
    check_ov(ov_tensor_data(output.get(), &data));
    const auto* values_ptr = static_cast<const float*>(data);
    return pool(tokens, {values_ptr, values_ptr + tokens.rows * tokens.columns * kDimensions});
  }
 private:
  OvHandle<ov_core_t, ov_core_free> core_{nullptr, ov_core_free};
  OvHandle<ov_compiled_model_t, ov_compiled_model_free> compiled_{nullptr, ov_compiled_model_free};
  OvHandle<ov_infer_request_t, ov_infer_request_free> request_{nullptr, ov_infer_request_free};
};
#endif

void compare(const Vectors& expected, const Vectors& actual, const std::string& label) {
  // Fixed FP32 acceptance bounds, not automatically calibrated to an output.
  constexpr double kAbsolute = 2e-5;
  constexpr double kRelative = 2e-4;
  constexpr double kMinCosine = 0.99999;
  require(expected.size() == actual.size(), label + " row count");
  double max_error = 0;
  for (std::size_t row = 0; row < expected.size(); ++row) {
    require(expected[row].size() == kDimensions && actual[row].size() == kDimensions, label + " dimensions");
    double dot = 0, expected_norm = 0, actual_norm = 0;
    for (std::size_t dim = 0; dim < kDimensions; ++dim) {
      const double a = expected[row][dim], b = actual[row][dim];
      require(std::isfinite(a) && std::isfinite(b), label + " non-finite coordinate");
      const double error = std::abs(a - b);
      max_error = std::max(max_error, error);
      require(error <= kAbsolute + kRelative * std::abs(a),
          label + " coordinate mismatch row=" + std::to_string(row) + " dim=" + std::to_string(dim) +
          " abs_error=" + std::to_string(error));
      dot += a * b;
      expected_norm += a * a;
      actual_norm += b * b;
    }
    require(std::abs(std::sqrt(expected_norm) - 1) <= 1e-5 &&
                std::abs(std::sqrt(actual_norm) - 1) <= 1e-5, label + " unit norm");
    require(dot / std::sqrt(expected_norm * actual_norm) >= kMinCosine, label + " cosine mismatch");
  }
  std::println("reference {}: rows={}, max_abs_error={:.9g}", label, actual.size(), max_error);
}

std::string repeated_hello(int count) {
  std::string text;
  for (int i = 0; i < count; ++i) text += "hello ";
  return text;
}

int run(const fs::path& directory) {
  const auto tokenizer_bytes = verified_bytes(directory, kTokenizer);
  const auto model_bytes = verified_bytes(directory, kOnnx);
  ReferenceTokenizer tokenizer(tokenizer_bytes);
  OrtReference reference(model_bytes);
#ifdef GRPARSE_REFERENCE_OPENVINO
  const auto xml = verified_bytes(directory, kXml);
  const auto weights = verified_bytes(directory, kBin);
  OvReference ov_reference(xml, weights);
#endif
  grparse::EmbeddingConfig config;
  config.backend = grparse::EmbeddingBackend::cpu;
  config.model_dir = directory;
  config.max_batch_size = kMaxBatch;
  const auto production = grparse::make_embedding_engine(config);
  require(production != nullptr, "production CPU engine missing");
  const auto identity = production->identity();
  require(identity.model_id == "sentence-transformers/all-MiniLM-L6-v2" &&
              identity.revision == kRevision && identity.backend == "cpu" &&
              identity.dimensions == 384 && identity.normalized && identity.max_input_tokens == 256 &&
              identity.pooling == "attention-mask-mean", "production identity mismatch");
  require(tokenizer.encode(repeated_hello(254)).size() == 256, "reference exact-limit token count");
  require(tokenizer.encode(repeated_hello(255)).size() == 257, "reference must not truncate oversized text");
  require(tokenizer.encode(repeated_hello(129)).size() == 131, "reference must remove stored 128-token truncation");

  auto check_batch = [&](const std::vector<std::string>& texts, const std::string& label) {
    const auto tokens = tokenizer.batch(texts);
    const auto expected = reference.run(tokens);
    grparse::EmbeddingBatchResult actual;
    const auto status = production->embed(texts, {}, &actual);
    require(status.ok(), label + " production failed: " + status.error_message());
    require(actual.model == identity, label + " result provenance");
    compare(expected, actual.vectors, label + "/ORT-vs-production");
#ifdef GRPARSE_REFERENCE_OPENVINO
    compare(ov_reference.run(tokens), actual.vectors, label + "/OV-IR-CPU-vs-production");
#endif
    return actual.vectors;
  };
  // Inputs from the pinned upstream model card; expected vectors are computed,
  // never copied from a truncated README printout or generated by production.
  check_batch({"This is an example sentence", "Each sentence is converted"}, "model-card");
  const auto mixed = check_batch({"", "Héllo, 世界", "same input", "same input"}, "empty-unicode-duplicates");
  compare({mixed[2]}, {mixed[3]}, "duplicate-in-batch");
  const auto alone = check_batch({"same input"}, "singleton");
  compare(alone, {mixed[2]}, "padding-invariance");
  const auto long_batch = check_batch({repeated_hello(129), repeated_hello(254), "same input"}, "token-boundaries");
  compare(alone, {long_batch[2]}, "long-padding-invariance");
  grparse::EmbeddingBatchResult rejected;
  rejected.model = identity;
  rejected.vectors = {{42.0f}};
  const auto status = production->embed({"valid prefix", repeated_hello(255)}, {}, &rejected);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              rejected.model == identity && rejected.vectors == Vectors{{42.0f}},
          "255 content tokens must be rejected atomically, not truncated");
  std::println("embedding-reference-test: passed revision={}, tokenizer=C-postprocessor, ORT={}, batch<=4, threads=2",
               kRevision, OrtGetApiBase()->GetVersionString());
#ifdef GRPARSE_REFERENCE_OPENVINO
  std::println("embedding-reference-test: official OpenVINO IR CPU FP32 comparison passed");
#else
  std::println("embedding-reference-test: OpenVINO IR reference not compiled; ONNX/tokenizer libraries remain shared");
#endif
  return EXIT_SUCCESS;
}
}  // namespace

int main() {
  try {
    verify_pool_equation();
#ifndef GRPARSE_REFERENCE_OPENVINO
    require(!enabled("GRPARSE_EMBEDDING_REFERENCE_REQUIRE_OV"), "OpenVINO IR reference required but not compiled");
#endif
    const bool required = enabled("GRPARSE_EMBEDDING_TEST_REQUIRE") ||
                          enabled("GRPARSE_EMBEDDING_REFERENCE_REQUIRE_OV");
    const char* value = std::getenv("GRPARSE_EMBEDDING_TEST_MODELS");
    std::vector<const Artifact*> artifacts{&kTokenizer, &kOnnx};
#ifdef GRPARSE_REFERENCE_OPENVINO
    artifacts.push_back(&kXml);
    artifacts.push_back(&kBin);
#endif
    if (value == nullptr || *value == '\0') {
      std::println(stderr, "embedding-reference-test: GRPARSE_EMBEDDING_TEST_MODELS is unset");
      return required ? EXIT_FAILURE : 77;
    }
    for (const auto* artifact : artifacts) {
      if (!fs::exists(fs::path(value) / artifact->name)) {
        std::println(stderr, "embedding-reference-test: missing artifact {}", artifact->name);
        return required ? EXIT_FAILURE : 77;
      }
    }
    return run(value);
  } catch (const std::exception& error) {
    std::println(stderr, "embedding-reference-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
