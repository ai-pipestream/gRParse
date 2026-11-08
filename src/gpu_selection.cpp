#include "grparse/gpu_selection.h"

#include <fstream>
#include <memory>
#include <stdexcept>

#include "grparse_session_ep.h"

#ifdef GRPARSE_EMBED_OPENVINO
#include <openvino/c/openvino.h>
#endif

namespace grparse {
namespace {
// These PCI displays provide a console, not a supported compute GPU. They
// must not prevent automatic CPU operation on headless servers and VMs.
bool console_display_vendor(unsigned vendor) {
  switch (vendor) {
    case 0x1a03:  // ASPEED BMC
    case 0x102b:  // Matrox server console
    case 0x1af4:  // virtio
    case 0x1234:  // QEMU
    case 0x15ad:  // VMware
    case 0x1414:  // Hyper-V
    case 0x80ee:  // VirtualBox
    case 0x1013:  // Cirrus emulated VGA
      return true;
    default: return false;
  }
}

unsigned read_hex(const std::filesystem::path& path) {
  std::ifstream input(path);
  unsigned value = 0;
  if (!(input >> std::hex >> value)) {
    throw std::runtime_error("Cannot read GPU hardware identity: " + path.string());
  }
  return value;
}
}  // namespace

GpuHardware read_gpu_hardware(const std::filesystem::path& pci_devices) {
  if (!std::filesystem::is_directory(pci_devices)) {
    throw std::runtime_error("Cannot discover GPU hardware at " + pci_devices.string() +
                             "; select GRPARSE_ORT_EP explicitly");
  }
  GpuHardware hardware;
  for (const auto& entry : std::filesystem::directory_iterator(pci_devices)) {
    if ((read_hex(entry.path() / "class") & 0xff0000U) != 0x030000U) continue;
    const unsigned vendor = read_hex(entry.path() / "vendor");
    if (vendor == 0x10de) hardware.nvidia = true;
    if (vendor == 0x1002) hardware.amd = true;
    if (vendor != 0x10de && vendor != 0x1002 && vendor != 0x8086 &&
        !console_display_vendor(vendor)) hardware.other = true;
    if (vendor == 0x8086) {
      hardware.intel = true;
      hardware.intel_b70 = hardware.intel_b70 || read_hex(entry.path() / "device") == 0xe223;
    }
  }
  return hardware;
}

OrtEp choose_auto_execution_provider(const GpuHardware& hardware,
                                     bool cuda_available, bool openvino_available) {
  if (hardware.intel_b70) {
    if (openvino_available) return OrtEp::kOpenVino;
    throw std::runtime_error("Intel B70 detected but this image lacks OpenVINO; use Dockerfile.openvino");
  }
  if (hardware.nvidia && cuda_available) return OrtEp::kCuda;
  if (hardware.intel && openvino_available) return OrtEp::kOpenVino;
  if (hardware.nvidia || hardware.intel || hardware.amd || hardware.other) {
    throw std::runtime_error("GPU hardware detected but no compatible execution provider is available; "
                             "use the matching image or explicitly select GRPARSE_ORT_EP=cpu");
  }
  return OrtEp::kCpu;
}

bool is_b70_openvino_device_name(std::string_view name) {
  return name.find("B70") != std::string_view::npos ||
         name.find("[0xe223]") != std::string_view::npos;
}

std::string preferred_openvino_device(const std::string& requested) {
  if (requested != "GPU" || !read_gpu_hardware().intel_b70) return requested;
#ifdef GRPARSE_EMBED_OPENVINO
  ov_core_t* raw_core = nullptr;
  if (ov_core_create(&raw_core) != OK) {
    throw std::runtime_error("B70 detected but OpenVINO device discovery failed");
  }
  std::unique_ptr<ov_core_t, decltype(&ov_core_free)> core(raw_core, ov_core_free);
  ov_available_devices_t devices{};
  if (ov_core_get_available_devices(core.get(), &devices) != OK) {
    throw std::runtime_error("B70 detected but OpenVINO could not enumerate devices");
  }
  std::string selected;
  for (size_t index = 0; index < devices.size; ++index) {
    const std::string device = devices.devices[index];
    if (!device.starts_with("GPU")) continue;
    char* raw_name = nullptr;
    if (ov_core_get_property(core.get(), device.c_str(), "FULL_DEVICE_NAME", &raw_name) != OK) {
      ov_available_devices_free(&devices);
      throw std::runtime_error("OpenVINO could not identify GPU " + device);
    }
    const std::string name(raw_name);
    ov_free(raw_name);
    if (is_b70_openvino_device_name(name)) {
      selected = device == "GPU" ? "GPU.0" : device;
      break;
    }
  }
  ov_available_devices_free(&devices);
  if (selected.empty()) {
    throw std::runtime_error("B70 is present on PCI but OpenVINO cannot use it; check the render device and driver");
  }
  return selected;
#else
  throw std::runtime_error("B70 selection needs the native OpenVINO device discovery built into Dockerfile.openvino");
#endif
}

}  // namespace grparse
