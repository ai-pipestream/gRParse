#include "levelzero/table_encoder.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>

namespace grparse::levelzero {

// table_encoder.cl as SPIR-V, generated at build time (cmake/LevelZero.cmake).
extern const unsigned char kTableEncoderSpirv[];
extern const size_t kTableEncoderSpirvSize;

namespace {

uint32_t groups_for(int count, uint32_t group) { return (static_cast<uint32_t>(count) + group - 1) / group; }

}  // namespace

TableEncoder::TableEncoder(Device& device, const std::filesystem::path& encoder_model, int max_images)
    : device_(device), plan_(plan_encoder(encoder_model, kInputSide)) {
  const PlanBuffer& in = plan_.buffers[size_t(plan_.input)];
  const PlanBuffer& out = plan_.buffers[size_t(plan_.output)];
  if (in.channels != 3 || out.channels != 256 || out.height != 96) {
    throw std::runtime_error(encoder_model.string() + ": the encoder does not map [3][488][488] to [256][96]");
  }
  ze_module_handle_t module = device_.module(kTableEncoderSpirv, kTableEncoderSpirvSize, "table encoder");
  kernels_ = {device_.kernel(module, "conv", 16, 4, 1),        device_.kernel(module, "depthwise", 16, 4, 1),
              device_.kernel(module, "pointwise", 64, 1, 1),   device_.kernel(module, "pointwise_vector", 256, 1, 1),
              device_.kernel(module, "global_pool", 256, 1, 1),
              device_.kernel(module, "scale", 256, 1, 1),      device_.kernel(module, "resize_nearest", 16, 4, 1),
              device_.kernel(module, "copy", 256, 1, 1),       device_.kernel(module, "transpose", 16, 16, 1)};
  weights_ = static_cast<float*>(device_.allocate(plan_.weights.size() * sizeof(float)));
  activations_ = static_cast<float*>(device_.allocate(size_t(max_images) * plan_.activation_floats * sizeof(float)));
  input_ = activations_ + in.offset;
  device_.upload(weights_, plan_.weights.data(), plan_.weights.size() * sizeof(float));
}

void TableEncoder::launch(ze_command_list_handle_t list, ze_kernel_handle_t kernel, const ze_group_count_t& groups,
                          const std::vector<std::pair<size_t, const void*>>& arguments, uint32_t gx, uint32_t gy,
                          uint32_t gz, ze_event_handle_t signal) const {
  for (uint32_t index = 0; index < arguments.size(); ++index) {
    ze_check(zeKernelSetArgumentValue(kernel, index, arguments[index].first, arguments[index].second),
             "encoder kernel argument");
  }
  ze_check(zeKernelSetGroupSize(kernel, gx, gy, gz), "encoder group size");
  ze_check(zeCommandListAppendLaunchKernel(list, kernel, &groups, signal, 0, nullptr), "encoder launch");
  ze_check(zeCommandListAppendBarrier(list, nullptr, 0, nullptr), "encoder barrier");
}

void TableEncoder::append(ze_command_list_handle_t list, int images, float* features,
                          const ze_event_handle_t* timestamps) const {
  const auto stride = static_cast<uint32_t>(plan_.activation_floats);
  const auto at = [&](int buffer) { return activations_ + plan_.buffers[size_t(buffer)].offset; };
  const auto n = static_cast<uint32_t>(images);
  for (size_t index = 0; index < plan_.steps.size(); ++index) {
    const PlanStep& step = plan_.steps[index];
    ze_event_handle_t signal = timestamps == nullptr ? nullptr : timestamps[index];
    const PlanBuffer& in = plan_.buffers[size_t(step.input)];
    const PlanBuffer& out = plan_.buffers[size_t(step.output)];
    const float* src = at(step.input);
    float* dst = at(step.output);
    const float* w = weights_ + step.weights;
    const float* b = weights_ + step.bias;
    const int act = static_cast<int>(step.activation);
    const int ci = in.channels, co = out.channels, ih = in.height, iw = in.width, oh = out.height, ow = out.width;
    const int hw = oh * ow, count = int(in.size());
    using A = std::vector<std::pair<size_t, const void*>>;
    const auto p = [](const auto& v) { return std::pair<size_t, const void*>{sizeof(v), &v}; };
    switch (step.kind) {
      case StepKind::kPointwise:
        if (hw == 1) {
          launch(list, kernels_.pointwise_vector, {groups_for(co, 16), n, 1},
                 A{p(src), p(dst), p(w), p(b), p(ci), p(co), p(stride), p(act), p(step.alpha), p(step.beta)}, 256, 1,
                 1, signal);
        } else {
          launch(list, kernels_.pointwise, {groups_for(hw, 64), groups_for(co, 16), n},
                 A{p(src), p(dst), p(w), p(b), p(ci), p(co), p(hw), p(stride), p(act), p(step.alpha), p(step.beta)},
                 64, 1, 1, signal);
        }
        break;
      case StepKind::kDepthwise:
        launch(list, kernels_.depthwise, {groups_for(ow, 16), groups_for(oh, 4), uint32_t(co) * n},
               A{p(src), p(dst), p(w), p(b), p(co), p(ih), p(iw), p(oh), p(ow), p(step.kernel), p(step.stride),
                 p(step.pad), p(stride), p(act), p(step.alpha), p(step.beta)},
               16, 4, 1, signal);
        break;
      case StepKind::kConv:
        launch(list, kernels_.conv, {groups_for(ow, 16), groups_for(oh, 4), uint32_t((co + 15) / 16) * n},
               A{p(src), p(dst), p(w), p(b), p(ci), p(co), p(ih), p(iw), p(oh), p(ow), p(step.kernel), p(step.stride),
                 p(step.pad), p(stride), p(act), p(step.alpha), p(step.beta)},
               16, 4, 1, signal);
        break;
      case StepKind::kGlobalPool: {
        const int in_hw = ih * iw;
        launch(list, kernels_.global_pool, {groups_for(ci, 16), n, 1}, A{p(src), p(dst), p(ci), p(in_hw), p(stride)},
               256, 1, 1, signal);
        break;
      }
      case StepKind::kScale: {
        const float* factor = at(step.other);
        const int in_hw = ih * iw;
        launch(list, kernels_.scale, {groups_for(count, 256), n, 1},
               A{p(src), p(factor), p(dst), p(in_hw), p(count), p(stride)}, 256, 1, 1, signal);
        break;
      }
      case StepKind::kResize:
        launch(list, kernels_.resize, {groups_for(ow, 16), groups_for(oh, 4), uint32_t(ci) * n},
               A{p(src), p(dst), p(ci), p(ih), p(iw), p(oh), p(ow), p(stride)}, 16, 4, 1, signal);
        break;
      case StepKind::kCopy: {
        float* slice = dst + size_t(step.channel_offset) * out.height * out.width;
        launch(list, kernels_.copy, {groups_for(count, 256), n, 1}, A{p(src), p(slice), p(count), p(stride)}, 256, 1,
               1, signal);
        break;
      }
      case StepKind::kTranspose: {
        const int channels = in.channels, positions = in.height * in.width;
        const auto out_stride = static_cast<uint32_t>(positions * channels);
        launch(list, kernels_.transpose, {groups_for(channels, 16), groups_for(positions, 16), n},
               A{p(src), p(features), p(channels), p(positions), p(stride), p(out_stride)}, 16, 16, 1, signal);
        break;
      }
    }
  }
}

}  // namespace grparse::levelzero
