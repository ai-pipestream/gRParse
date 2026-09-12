#include "tensor_backend.h"

#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

namespace grparse::embedding::detail {
namespace {
void check_cuda(cudaError_t status) {
  if (status != cudaSuccess)
    throw std::runtime_error(std::string("embedding CUDA failure: ") + cudaGetErrorString(status));
}

// CUDA's current device belongs to the calling thread, not this engine.
// Declare before device-owned locals so they are destroyed before restoration.
class DeviceGuard {
 public:
  explicit DeviceGuard(int device) {
    check_cuda(cudaGetDevice(&previous_));
    if (previous_ != device) {
      const auto status = cudaSetDevice(device);
      if (status != cudaSuccess) {
        restore();
        check_cuda(status);
      }
      changed_ = true;
    }
  }
  ~DeviceGuard() noexcept { if (changed_) restore(); }
  DeviceGuard(const DeviceGuard&) = delete;
  DeviceGuard& operator=(const DeviceGuard&) = delete;

 private:
  void restore() const noexcept {
    const auto status = cudaSetDevice(previous_);
    if (status != cudaSuccess)
      std::fprintf(stderr, "embedding CUDA device restoration failed: %s\n", cudaGetErrorString(status));
  }
  int previous_ = 0;
  bool changed_ = false;
};

class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override {
    if (severity <= Severity::kWARNING)
      std::cerr << "embedding TensorRT: " << message << '\n';
  }
};

class DeviceBuffer {
 public:
  explicit DeviceBuffer(std::size_t bytes) { check_cuda(cudaMalloc(&data_, bytes)); }
  ~DeviceBuffer() { if (data_) cudaFree(data_); }
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  void* get() const { return data_; }
 private:
  void* data_ = nullptr;
};

class Stream {
 public:
  Stream() { check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking)); }
  ~Stream() { if (stream_) { cudaStreamSynchronize(stream_); cudaStreamDestroy(stream_); } }
  cudaStream_t get() const { return stream_; }
 private:
  cudaStream_t stream_{};
};

