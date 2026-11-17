#pragma once

// The Hailo device's Prometheus series: frames each model ran on the chip
// and how often a model was loaded onto it again.  A build or a selection
// without the device renders nothing.

#include <string>
#include <vector>

#include "hailo_runtime.h"

namespace grparse::hailo {

std::string render_hailo_metrics(const std::vector<ModelFrames>& frames);

}  // namespace grparse::hailo
