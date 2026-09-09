#include "grparse_session_ep.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace grparse {
namespace {

std::mutex selection_mutex;
OrtEpSelection current_selection;
bool explicitly_selected = false;
std::atomic<uint64_t> hook_invocations{0};
std::atomic<uint64_t> ep_fallbacks{0};
std::atomic<uint64_t> ep_build_retries{0};
std::atomic<int> injected_build_failures{0};
std::atomic<int> intra_op_threads{0};

// One initial build plus two retries for a model on the OpenVINO provider.
// Its toolchain fails intermittently even on a healthy host, and most of
// those failures are transient, so a small bounded retry converts most
// would-be crashes into successful GPU builds; retrying stays on the GPU.
// What the retry cannot fix is the other failure class: heap corruption and
// longjmp-across-stack inside the toolchain kill the process outright, with
// no exception to catch; those stay fatal (see README "Intel GPUs").
constexpr int kEpBuildMaxAttempts = 3;

// The one lock OvCompileGate takes.  Deliberately not the selection mutex:
// session builds read the selection, so holding this while building must
// never block a caller that only wants to change or read it.
std::mutex compile_mutex;

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

// The CPU-only options make_session and the patched OCR nets build with.
void configure_cpu_options(Ort::SessionOptions& options, int requested_intra_op_threads) {
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  const int threads = resolved_intra_op_threads(requested_intra_op_threads);
  if (threads > 0) options.SetIntraOpNumThreads(threads);
}

bool build_failure_injection_pending() {
  return injected_build_failures.load(std::memory_order_relaxed) > 0;
}

// Throws the synthetic failure while the test-injection counter is positive.
// Only the ort_session_ep test sets that counter; production builds never
// throw here.
void throw_if_build_failure_injected() {
  if (injected_build_failures.load(std::memory_order_relaxed) > 0) {
    injected_build_failures.fetch_sub(1, std::memory_order_relaxed);
    throw std::runtime_error("injected transient execution-provider build failure");
  }
}

// Short growing backoff with jitter, so several sessions whose builds fail
// together do not retry in lockstep and hammer the same JIT compiler.
std::chrono::milliseconds ep_retry_backoff(int failed_attempt) {
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> jitter(0, 100);
  return std::chrono::milliseconds(100 * failed_attempt + jitter(rng));
}

// Runs `attempt` up to kEpBuildMaxAttempts times, returning the first
// success; after the last attempt the exception escapes.  Retries are scoped
// to the OpenVINO provider, whose toolchain fails intermittently even on a
// healthy host and whose retry stays on the GPU; every other provider keeps
// its single-attempt behavior.  The test-injection counter opts into retries
// too, so the loop is exercisable without a GPU.  `what` names the model in
// the retry log lines.
template <typename Build>
decltype(auto) build_with_ep_retries(std::string_view what, OrtEp ep, Build&& attempt) {
  const bool retryable = ep == OrtEp::kOpenVino || build_failure_injection_pending();
  for (int attempt_number = 1;; ++attempt_number) {
    try {
      return std::forward<Build>(attempt)();
    } catch (const std::exception& error) {
      if (!retryable || attempt_number >= kEpBuildMaxAttempts) throw;
      ep_build_retries.fetch_add(1);
      std::println(stderr,
                   "gRParse {}: the {} execution provider failed to build it (attempt {}/{}, "
                   "{}); retrying",
                   what, ort_ep_name(ep), attempt_number, kEpBuildMaxAttempts, error.what());
      std::this_thread::sleep_for(ep_retry_backoff(attempt_number));
    }
  }
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

uint64_t ep_fallback_count() { return ep_fallbacks.load(); }

uint64_t ep_build_retry_count() { return ep_build_retries.load(); }

void ort_ep_test_inject_build_failures(int failures) {
  injected_build_failures.store(failures < 0 ? 0 : failures, std::memory_order_relaxed);
}

OvCompileGate::OvCompileGate() {
  if (ort_ep_selection().ep == OrtEp::kOpenVino) lock_ = std::unique_lock<std::mutex>(compile_mutex);
}

OvCompileGate::~OvCompileGate() = default;

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
  // A CPU failure is a broken model file, deterministic, and the caller wants
  // the error now, so CPU builds skip the retry loop.  The test-injection
  // counter routes through it anyway so the tests can exercise retries
  // without a GPU.
  if (ep == OrtEp::kCpu && !build_failure_injection_pending()) {
    return make_cpu_session(env, model_path, intra_op_threads);
  }
  try {
    return build_with_ep_retries(what, ep, [&]() -> Ort::Session {
      // The OpenVINO provider compiles the model inside the Session
      // constructor; serialize those compiles process-wide, one attempt at a
      // time.  The gate is per attempt so the backoff sleeps between attempts
      // never block another session's build.
      const OvCompileGate compile_gate;
      if (ep == OrtEp::kCpu) {
        // Only reachable under test failure injection.
        throw_if_build_failure_injected();
        return make_cpu_session(env, model_path, intra_op_threads);
      }
      Ort::SessionOptions options = session_options(precision, intra_op_threads);
      throw_if_build_failure_injected();
      return Ort::Session(env, model_path.c_str(), options);
    });
  } catch (const std::exception& error) {
    if (ep == OrtEp::kCpu) throw;
    // Pre-existing policy, unchanged: this model loses its acceleration,
    // not the whole server (the slanet_plus table model lands here by
    // design).  The OCR nets are different: they build through
    // make_rapidocr_session and fail loudly instead.
    ep_fallbacks.fetch_add(1);
    std::println(stderr,
                 "gRParse {}: the {} execution provider would not build {} ({}); this model "
                 "runs on CPU",
                 what, ort_ep_name(ep), model_path.string(), error.what());
  }
  return make_cpu_session(env, model_path, intra_op_threads);
}

Ort::Session* make_rapidocr_session(Ort::Env& env, const std::filesystem::path& model_path,
                                    std::string_view what, int num_thread) {
  const OrtEp ep = ort_ep_selection().ep;
  const auto cpu_build = [&]() -> Ort::Session* {
    Ort::SessionOptions options;
    configure_cpu_options(options, num_thread);
    return new Ort::Session(env, model_path.c_str(), options);
  };
  // A CPU failure is a broken model file, deterministic, and the caller wants
  // the error now, so CPU builds skip the retry loop.  The test-injection
  // counter routes through it anyway so the tests can exercise retries
  // without a GPU.
  if (ep == OrtEp::kCpu && !build_failure_injection_pending()) return cpu_build();
  // No catch: after the retries exhaust, the exception propagates out of the
  // net's initModel and takes the engine down with a clear error.  The
  // openvino flavor never silently degrades an OCR model to CPU.
  return build_with_ep_retries(what, ep, [&]() -> Ort::Session* {
    // Same gate discipline as make_session: one attempt under the gate at a
    // time, backoff sleeps outside it.
    const OvCompileGate compile_gate;
    if (ep == OrtEp::kCpu) {
      // Only reachable under test failure injection.
      throw_if_build_failure_injected();
      return cpu_build();
    }
    // The same options the patched nets used to configure themselves:
    // their thread count, then the central provider hook.
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    if (num_thread > 0) options.SetIntraOpNumThreads(num_thread);
    append_execution_provider(options, -1);
    throw_if_build_failure_injected();
    return new Ort::Session(env, model_path.c_str(), options);
  });
}

Ort::Session make_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                              int intra_op_threads) {
  Ort::SessionOptions cpu_options;
  configure_cpu_options(cpu_options, intra_op_threads);
  return Ort::Session(env, model_path.c_str(), cpu_options);
}

}  // namespace grparse
