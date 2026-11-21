#pragma once

// The native TensorRT backend of grparse::InferenceModel
// (compat/grparse_inference_model.h): one engine per model file for the
// whole process, built at startup or loaded from the engine cache, run
// through execution contexts that are pooled per engine.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "grparse_inference_model.h"

namespace grparse::trt {

struct TensorRtModelSpec {
  std::filesystem::path model;
  // Names the model in logs, errors and metrics.
  std::string what;
  std::vector<ShapeProfile> profiles;
  int cuda_device = 0;
  bool tf32 = false;
};

// A handle on the process's engine for this model, building or loading the
// engine on first use.  Every handle of one (model, profiles, device, tf32)
// shares the engine and its weights on the GPU; calls from several handles
// run concurrently, each on its own execution context.
//
// Throws std::runtime_error with TensorRT's or CUDA's own message when the
// device is missing, the model does not import, or the engine does not
// build.  No other runtime is tried.
std::unique_ptr<InferenceModel> make_tensorrt_model(const TensorRtModelSpec& spec);

// Whether CUDA device `cuda_device` exists and initializes; `detail` gets
// CUDA's own message when it does not.
bool device_present(int cuda_device, std::string* detail);

// How the process's engines came to be, for the startup log and the suite
// that proves the cache works.
struct EngineCounts {
  uint64_t built = 0;
  uint64_t loaded_from_cache = 0;
};
EngineCounts engine_counts();

}  // namespace grparse::trt
