#pragma once

// The process's models on the Hailo device: each HEF is loaded once, warmed,
// and recorded in the placement ledger, and every engine shares it (the
// device has one neural core; HailoRT's scheduler takes frames from any
// thread).

#include <filesystem>

#include "hailo_placement.h"
#include "hailo_runtime.h"

namespace grparse::hailo {

// The model for `id` from the selection's HEF directory
// (OrtEpSelection::hailo_hef_dir).  The first call loads it and throws when
// the rule is a CPU pin, the HEF is missing, or the device refuses it; later
// calls return the same model.
HailoModel& shared_model(ModelId id);

}  // namespace grparse::hailo
