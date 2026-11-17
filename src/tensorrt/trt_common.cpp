#include "trt_common.h"

#include <cstdio>
#include <format>
#include <print>
#include <stdexcept>

namespace grparse::trt {

void check_cuda(cudaError_t status, std::string_view what) {
  if (status == cudaSuccess) return;
  throw std::runtime_error(std::format("TensorRT {}: {}", what, cudaGetErrorString(status)));
}

void Logger::log(Severity severity, const char* message) noexcept {
  if (severity > Severity::kWARNING) return;
  try {
    std::println(stderr, "gRParse TensorRT: {}", message);
    if (severity <= Severity::kERROR) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!errors_.empty()) errors_ += "; ";
      errors_ += message;
    }
  } catch (...) {
    // A logger must not throw into TensorRT; a lost line is the lesser harm.
  }
}

std::string Logger::take_errors() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string taken;
  taken.swap(errors_);
  return taken;
}

Logger& logger() {
  static Logger instance;
  return instance;
}

DeviceGuard::DeviceGuard(int device) {
  check_cuda(cudaGetDevice(&previous_), "current device");
  if (previous_ == device) return;
  check_cuda(cudaSetDevice(device), std::format("select device {}", device));
  changed_ = true;
}

DeviceGuard::~DeviceGuard() noexcept {
  if (!changed_) return;
  if (const cudaError_t status = cudaSetDevice(previous_); status != cudaSuccess) {
    std::fputs("gRParse TensorRT: could not restore the previous CUDA device\n", stderr);
  }
}

DeviceBuffer::~DeviceBuffer() {
  if (data_ != nullptr) cudaFree(data_);
}

void* DeviceBuffer::reserve(size_t bytes) {
  if (bytes <= size_ && data_ != nullptr) return data_;
  if (data_ != nullptr) cudaFree(data_);
  data_ = nullptr;
  size_ = 0;
  check_cuda(cudaMalloc(&data_, bytes == 0 ? 1 : bytes), "device allocation");
  size_ = bytes == 0 ? 1 : bytes;
  return data_;
}

Stream::Stream() { check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "stream"); }

Stream::~Stream() {
  if (stream_ == nullptr) return;
  cudaStreamSynchronize(stream_);
  cudaStreamDestroy(stream_);
}

GpuIdentity gpu_identity(int device) {
  int count = 0;
  check_cuda(cudaGetDeviceCount(&count), "CUDA device count");
  if (device < 0 || device >= count) {
    throw std::runtime_error(std::format(
        "TensorRT: CUDA device {} does not exist ({} device{} visible)", device, count,
        count == 1 ? "" : "s"));
  }
  cudaDeviceProp properties{};
  check_cuda(cudaGetDeviceProperties(&properties, device), "device properties");
  return GpuIdentity{properties.name, std::format("{}.{}", properties.major, properties.minor)};
}

}  // namespace grparse::trt
