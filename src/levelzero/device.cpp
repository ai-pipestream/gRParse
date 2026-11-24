#include "levelzero/device.h"

#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace grparse::levelzero {

std::string ze_name(ze_result_t result) { return std::format("0x{:08x}", static_cast<uint32_t>(result)); }

void ze_check(ze_result_t result, std::string_view step) {
  if (result != ZE_RESULT_SUCCESS) {
    throw std::runtime_error(std::format("Level Zero {} failed ({})", step, ze_name(result)));
  }
}

Device& Device::shared(int gpu_index) {
  static std::mutex mutex;
  static std::map<int, std::unique_ptr<Device>> devices;
  std::lock_guard lock(mutex);
  auto& slot = devices[gpu_index];
  if (slot == nullptr) slot = std::make_unique<Device>(gpu_index);
  return *slot;
}

Device::Device(int gpu_index) {
  ze_check(zeInit(ZE_INIT_FLAG_GPU_ONLY), "initialization (is the Intel GPU driver installed?)");
  uint32_t driver_count = 0;
  ze_check(zeDriverGet(&driver_count, nullptr), "driver count");
  std::vector<ze_driver_handle_t> drivers(driver_count);
  ze_check(zeDriverGet(&driver_count, drivers.data()), "driver list");
  int seen = 0;
  for (ze_driver_handle_t candidate : drivers) {
    uint32_t count = 0;
    ze_check(zeDeviceGet(candidate, &count, nullptr), "device count");
    std::vector<ze_device_handle_t> devices(count);
    ze_check(zeDeviceGet(candidate, &count, devices.data()), "device list");
    for (ze_device_handle_t device : devices) {
      auto properties = ze_desc<ze_device_properties_t>(ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES);
      ze_check(zeDeviceGetProperties(device, &properties), "device properties");
      if (properties.type != ZE_DEVICE_TYPE_GPU || seen++ != gpu_index) continue;
      driver_ = candidate;
      device_ = device;
      description_ = std::format("Level Zero GPU {} ({})", gpu_index, properties.name);
    }
  }
  if (device_ == nullptr) throw std::runtime_error(std::format("Level Zero has no GPU {} ({} found)", gpu_index, seen));
  auto context_desc = ze_desc<ze_context_desc_t>(ZE_STRUCTURE_TYPE_CONTEXT_DESC);
  ze_check(zeContextCreate(driver_, &context_desc, &context_), "context creation");
}

// Modules, kernels and memory live as long as the process (the device is
// process-wide); only the context is released, which frees them all.
Device::~Device() {
  if (context_ != nullptr) zeContextDestroy(context_);
}

ze_module_handle_t Device::module(const unsigned char* spirv, size_t size, std::string_view what) {
  auto desc = ze_desc<ze_module_desc_t>(ZE_STRUCTURE_TYPE_MODULE_DESC);
  desc.format = ZE_MODULE_FORMAT_IL_SPIRV;
  desc.inputSize = size;
  desc.pInputModule = spirv;
  desc.pBuildFlags = "";
  ze_module_handle_t module = nullptr;
  ze_module_build_log_handle_t log = nullptr;
  const ze_result_t built = zeModuleCreate(context_, device_, &desc, &module, &log);
  std::string text;
  if (log != nullptr) {
    size_t length = 0;
    zeModuleBuildLogGetString(log, &length, nullptr);
    text.resize(length);
    zeModuleBuildLogGetString(log, &length, text.data());
    zeModuleBuildLogDestroy(log);
  }
  if (built != ZE_RESULT_SUCCESS) {
    throw std::runtime_error(std::format("the {} kernels did not build ({}): {}", what, ze_name(built), text));
  }
  return module;
}

ze_kernel_handle_t Device::kernel(ze_module_handle_t module, const char* name, uint32_t x, uint32_t y,
                                  uint32_t z) {
  auto desc = ze_desc<ze_kernel_desc_t>(ZE_STRUCTURE_TYPE_KERNEL_DESC);
  desc.pKernelName = name;
  ze_kernel_handle_t kernel = nullptr;
  ze_check(zeKernelCreate(module, &desc, &kernel), std::format("kernel {} creation", name));
  ze_check(zeKernelSetGroupSize(kernel, x, y, z), std::format("kernel {} group size", name));
  return kernel;
}

void* Device::allocate(size_t bytes) {
  auto desc = ze_desc<ze_device_mem_alloc_desc_t>(ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC);
  void* memory = nullptr;
  ze_check(zeMemAllocDevice(context_, &desc, bytes, 64, device_, &memory),
           std::format("device allocation of {} bytes", bytes));
  return memory;
}

void Device::upload(void* device_memory, const void* host, size_t bytes) {
  auto desc = ze_desc<ze_command_queue_desc_t>(ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC);
  desc.mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS;
  ze_command_list_handle_t list = nullptr;
  ze_check(zeCommandListCreateImmediate(context_, device_, &desc, &list), "upload list");
  const ze_result_t copied = zeCommandListAppendMemoryCopy(list, device_memory, host, bytes, nullptr, 0, nullptr);
  zeCommandListDestroy(list);
  ze_check(copied, std::format("upload of {} bytes", bytes));
}

void* Device::allocate_host(size_t bytes) {
  auto desc = ze_desc<ze_host_mem_alloc_desc_t>(ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC);
  void* memory = nullptr;
  ze_check(zeMemAllocHost(context_, &desc, bytes, 64, &memory), std::format("host allocation of {} bytes", bytes));
  return memory;
}

}  // namespace grparse::levelzero
