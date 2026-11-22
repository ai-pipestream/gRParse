#pragma once

// Shared pieces of the native TensorRT backend (src/tensorrt/): the logger
// that keeps the builder's own error text, CUDA status checks, and the
// per-thread device selection.  Internal to the backend; nothing outside
// src/tensorrt/ and its tests includes it.

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

namespace grparse::trt {

// Throws std::runtime_error("TensorRT <what>: <CUDA error text>") unless
// status is cudaSuccess.
void check_cuda(cudaError_t status, std::string_view what);

// TensorRT reports why a parse or a build failed only through its logger, so
// this one keeps the error lines it saw; a failed build puts them in the
// exception it throws.  Warnings and errors also go to stderr.
class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override;
  // The error lines logged since the last call, joined with "; "; empties
  // the record.
  std::string take_errors();

 private:
  std::mutex mutex_;
  std::string errors_;
};

// The one logger every builder, runtime and parser of the process shares.
Logger& logger();

// CUDA's current device belongs to the calling thread.  Selects `device` for
// the guard's lifetime and restores the previous one after.
class DeviceGuard {
 public:
  explicit DeviceGuard(int device);
  ~DeviceGuard() noexcept;
  DeviceGuard(const DeviceGuard&) = delete;
  DeviceGuard& operator=(const DeviceGuard&) = delete;

 private:
  int previous_ = 0;
  bool changed_ = false;
};

// Device memory that only grows: reserve() returns a block of at least
// `bytes`, reusing the current one when it is large enough.
class DeviceBuffer {
 public:
  DeviceBuffer() = default;
  ~DeviceBuffer();
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  void* reserve(size_t bytes);
  void* data() const { return data_; }

 private:
  void* data_ = nullptr;
  size_t size_ = 0;
};

// A non-blocking CUDA stream, synchronized and destroyed with its owner.
class Stream {
 public:
  Stream();
  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  cudaStream_t get() const { return stream_; }

 private:
  cudaStream_t stream_ = nullptr;
};

// The GPU an engine is built for, as the engine cache keys it.
struct GpuIdentity {
  std::string name;                // "NVIDIA GeForce RTX 4080 SUPER"
  std::string compute_capability;  // "8.9"
};

// Throws when the device index does not exist or CUDA cannot initialize, with
// CUDA's own error text: the message a container started without a GPU gets.
GpuIdentity gpu_identity(int device);

}  // namespace grparse::trt