class NvidiaBackend final : public TensorBackend {
 public:
  NvidiaBackend(std::string_view model, int gpu_index) : gpu_index_(gpu_index) {
    DeviceGuard device(gpu_index_);
    auto builder = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(logger_));
    if (!builder) throw std::runtime_error("cannot create TensorRT embedding builder");
    auto network = std::unique_ptr<nvinfer1::INetworkDefinition>(builder->createNetworkV2(0));
    if (!network) throw std::runtime_error("cannot create TensorRT embedding network");
    auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, logger_));
    if (!parser || !parser->parse(model.data(), model.size()))
      throw std::runtime_error("cannot import embedding ONNX model into TensorRT");
    if (network->getNbInputs() != 3 || network->getNbOutputs() != 1)
      throw std::runtime_error("TensorRT embedding model must have three inputs and one output");
    auto config = std::unique_ptr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    // TensorRT 11.2.1: the builder retains ownership; callers must not delete
    // the profile. The builder outlives both config and the engine build.
    auto* profile = builder->createOptimizationProfile();
    if (!config || !profile) throw std::runtime_error("cannot configure TensorRT embeddings");
    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 512ULL << 20);
    // FP32 tensor types alone still permit TF32's rounded multiplication.
    // Disable that default for the shared FP32 embedding contract.
    config->clearFlag(nvinfer1::BuilderFlag::kTF32);
    for (int i = 0; i < network->getNbInputs(); ++i) {
      const auto* input = network->getInput(i);
      if (input->getType() != nvinfer1::DataType::kINT64)
        throw std::runtime_error("TensorRT embedding input must be INT64 tokens");
      if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims2{1, 2}) ||
          !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims2{4, 128}) ||
          !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims2{16, 256}))
        throw std::runtime_error("cannot set TensorRT embedding shape profile");
    }
    if (config->addOptimizationProfile(profile) < 0)
      throw std::runtime_error("invalid TensorRT embedding shape profile");
    // The engine is compiled once per provider instance, on the target GPU.
    // No document data or machine-specific serialized plan is written to disk.
    auto plan = std::unique_ptr<nvinfer1::IHostMemory>(builder->buildSerializedNetwork(*network, *config));
    if (!plan) throw std::runtime_error("TensorRT embedding engine build failed");
    // Keep ownership local until construction succeeds: constructor-body
    // locals unwind before members, so the device guard must outlive them.
    auto runtime = std::unique_ptr<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(logger_));
    if (!runtime) throw std::runtime_error("cannot create TensorRT embedding runtime");
    auto engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(plan->data(), plan->size()));
    if (!engine) throw std::runtime_error("cannot load TensorRT embedding engine");
    auto context = std::unique_ptr<nvinfer1::IExecutionContext>(engine->createExecutionContext());
    if (!context) throw std::runtime_error("cannot create TensorRT embedding execution context");
    for (int i = 0; i < engine->getNbIOTensors(); ++i) {
      const char* name = engine->getIOTensorName(i);
      if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        output_name_ = name;
    }
    if (output_name_.empty() || engine->getTensorDataType(output_name_.c_str()) != nvinfer1::DataType::kFLOAT)
      throw std::runtime_error("TensorRT embedding output must be FP32");
    runtime_ = std::move(runtime);
    engine_ = std::move(engine);
    context_ = std::move(context);
  }

  ~NvidiaBackend() override {
    try {
      DeviceGuard device(gpu_index_);
      context_.reset();
      engine_.reset();
      runtime_.reset();
    } catch (const std::exception&) {
      // A broken CUDA runtime must not cause destruction on an unrelated
      // device or terminate the process through a throwing destructor.
      std::fprintf(stderr, "embedding TensorRT cleanup skipped: cannot select owning CUDA device\n");
      (void)context_.release();
      (void)engine_.release();
      (void)runtime_.release();
    }
  }

  std::vector<float> infer(const TokenBatch& tokens) override {
    DeviceGuard device(gpu_index_);
    constexpr std::array<const char*, 3> names{"input_ids", "attention_mask", "token_type_ids"};
    const std::array<const std::vector<int64_t>*, 3> inputs{&tokens.ids, &tokens.mask, &tokens.types};
    const auto bytes = tokens.rows * tokens.columns * sizeof(int64_t);
    std::array<DeviceBuffer, 3> buffers{DeviceBuffer(bytes), DeviceBuffer(bytes), DeviceBuffer(bytes)};
    DeviceBuffer output_buffer(tokens.rows * tokens.columns * 384 * sizeof(float));
    std::vector<float> result(tokens.rows * tokens.columns * 384);
    // Destroy/synchronize the stream before freeing any referenced buffers,
    // including when a shape check or enqueue throws.
    Stream stream;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (!context_->setInputShape(names[i], nvinfer1::Dims2{static_cast<int>(tokens.rows), static_cast<int>(tokens.columns)}) ||
          !context_->setTensorAddress(names[i], buffers[i].get()))
        throw std::runtime_error("cannot bind TensorRT embedding input");
      check_cuda(cudaMemcpyAsync(buffers[i].get(), inputs[i]->data(), bytes, cudaMemcpyHostToDevice, stream.get()));
    }
    const auto shape = context_->getTensorShape(output_name_.c_str());
    if (shape.nbDims != 3 || shape.d[0] != static_cast<int64_t>(tokens.rows) ||
        shape.d[1] != static_cast<int64_t>(tokens.columns) || shape.d[2] != 384)
      throw std::runtime_error("TensorRT embedding output shape does not match tokens");
    if (!context_->setTensorAddress(output_name_.c_str(), output_buffer.get()) || !context_->enqueueV3(stream.get()))
      throw std::runtime_error("TensorRT embedding inference failed");
    check_cuda(cudaMemcpyAsync(result.data(), output_buffer.get(), result.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.get()));
    check_cuda(cudaStreamSynchronize(stream.get()));
    return result;
  }

 private:
  int gpu_index_;
  Logger logger_;
  std::unique_ptr<nvinfer1::IRuntime> runtime_;
  std::unique_ptr<nvinfer1::ICudaEngine> engine_;
  std::unique_ptr<nvinfer1::IExecutionContext> context_;
  std::string output_name_;
};
}  // namespace

std::unique_ptr<TensorBackend> make_nvidia_backend(std::string_view model, int gpu_index) {
  return std::make_unique<NvidiaBackend>(model, gpu_index);
}
}  // namespace grparse::embedding::detail
