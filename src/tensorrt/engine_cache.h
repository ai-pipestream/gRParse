#pragma once

// The on-disk cache of built TensorRT engines.  Off unless
// GRPARSE_TENSORRT_CACHE_DIR names a directory, so the image runs in a
// read-only container as it is; with a directory (a volume), a restart loads
// each engine instead of building it again.
//
// An engine file is valid only for what it was built from, so its name
// carries a digest of all of it: the model file's SHA-256, the TensorRT
// version, the GPU's name and compute capability, and the build recipe
// (TF32, optimization profiles, builder settings).  Precision needs no field
// of its own: an engine computes in the types its model file declares, so a
// model file of another precision is another hash.  Change any one
// and the name changes; the old file is ignored, never loaded.
//
// No TensorRT or CUDA type appears here, so this compiles and is tested in
// every flavor.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "grparse_inference_model.h"

namespace grparse::trt {

// Bumped whenever the builder is configured differently (src/tensorrt/
// engine_builder.cpp), so engines built by an older recipe are not reused.
inline constexpr int kBuilderRecipeVersion = 1;

struct EngineCacheKey {
  std::string model_sha256;        // 64 lowercase hex characters
  std::string tensorrt_version;    // "11.4.0.106"
  std::string gpu_name;            // "NVIDIA GeForce RTX 4080 SUPER"
  std::string compute_capability;  // "8.9"
  bool tf32 = false;
  std::vector<ShapeProfile> profiles;
  int recipe_version = kBuilderRecipeVersion;
};

// The canonical text the digest is taken over, one field per line.  Two keys
// that differ in any field give different text.
std::string engine_cache_key_text(const EngineCacheKey& key);

// "<model stem>-<32 hex>.engine": the stem keeps the directory readable, the
// digest decides validity.
std::string engine_cache_file_name(std::string_view model_stem, const EngineCacheKey& key);

// GRPARSE_TENSORRT_CACHE_DIR, or empty when unset or empty (no cache).
std::filesystem::path configured_engine_cache_dir();

// Creates the directory if needed and proves a file can be written in it.
// Throws std::runtime_error naming the variable and the directory otherwise:
// a cache that was asked for and cannot work fails startup instead of
// building every engine on every start without saying so.
void require_writable_cache_dir(const std::filesystem::path& directory);

// The bytes of `name` under `directory`, or nullopt when there is no such
// file or it is empty.
std::optional<std::string> read_cache_file(const std::filesystem::path& directory,
                                           std::string_view name);

// Writes `name` under `directory` through a temporary file and a rename, so
// a reader never sees a partial file and two processes that build the same
// engine leave one whole copy.  Throws on any failure.
void write_cache_file(const std::filesystem::path& directory, std::string_view name,
                      const void* data, size_t size);

}  // namespace grparse::trt
