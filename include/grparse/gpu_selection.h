#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace grparse {

enum class OrtEp;

struct GpuHardware {
  bool nvidia = false;
  bool intel = false;
  bool amd = false;
  bool intel_b70 = false;
  bool other = false;
};

// PCI visibility is intentionally independent of driver initialization. A
// present card with broken drivers or permissions is not a CPU-only host.
GpuHardware read_gpu_hardware(
    const std::filesystem::path& pci_devices = "/sys/bus/pci/devices");
OrtEp choose_auto_execution_provider(const GpuHardware& hardware,
                                     bool cuda_available, bool openvino_available);

// Older NEO drivers expose the verified PCI device ID instead of the
// marketing name. Both identify the B70; other Intel GPU IDs do not.
bool is_b70_openvino_device_name(std::string_view name);

// Generic GPU selection prefers a visible B70. Explicit device indices and
// intentional CPU/NPU selections remain verbatim.
std::string preferred_openvino_device(const std::string& requested);

}  // namespace grparse
