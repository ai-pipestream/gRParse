#pragma once

// One Intel GPU opened through Level Zero, shared by the table structure
// encoder and decoder: the driver, device and context, the module builder,
// and device memory that lives as long as the process.

#include <level_zero/ze_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace grparse::levelzero {

// A descriptor with its structure type set and every other field zero.
template <typename Desc>
Desc ze_desc(ze_structure_type_t type) {
  Desc desc{};
  desc.stype = type;
  return desc;
}

std::string ze_name(ze_result_t result);

// Throws std::runtime_error naming `step` when `result` is a failure: for
// setup, where the caller turns it into a refusal.
void ze_check(ze_result_t result, std::string_view step);

class Device {
 public:
  // The process's handle on GPU `gpu_index` (the n-th Level Zero GPU).
  static Device& shared(int gpu_index);

  explicit Device(int gpu_index);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  ze_device_handle_t device() const { return device_; }
  ze_context_handle_t context() const { return context_; }
  // "Level Zero GPU <n> (<device name>)".
  const std::string& description() const { return description_; }

  // A module from SPIR-V, built for this device; throws with the build log.
  ze_module_handle_t module(const unsigned char* spirv, size_t size, std::string_view what);
  // A kernel of `module` with its group size set.
  ze_kernel_handle_t kernel(ze_module_handle_t module, const char* name, uint32_t x, uint32_t y, uint32_t z);
  // Device memory freed with the device.
  void* allocate(size_t bytes);
  // Copies `bytes` from host memory to device memory and waits for it.
  void upload(void* device_memory, const void* host, size_t bytes);
  // Host memory the device reads and writes directly, freed with the device.
  void* allocate_host(size_t bytes);

 private:
  ze_driver_handle_t driver_ = nullptr;
  ze_device_handle_t device_ = nullptr;
  ze_context_handle_t context_ = nullptr;
  std::string description_;
};

}  // namespace grparse::levelzero
