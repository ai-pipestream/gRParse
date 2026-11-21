// Proves the TensorRT engine cache's naming and file handling
// (src/tensorrt/engine_cache.h): an engine file is found only under the
// exact model, TensorRT version, GPU and recipe it was built from, the cache
// is off unless a directory is configured, a configured directory that
// cannot be written fails loudly, and files appear whole or not at all.

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "../src/tensorrt/engine_cache.h"
#include "grparse_inference_model.h"
#include "support/check.h"

namespace {

using grparse::trt::EngineCacheKey;
using grparse::trt::engine_cache_file_name;
using grparse_test::require;
using grparse_test::require_equal;

EngineCacheKey base_key() {
  EngineCacheKey key;
  key.model_sha256 = std::string(64, 'a');
  key.tensorrt_version = "11.4.0.106";
  key.gpu_name = "NVIDIA GeForce RTX 4080 SUPER";
  key.compute_capability = "8.9";
  key.profiles.push_back(grparse::static_profile({{1, 3, 640, 640}, {1, 2}}));
  return key;
}

std::filesystem::path scratch_directory(std::string_view label) {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("grparse-engine-cache-{}-{}", label, ::getpid());
  std::filesystem::remove_all(path);
  return path;
}

// Each field of the key moves the file name; an equal key gives it back.
void verify_every_key_field_names_a_different_file() {
  const EngineCacheKey base = base_key();
  std::set<std::string> names{engine_cache_file_name("layout_heron", base)};
  const auto differs = [&](EngineCacheKey key, std::string_view what) {
    require(names.insert(engine_cache_file_name("layout_heron", key)).second,
            std::format("{} changes the engine file name", what));
  };
  EngineCacheKey changed = base;
  changed.model_sha256 = std::string(64, 'b');
  differs(changed, "the model hash");
  changed = base;
  changed.tensorrt_version = "11.4.1.2";
  differs(changed, "the TensorRT version");
  changed = base;
  changed.gpu_name = "NVIDIA GeForce RTX 4090";
  differs(changed, "the GPU name");
  changed = base;
  changed.compute_capability = "9.0";
  differs(changed, "the compute capability");
  changed = base;
  changed.tf32 = true;
  differs(changed, "TF32");
  changed = base;
  changed.recipe_version += 1;
  differs(changed, "the builder recipe version");
  changed = base;
  changed.profiles[0].inputs[0].max = {2, 3, 640, 640};
  differs(changed, "a profile's largest shape");
  changed = base;
  changed.profiles.push_back(base.profiles[0]);
  differs(changed, "one more profile");
  require_equal(engine_cache_file_name("layout_heron", base_key()),
                engine_cache_file_name("layout_heron", base), "an equal key names the same file");
}

void verify_file_name_shape() {
  const std::string name = engine_cache_file_name("ch_PP-OCRv3_det_infer", base_key());
  require(name.starts_with("ch_PP-OCRv3_det_infer-") && name.ends_with(".engine"),
          "the name is <stem>-<digest>.engine");
  require_equal(name.size(), std::string("ch_PP-OCRv3_det_infer-.engine").size() + 32,
                "the digest is 32 hex characters");
  const std::string text = grparse::trt::engine_cache_key_text(base_key());
  for (const char* field : {"model=", "tensorrt=11.4.0.106", "gpu=NVIDIA GeForce RTX 4080 SUPER",
                            "compute=8.9", "tf32=off",
                            "profile [1x3x640x640 1x3x640x640 1x3x640x640] [1x2 1x2 1x2]"}) {
    require(text.find(field) != std::string::npos, std::format("the key text carries {}", field));
  }
}

// Unset means no cache: the default a read-only container runs with.
void verify_cache_is_off_by_default() {
  ::unsetenv("GRPARSE_TENSORRT_CACHE_DIR");
  require(grparse::trt::configured_engine_cache_dir().empty(), "no variable, no cache");
  ::setenv("GRPARSE_TENSORRT_CACHE_DIR", "", 1);
  require(grparse::trt::configured_engine_cache_dir().empty(), "an empty variable, no cache");
  ::setenv("GRPARSE_TENSORRT_CACHE_DIR", "/var/cache/tensorrt", 1);
  require_equal(grparse::trt::configured_engine_cache_dir().string(), "/var/cache/tensorrt",
                "the variable names the directory");
  ::unsetenv("GRPARSE_TENSORRT_CACHE_DIR");
}

void verify_files_round_trip_whole() {
  const auto directory = scratch_directory("roundtrip");
  grparse::trt::require_writable_cache_dir(directory);
  require(std::filesystem::is_directory(directory), "the directory is created");
  require(!grparse::trt::read_cache_file(directory, "absent.engine").has_value(),
          "a file that is not there reads as absent");
  const std::string bytes = std::string("plan\0bytes", 10) + std::string(100000, 'x');
  grparse::trt::write_cache_file(directory, "model-0123.engine", bytes.data(), bytes.size());
  const auto read = grparse::trt::read_cache_file(directory, "model-0123.engine");
  require(read.has_value() && *read == bytes, "the bytes come back unchanged");
  grparse::trt::write_cache_file(directory, "model-0123.engine", "new", 3);
  require_equal(*grparse::trt::read_cache_file(directory, "model-0123.engine"), "new",
                "a second write replaces the file");
  { std::ofstream empty(directory / "empty.engine"); }
  require(!grparse::trt::read_cache_file(directory, "empty.engine").has_value(),
          "an empty file reads as absent, so a build replaces it");
  size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    require(entry.path().extension() != ".tmp", "no temporary file is left behind");
    ++files;
  }
  require_equal(files, size_t{2}, "only the two named files remain");
  std::filesystem::remove_all(directory);
}

// A cache that was asked for and cannot be written stops startup; it is
// never dropped without a word.
void verify_unwritable_directory_is_refused() {
  const auto directory = scratch_directory("unwritable");
  std::filesystem::create_directories(directory);
  const auto blocker = directory / "a-file";
  { std::ofstream file(blocker); file << "x"; }
  bool refused = false;
  try {
    grparse::trt::require_writable_cache_dir(blocker / "cache");
  } catch (const std::runtime_error& error) {
    refused = true;
    const std::string text = error.what();
    require(text.find("GRPARSE_TENSORRT_CACHE_DIR") != std::string::npos &&
                text.find((blocker / "cache").string()) != std::string::npos,
            "the refusal names the variable and the directory");
  }
  require(refused, "a directory that cannot exist is refused");
  refused = false;
  try {
    grparse::trt::write_cache_file(blocker / "cache", "x.engine", "x", 1);
  } catch (const std::runtime_error&) {
    refused = true;
  }
  require(refused, "a write into it throws");
  std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
  return grparse_test::run_test_main("tensorrt-engine-cache-test", "all checks passed", {
      verify_every_key_field_names_a_different_file,
      verify_file_name_shape,
      verify_cache_is_off_by_default,
      verify_files_round_trip_whole,
      verify_unwritable_directory_is_refused,
  });
}
