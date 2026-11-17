#pragma once

// The Hailo device behind compat/grparse_inference_model.h: one process-wide
// HailoRT virtual device, one configured model per HEF, shared by every
// engine.  A HEF is a network compiled offline for one chip family at fixed
// input shapes (models/hailo/); nothing here compiles, and nothing here can
// run a model anywhere but on the device: a HEF that will not load or a
// device that does not answer throws, and no caller builds a CPU session in
// its place.
//
// The implementation is src/hailo/hailo_runtime.cpp in a GRPARSE_HAILO=ON
// build and src/hailo/hailo_runtime_stub.cpp otherwise; the stub's
// open_device() throws, so selecting the device in a build without HailoRT
// stops startup with a message naming the image that has it.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "grparse_inference_model.h"

namespace grparse::hailo {

// True when this binary links HailoRT.
bool runtime_built();

struct DeviceIdentity {
  std::string id;            // the PCIe address HailoRT scanned, e.g. 0001:01:00.0
  std::string architecture;  // HAILO10H
  std::string firmware;      // 5.1.1
};

// Opens the device once per process and identifies it.  Throws
// std::runtime_error when HailoRT finds no device, cannot open it, or the
// chip is not a Hailo-10H; the message carries HailoRT's status.
DeviceIdentity open_device();

// What the chip reports about itself: its own memory and die temperature,
// read from its firmware once, the first time this is asked after
// open_device() (the server asks when the models are loaded).  The firmware
// also has a neural-core utilization figure; HailoRT 5.1.1 answers 0 for it
// under load on a Hailo-10H, so it is not carried.
struct DeviceStats {
  int64_t ram_total_bytes = 0;
  int64_t ram_used_bytes = 0;
  float temperature_celsius = 0.0F;
};

// Empty until open_device() has succeeded, and when the query fails.
std::optional<DeviceStats> device_stats();

struct HailoModelSpec {
  std::filesystem::path hef;
  // Names the model in logs, errors and metrics ("OCR detection").
  std::string what;
  // How many frames the device runs through each of the network's contexts
  // before it switches to the next; a multi-context network pays its context
  // switches once per batch.  A call with fewer frames runs what it has.
  uint16_t batch_size = 1;
};

// A HEF's fixed tensors.  Inputs are uint8 NHWC (normalization runs on the
// chip); outputs come back float32, dequantized on the host from the chip's
// 8 or 16 bit numbers.  Shapes have no batch dimension.
struct HailoModelShapes {
  std::vector<TensorSpec> inputs;
  std::vector<TensorSpec> outputs;
};

// run(): each input is [frames, height, width, channels] uint8 with every
// input carrying the same frame count; each output is [frames, ...] float32.
// Safe to call from several threads; calls take turns.  When the device
// aborts the model, the model is loaded onto the device again and the call
// runs once more there; a second failure throws InferenceDeviceError, and
// nothing but the device ever answers the call.
class HailoModel : public InferenceModel {
 public:
  virtual const HailoModelShapes& shapes() const = 0;
};

// Loads the HEF onto the open device.  Throws std::runtime_error naming the
// model and HailoRT's status when the file is missing, is not a HEF for this
// chip, or the device refuses it.  Requires open_device().
std::unique_ptr<HailoModel> make_hailo_model(const HailoModelSpec& spec);

// What each model has done on the device since start, in load order:
// frames of calls that completed, and how often the model was loaded again
// after the device aborted it (0 on a healthy device).
struct ModelFrames {
  std::string model;
  uint64_t frames = 0;
  uint64_t reloads = 0;
};
std::vector<ModelFrames> model_frame_counts();

}  // namespace grparse::hailo
