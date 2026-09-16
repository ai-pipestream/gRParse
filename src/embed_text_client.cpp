#include "grparse/embedding.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sys/resource.h>

namespace {

constexpr std::size_t kMaxInputs = 16;
constexpr std::size_t kMaxInputBytes = 1024 * 1024;

using BenchmarkClock = std::chrono::steady_clock;

struct Benchmark {
  int repeats = 0;
  double model_load_ms = 0;
  double mean_batch_ms = 0;
  double min_batch_ms = std::numeric_limits<double>::max();
  double max_batch_ms = 0;
  long peak_process_rss_kib = 0;
};

bool read_benchmark_repeats(int* repeats) {
  const char* value = std::getenv("GRPARSE_EMBED_BENCHMARK_REPEATS");
  if (value == nullptr) return true;
  const std::string_view text(value);
  if (text.empty()) return false;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *repeats);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         *repeats >= 1 && *repeats <= 1000;
}

bool parse_backend(std::string_view name, grparse::EmbeddingBackend* backend) {
  if (name == "cpu") {
    *backend = grparse::EmbeddingBackend::cpu;
  } else if (name == "openvino") {
    *backend = grparse::EmbeddingBackend::openvino;
  } else if (name == "tensorrt") {
    *backend = grparse::EmbeddingBackend::tensorrt;
  } else {
    return false;
  }
  return true;
}

void write_json_string(std::ostream& output, std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.put('"');
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (byte < 0x20 || byte >= 0x7f) {
          output << "\\u00" << kHex[byte >> 4] << kHex[byte & 0x0f];
        } else {
          output.put(static_cast<char>(byte));
        }
        break;
    }
  }
  output.put('"');
}

bool valid_result(const grparse::EmbeddingBatchResult& result, std::size_t inputs,
                  std::string_view requested_backend) {
  if (result.model.backend != requested_backend || result.model.dimensions == 0 ||
      result.vectors.size() != inputs) {
    return false;
  }
  for (const auto& vector : result.vectors) {
    if (vector.size() != result.model.dimensions) return false;
    for (const float value : vector) {
      if (!std::isfinite(value)) return false;
    }
  }
  return true;
}

