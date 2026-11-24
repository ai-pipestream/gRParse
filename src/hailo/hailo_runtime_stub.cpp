#include "hailo_runtime.h"

#include <stdexcept>

// Built when GRPARSE_HAILO is OFF: the device can be named in configuration
// but never opened, so a selection that needs it fails startup.

namespace grparse::hailo {

bool runtime_built() { return false; }

DeviceIdentity open_device() {
  throw std::runtime_error(
      "this build has no HailoRT; use the image built with GRPARSE_HAILORT_VERSION "
      "(Dockerfile.cpu) on a host with a Hailo-10H");
}

std::unique_ptr<HailoModel> make_hailo_model(const HailoModelSpec&) {
  (void)open_device();
  return nullptr;
}

std::vector<ModelFrames> model_frame_counts() { return {}; }

}  // namespace grparse::hailo
