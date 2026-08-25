#include "grparse_session_ep.h"

#include <atomic>
#include <mutex>
#include <print>
#include <string>
#include <unordered_map>
#include <utility>

namespace grparse {
namespace {

std::mutex selection_mutex;
OrtEpSelection current_selection;
bool explicitly_selected = false;
std::atomic<uint64_t> hook_invocations{0};
std::atomic<int> intra_op_threads{0};

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

// Resolves what a session actually asks for: its own explicit count, or the
// process-wide one, or nothing at all (ONNX Runtime's every-core default).
int resolved_intra_op_threads(int requested) {
  if (requested == kIntraOpAllCores) return 0;
  if (requested > 0) return requested;
  return intra_op_threads.load();
}

// Every session this process builds asks for the same optimization level; the
// engines differ in their models, not in how ORT should compile them.
Ort::SessionOptions session_options(OrtPrecision precision, int threads) {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  append_execution_provider(options, -1, precision, threads);
  return options;
}

}  // namespace

std::string_view ort_ep_name(OrtEp ep) {
  switch (ep) {
    case OrtEp::kCuda: return "CUDA";
    case OrtEp::kOpenVino: return "OpenVINO";
    case OrtEp::kCpu: break;
  }
  return "CPU";
}

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

void set_ort_intra_op_threads(int threads) { intra_op_threads.store(threads > 0 ? threads : 0); }

int ort_intra_op_threads() { return intra_op_threads.load(); }

void append_execution_provider(Ort::SessionOptions& options, int legacy_gpu_index) {
  append_execution_provider(options, legacy_gpu_index, OrtPrecision::kProviderDefault,
                            kIntraOpProcessDefault);
}

void append_execution_provider(Ort::SessionOptions& options, int legacy_gpu_index,
                               OrtPrecision precision, int requested_intra_op_threads) {
  hook_invocations.fetch_add(1);
  const int threads = resolved_intra_op_threads(requested_intra_op_threads);
  if (threads > 0) options.SetIntraOpNumThreads(threads);
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
    case OrtEp::kOpenVino: {
      // Throws if this ONNX Runtime build lacks the OpenVINO provider or the
      // device cannot initialize; make_session decides what that costs.
      std::unordered_map<std::string, std::string> openvino_options{
          {"device_type", selection.openvino_device}};
      // The GPU plugin picks half precision on its own.  A session that says
      // it needs single precision gets it, on every device.
      if (precision == OrtPrecision::kFloat32) openvino_options["precision"] = "FP32";
      if (!selection.openvino_cache_dir.empty()) {
        openvino_options["cache_dir"] = selection.openvino_cache_dir;
      }
      // The OpenVINO plugin runs its own subgraphs on its own pool, so the
      // ONNX Runtime intra-op setting above never reaches them; this is the
      // knob that does.
      if (threads > 0) openvino_options["num_of_threads"] = std::to_string(threads);
      options.AppendExecutionProvider_OpenVINO_V2(openvino_options);
      return;
    }
    case OrtEp::kCpu:
      return;
  }
}

Ort::Session make_session(Ort::Env& env, const std::filesystem::path& model_path,
                          std::string_view what, OrtPrecision precision,
                          int intra_op_threads) {
  const OrtEp ep = ort_ep_selection().ep;
  try {
    Ort::SessionOptions options = session_options(precision, intra_op_threads);
    return Ort::Session(env, model_path.c_str(), options);
  } catch (const std::exception& error) {
    if (ep == OrtEp::kCpu) throw;
    std::println(stderr,
                 "gRParse {}: the {} execution provider would not build {} ({}); this model "
                 "runs on CPU",
                 what, ort_ep_name(ep), model_path.string(), error.what());
  }
  Ort::SessionOptions cpu_options;
  cpu_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  const int threads = resolved_intra_op_threads(intra_op_threads);
  if (threads > 0) cpu_options.SetIntraOpNumThreads(threads);
  return Ort::Session(env, model_path.c_str(), cpu_options);
}

}  // namespace grparse
