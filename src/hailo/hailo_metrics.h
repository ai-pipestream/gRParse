#pragma once

// The Hailo device's Prometheus series: frames each model ran on the chip,
// and what the chip's firmware reports about itself.  A build or a selection
// without the device renders nothing.

#include <optional>
#include <string>
#include <vector>

#include "hailo_runtime.h"

namespace grparse::hailo {

std::string render_hailo_metrics(const std::vector<ModelFrames>& frames,
                                 const std::optional<DeviceStats>& stats);

}  // namespace grparse::hailo