void write_result(const grparse::EmbeddingBatchResult& result, const Benchmark& benchmark) {
  std::cout << "{\"model\":";
  write_json_string(std::cout, result.model.model_id);
  std::cout << ",\"revision\":";
  write_json_string(std::cout, result.model.revision);
  std::cout << ",\"backend\":";
  write_json_string(std::cout, result.model.backend);
  std::cout << ",\"dimensions\":" << result.model.dimensions << ",\"embeddings\":[";
  std::cout << std::setprecision(std::numeric_limits<float>::max_digits10);
  for (std::size_t index = 0; index < result.vectors.size(); ++index) {
    if (index != 0) std::cout.put(',');
    std::cout << "{\"text_index\":" << index << ",\"vector\":[";
    const auto& vector = result.vectors[index];
    for (std::size_t dimension = 0; dimension < vector.size(); ++dimension) {
      if (dimension != 0) std::cout.put(',');
      std::cout << vector[dimension];
    }
    std::cout << "]}";
  }
  std::cout << ']';
  if (benchmark.repeats != 0) {
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << ",\"benchmark\":{\"repeats\":" << benchmark.repeats
              << ",\"model_load_ms\":" << benchmark.model_load_ms
              << ",\"mean_batch_ms\":" << benchmark.mean_batch_ms
              << ",\"min_batch_ms\":" << benchmark.min_batch_ms
              << ",\"max_batch_ms\":" << benchmark.max_batch_ms
              << ",\"peak_process_rss_kib\":" << benchmark.peak_process_rss_kib << '}';
  }
  std::cout << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > static_cast<int>(kMaxInputs + 3)) {
    std::cerr << "Usage: grparse-embed-text <cpu|openvino|tensorrt> <model-dir> "
                 "<text> [text...]\n";
    return 64;
  }

  Benchmark benchmark;
  if (!read_benchmark_repeats(&benchmark.repeats)) {
    std::cerr << "grparse-embed-text: invalid benchmark repeats (expected integer 1..1000)\n";
    return 64;
  }

  grparse::EmbeddingConfig config;
  if (!parse_backend(argv[1], &config.backend)) {
    std::cerr << "grparse-embed-text: invalid arguments\n";
    return 64;
  }
  config.model_dir = argv[2];
  config.max_batch_size = kMaxInputs;
  config.max_batch_bytes = kMaxInputBytes;

  std::vector<std::string> texts;
  texts.reserve(static_cast<std::size_t>(argc - 3));
  std::size_t input_bytes = 0;
  for (int argument = 3; argument < argc; ++argument) {
    const std::string text = argv[argument];
    if (text.size() > kMaxInputBytes - input_bytes) {
      std::cerr << "grparse-embed-text: input limit exceeded\n";
      return 64;
    }
    input_bytes += text.size();
    texts.push_back(text);
  }

  try {
    const auto load_start = benchmark.repeats != 0 ? BenchmarkClock::now()
                                                 : BenchmarkClock::time_point{};
    const std::shared_ptr<grparse::EmbeddingEngine> engine =
        grparse::make_embedding_engine(config);
    if (benchmark.repeats != 0) {
      benchmark.model_load_ms =
          std::chrono::duration<double, std::milli>(BenchmarkClock::now() - load_start).count();
    }
    if (!engine) {
      std::cerr << "grparse-embed-text: backend unavailable\n";
      return 1;
    }
    grparse::EmbeddingBatchResult result;
    const grpc::Status status = engine->embed(texts, {}, &result);
    if (!status.ok()) {
      std::cerr << "grparse-embed-text: embedding failed (status "
                << static_cast<int>(status.error_code()) << ")\n";
      return 1;
    }
    if (!valid_result(result, texts.size(), argv[1])) {
      std::cerr << "grparse-embed-text: invalid backend result\n";
      return 1;
    }
    // The original successful call is the warmup and supplies the output vectors.
    // Time only embed(), excluding validation and JSON serialization.
    double total_batch_ms = 0;
    for (int repeat = 0; repeat < benchmark.repeats; ++repeat) {
      grparse::EmbeddingBatchResult repeated_result;
      const auto batch_start = BenchmarkClock::now();
      const auto repeated_status = engine->embed(texts, {}, &repeated_result);
      const double batch_ms =
          std::chrono::duration<double, std::milli>(BenchmarkClock::now() - batch_start).count();
      if (!repeated_status.ok()) {
        std::cerr << "grparse-embed-text: embedding failed (status "
                  << static_cast<int>(repeated_status.error_code()) << ")\n";
        return 1;
      }
      if (!valid_result(repeated_result, texts.size(), argv[1]) ||
          repeated_result.model != result.model) {
        std::cerr << "grparse-embed-text: invalid backend result\n";
        return 1;
      }
      total_batch_ms += batch_ms;
      if (batch_ms < benchmark.min_batch_ms) benchmark.min_batch_ms = batch_ms;
      if (batch_ms > benchmark.max_batch_ms) benchmark.max_batch_ms = batch_ms;
    }
    if (benchmark.repeats != 0) {
      benchmark.mean_batch_ms = total_batch_ms / benchmark.repeats;
      struct rusage usage {};
      if (getrusage(RUSAGE_SELF, &usage) != 0) {
        std::cerr << "grparse-embed-text: process memory measurement failed\n";
        return 1;
      }
      // Linux ru_maxrss is process-lifetime peak resident memory in KiB.
      benchmark.peak_process_rss_kib = usage.ru_maxrss;
    }
    write_result(result, benchmark);
    if (!std::cout) {
      std::cerr << "grparse-embed-text: output failed\n";
      return 1;
    }
    return 0;
  } catch (...) {
    std::cerr << "grparse-embed-text: initialization failed\n";
    return 1;
  }
}
