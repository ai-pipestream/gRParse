#pragma once

// The SLANet+ encoder on an Intel GPU through Level Zero: the plan
// encoder_plan.h lowers from slanet_plus_encoder.onnx, run by the kernels of
// table_encoder.cl.  The decoder appends it to its own command list, so the
// features go from the encoder to the decoder without leaving the GPU.

#include <level_zero/ze_api.h>

#include <filesystem>
#include <vector>

#include "levelzero/device.h"
#include "levelzero/encoder_plan.h"

namespace grparse::levelzero {

inline constexpr int kInputSide = 488;

class TableEncoder {
 public:
  // Lowers the model, builds the kernels and uploads the weights; images up
  // to `max_images` per run.  Throws naming the cause.
  TableEncoder(Device& device, const std::filesystem::path& encoder_model, int max_images);

  // Where a run reads its images: [max_images][3][488][488], device memory.
  float* input() const { return input_; }
  // Floats from one image's input to the next.
  size_t image_stride() const { return plan_.activation_floats; }

  // Appends one run over `images` images to `list`, each step behind a
  // barrier, writing features [images][256][96] to `features`.  With
  // `timestamps`, step i signals timestamps[i] (one event per step(), for
  // profiling).
  void append(ze_command_list_handle_t list, int images, float* features,
              const ze_event_handle_t* timestamps = nullptr) const;

  size_t steps() const { return plan_.steps.size(); }
  const EncoderPlan& plan() const { return plan_; }

 private:
  struct Kernels {
    ze_kernel_handle_t conv, depthwise, pointwise, pointwise_vector, global_pool, scale, resize, copy, transpose;
  };

  void launch(ze_command_list_handle_t list, ze_kernel_handle_t kernel, const ze_group_count_t& groups,
              const std::vector<std::pair<size_t, const void*>>& arguments, uint32_t gx, uint32_t gy,
              uint32_t gz, ze_event_handle_t signal) const;

  Device& device_;
  EncoderPlan plan_;
  Kernels kernels_{};
  float* weights_ = nullptr;
  float* activations_ = nullptr;
  float* input_ = nullptr;
};

}  // namespace grparse::levelzero
