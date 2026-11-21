#include "engine_cache.h"

#include <atomic>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#include "../targets/sha256.h"

namespace grparse::trt {
namespace {

void append_shape(std::string& text, const std::vector<int64_t>& shape) {
  for (size_t index = 0; index < shape.size(); ++index) {
    if (index != 0) text += 'x';
    text += std::to_string(shape[index]);
  }
}

// A name no other process and no other thread of this one is writing.
std::filesystem::path temporary_path(const std::filesystem::path& target) {
  static std::atomic<uint64_t> serial{0};
  return std::filesystem::path(
      std::format("{}.{}.{}.tmp", target.string(), ::getpid(), serial.fetch_add(1)));
}

}  // namespace

std::string engine_cache_key_text(const EngineCacheKey& key) {
  std::string text = std::format(
      "recipe={}\nmodel={}\ntensorrt={}\ngpu={}\ncompute={}\ntf32={}\n", key.recipe_version,
      key.model_sha256, key.tensorrt_version, key.gpu_name, key.compute_capability,
      key.tf32 ? "on" : "off");
  for (const ShapeProfile& profile : key.profiles) {
    text += "profile";
    for (const ShapeRange& range : profile.inputs) {
      text += " [";
      append_shape(text, range.min);
      text += ' ';
      append_shape(text, range.opt);
      text += ' ';
      append_shape(text, range.max);
      text += ']';
    }
    text += '\n';
  }
  return text;
}

std::string engine_cache_file_name(std::string_view model_stem, const EngineCacheKey& key) {
  const std::string digest = targets::sha256_hex(engine_cache_key_text(key));
  return std::format("{}-{}.engine", model_stem, digest.substr(0, 32));
}

std::filesystem::path configured_engine_cache_dir() {
  const char* configured = std::getenv("GRPARSE_TENSORRT_CACHE_DIR");
  return configured == nullptr ? std::filesystem::path() : std::filesystem::path(configured);
}

void require_writable_cache_dir(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path probe = temporary_path(directory / "write-probe");
  bool written = false;
  if (!error) {
    std::ofstream out(probe, std::ios::binary | std::ios::trunc);
    out << "probe";
    out.close();
    written = out.good();
  }
  std::error_code ignored;
  std::filesystem::remove(probe, ignored);
  if (!written) {
    throw std::runtime_error(std::format(
        "GRPARSE_TENSORRT_CACHE_DIR={} is not a writable directory{}; unset it to run without "
        "an engine cache",
        directory.string(), error ? " (" + error.message() + ")" : ""));
  }
}

std::optional<std::string> read_cache_file(const std::filesystem::path& directory,
                                           std::string_view name) {
  std::ifstream in(directory / name, std::ios::binary);
  if (!in) return std::nullopt;
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) return std::nullopt;
  if (bytes.empty()) return std::nullopt;
  return bytes;
}

void write_cache_file(const std::filesystem::path& directory, std::string_view name,
                      const void* data, size_t size) {
  const std::filesystem::path target = directory / name;
  const std::filesystem::path temporary = temporary_path(target);
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out.good()) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      throw std::runtime_error("TensorRT engine cache: cannot write " + temporary.string());
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error(std::format("TensorRT engine cache: cannot place {} ({})",
                                         target.string(), error.message()));
  }
}

}  // namespace grparse::trt
