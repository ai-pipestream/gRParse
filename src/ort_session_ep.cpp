#include "grparse_session_ep.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace grparse {
namespace {

std::mutex selection_mutex;
OrtEpSelection current_selection;
bool explicitly_selected = false;
std::atomic<uint64_t> hook_invocations{0};

void append_cuda(Ort::SessionOptions& options, int device) {
  // Same options upstream RapidOcrOnnx used, with the 2 GiB arena limit
  // computed in 64 bits (upstream's int expression overflowed).
  OrtCUDAProviderOptions cuda_options;
  cuda_options.device_id = device;
  cuda_options.arena_extend_strategy = 0;
  cuda_options.gpu_mem_limit = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearch::OrtCudnnConvAlgoSearchExhaustive;
  cuda_options.do_copy_in_default_stream = 1;
  options.AppendExecutionProvider_CUDA(cuda_options);
}

}  // namespace

void set_ort_ep_selection(OrtEpSelection selection) {
  std::lock_guard<std::mutex> lock(selection_mutex);
  current_selection = std::move(selection);
  explicitly_selected = true;
}

OrtEpSelection ort_ep_selection() {
  std::lock_guard<std::mutex> lock(selection_mutex);
  return current_selection;
}

uint64_t ep_hook_invocations() { return hook_invocations.load(); }

void append_execution_provider(Ort::SessionOptions& options, int legacy_gpu_index) {
  hook_invocations.fetch_add(1);
  OrtEpSelection selection;
  bool selected = false;
  {
    std::lock_guard<std::mutex> lock(selection_mutex);
    selection = current_selection;
    selected = explicitly_selected;
  }
  if (!selected) {
    // Upstream RapidOcrOnnx behaviour for callers that never chose a provider.
    if (legacy_gpu_index >= 0) append_cuda(options, legacy_gpu_index);
    return;
  }
  switch (selection.ep) {
    case OrtEp::kCuda:
      append_cuda(options, selection.cuda_device);
      return;
    case OrtEp::kOpenVino:
      // Throws if this ONNX Runtime build lacks the OpenVINO provider or the
      // device cannot initialize — the fail-loud startup the server wants.
      options.AppendExecutionProvider_OpenVINO_V2(
          std::unordered_map<std::string, std::string>{{"device_type", selection.openvino_device}});
      return;
    case OrtEp::kCpu:
      return;
  }
}

}  // namespace grparse
