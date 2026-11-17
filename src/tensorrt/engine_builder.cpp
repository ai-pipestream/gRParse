#include "engine_builder.h"

#include <format>
#include <print>
#include <stdexcept>

#include <NvOnnxParser.h>

#include "engine_cache.h"
#include "onnx_dim_names.h"
#include "shape_profiles.h"
#include "trt_common.h"

namespace grparse::trt {
namespace {

constexpr const char* kTimingCacheName = "timing.cache";
// The tactic scratch one layer may use while the builder times it.  The
// widest detector profile needs well under this; the cap keeps a build from
// claiming a GPU it shares with running engines.
constexpr size_t kWorkspaceBytes = 4ULL << 30;

nvinfer1::Dims to_dims(const std::vector<int64_t>& shape, std::string_view what) {
  if (shape.size() > static_cast<size_t>(nvinfer1::Dims::MAX_DIMS)) {
    throw std::invalid_argument(std::format("gRParse {}: shape {} has too many dimensions", what,
                                            shape_text(shape)));
  }
  nvinfer1::Dims dims{};
  dims.nbDims = static_cast<int32_t>(shape.size());
  for (size_t index = 0; index < shape.size(); ++index) dims.d[index] = shape[index];
  return dims;
}

bool is_dynamic(const nvinfer1::Dims& dims) {
  for (int32_t index = 0; index < dims.nbDims; ++index) {
    if (dims.d[index] < 0) return true;
  }
  return false;
}

// A failed TensorRT call: `step`, then whatever the logger recorded.
std::runtime_error build_failure(std::string_view what, std::string_view step) {
  const std::string detail = logger().take_errors();
  return std::runtime_error(std::format("gRParse {}: TensorRT {}{}{}", what, step,
                                        detail.empty() ? "" : ": ", detail));
}

void parse_model(nvonnxparser::IParser& parser, std::string model, std::string_view what) {
  // paddle2onnx names every unknown dimension "?", and the parser takes
  // equal names as equal sizes; give each its own name first.  Bytes the
  // renamer cannot walk are left as they are: the parser below says what is
  // wrong with them.
  try {
    name_unnamed_onnx_dims(&model);
  } catch (const std::invalid_argument&) {
  }
  if (parser.parse(model.data(), model.size())) return;
  std::string errors;
  for (int index = 0; index < parser.getNbErrors(); ++index) {
    if (!errors.empty()) errors += "; ";
    errors += parser.getError(index)->desc();
  }
  logger().take_errors();  // the same lines, already in `errors`
  throw std::runtime_error(std::format("gRParse {}: TensorRT could not import the ONNX model: {}",
                                       what, errors.empty() ? "no parser error given" : errors));
}

void add_profiles(nvinfer1::IBuilder& builder, nvinfer1::INetworkDefinition& network,
                  nvinfer1::IBuilderConfig& config, const EngineRecipe& recipe) {
  const auto input_count = static_cast<size_t>(network.getNbInputs());
  validate_profiles(recipe.profiles, input_count, recipe.what);
  if (recipe.profiles.empty()) {
    for (size_t input = 0; input < input_count; ++input) {
      const nvinfer1::ITensor* tensor = network.getInput(static_cast<int32_t>(input));
      if (!is_dynamic(tensor->getDimensions())) continue;
      throw std::invalid_argument(std::format(
          "gRParse {}: input {} has a dynamic shape and the model declares no optimization "
          "profile",
          recipe.what, tensor->getName()));
    }
    return;
  }
  for (const ShapeProfile& shapes : recipe.profiles) {
    // The builder owns the profile; it outlives the build.
    nvinfer1::IOptimizationProfile* profile = builder.createOptimizationProfile();
    if (profile == nullptr) throw build_failure(recipe.what, "could not create a profile");
    for (size_t input = 0; input < input_count; ++input) {
      const char* name = network.getInput(static_cast<int32_t>(input))->getName();
      const ShapeRange& range = shapes.inputs[input];
      using Selector = nvinfer1::OptProfileSelector;
      if (!profile->setDimensions(name, Selector::kMIN, to_dims(range.min, recipe.what)) ||
          !profile->setDimensions(name, Selector::kOPT, to_dims(range.opt, recipe.what)) ||
          !profile->setDimensions(name, Selector::kMAX, to_dims(range.max, recipe.what))) {
        throw build_failure(recipe.what, std::format("refused the shape range of input {}", name));
      }
    }
    if (config.addOptimizationProfile(profile) < 0) {
      throw build_failure(recipe.what, "refused an optimization profile");
    }
  }
}

// Loads the timing cache file, if any, into the config.  Returns the cache
// object, which must outlive the build.
std::unique_ptr<nvinfer1::ITimingCache> attach_timing_cache(nvinfer1::IBuilderConfig& config,
                                                            const std::filesystem::path& cache_dir) {
  if (cache_dir.empty()) return nullptr;
  const std::string stored = read_cache_file(cache_dir, kTimingCacheName).value_or(std::string());
  std::unique_ptr<nvinfer1::ITimingCache> cache(
      config.createTimingCache(stored.data(), stored.size()));
  // A cache from another GPU or TensorRT version is ignored by TensorRT
  // itself (ignoreMismatch false): the build then times every kernel.
  if (cache == nullptr || !config.setTimingCache(*cache, false)) {
    logger().take_errors();
    cache.reset(config.createTimingCache(nullptr, 0));
    if (cache != nullptr) config.setTimingCache(*cache, false);
  }
  return cache;
}

void store_timing_cache(const nvinfer1::IBuilderConfig& config,
                        const std::filesystem::path& cache_dir) {
  const nvinfer1::ITimingCache* cache = config.getTimingCache();
  if (cache == nullptr) return;
  std::unique_ptr<nvinfer1::IHostMemory> bytes(cache->serialize());
  if (bytes == nullptr) return;
  try {
    write_cache_file(cache_dir, kTimingCacheName, bytes->data(), bytes->size());
  } catch (const std::exception& error) {
    // The engine is built; a timing cache that cannot be saved only costs
    // the next build its head start.
    std::println(stderr, "gRParse TensorRT: timing cache not saved ({})", error.what());
  }
}

}  // namespace

std::unique_ptr<nvinfer1::IHostMemory> build_engine_plan(std::string model,
                                                         const EngineRecipe& recipe,
                                                         const std::filesystem::path& cache_dir) {
  logger().take_errors();
  std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger()));
  if (builder == nullptr) throw build_failure(recipe.what, "could not create a builder");
  std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
  if (network == nullptr) throw build_failure(recipe.what, "could not create a network");
  std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger()));
  if (parser == nullptr) throw build_failure(recipe.what, "could not create an ONNX parser");
  parse_model(*parser, std::move(model), recipe.what);

  std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
  if (config == nullptr) throw build_failure(recipe.what, "could not create a builder config");
  config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, kWorkspaceBytes);
  // TF32 is a builder default; an FP32 engine that must match the CPU
  // reference clears it.
  if (!recipe.tf32) config->clearFlag(nvinfer1::BuilderFlag::kTF32);
  add_profiles(*builder, *network, *config, recipe);
  const std::unique_ptr<nvinfer1::ITimingCache> timing = attach_timing_cache(*config, cache_dir);

  std::unique_ptr<nvinfer1::IHostMemory> plan(builder->buildSerializedNetwork(*network, *config));
  if (plan == nullptr) throw build_failure(recipe.what, "engine build failed");
  if (timing != nullptr) store_timing_cache(*config, cache_dir);
  logger().take_errors();
  return plan;
}

}  // namespace grparse::trt
