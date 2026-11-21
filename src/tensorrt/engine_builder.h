#pragma once

// Builds one TensorRT engine from an ONNX file.  The only place a builder,
// a parser or an optimization profile is created for the image models.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <NvInfer.h>

#include "grparse_inference_model.h"

namespace grparse::trt {

// Everything that decides an engine's bytes besides the model file and the
// GPU it is built on.
struct EngineRecipe {
  // Names the model in logs and errors ("layout", "OCR detection").
  std::string what;
  // Explicit optimization profiles; may be empty only for a model whose
  // inputs are all static.
  std::vector<ShapeProfile> profiles;
  // TF32 lets FP32 convolutions and matrix products round their inputs to
  // 10 mantissa bits on Ampere and newer.  Off unless GRPARSE_CUDA_TF32=on.
  bool tf32 = false;
};

// Parses `model` (ONNX bytes) and builds a serialized engine on the calling
// thread's current CUDA device.  TensorRT 11 networks are strongly typed:
// every tensor keeps the type the ONNX file gives it, so an FP32 model
// builds an FP32 engine and no builder flag lowers it.  With a cache
// directory, the builder's timing cache is read from and written back to
// `<directory>/timing.cache`, so a second engine reuses the kernel timings
// of the first.
//
// Throws std::runtime_error carrying TensorRT's own parser or builder
// errors.  Nothing else is tried: there is no second builder and no other
// runtime behind this call.
std::unique_ptr<nvinfer1::IHostMemory> build_engine_plan(std::string model,
                                                         const EngineRecipe& recipe,
                                                         const std::filesystem::path& cache_dir);

}  // namespace grparse::trt
