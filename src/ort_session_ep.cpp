#include "grparse_session_ep.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::mutex placement_mutex;
std::vector<OrtPlacement> placements;

OrtEpSelection selection_copy() {
  std::lock_guard<std::mutex> lock(selection_mutex);
  return current_selection;
}

std::string device_label(OrtEp ep, const OrtEpSelection& selection) {
  switch (ep) {
    case OrtEp::kCuda: return std::to_string(selection.cuda_device);
    case OrtEp::kOpenVino: return selection.openvino_device;
    case OrtEp::kCpu: break;
  }
  return {};
}

// One more session of `what` placed on `actual`; same key, same entry.
void record_placement(std::string_view what, OrtEp actual, std::string_view pin_reason) {
  const OrtEpSelection selection = selection_copy();
  OrtPlacement entry{
      .model = std::string(what),
      .requested = selection.ep,
      .actual = actual,
      .device = device_label(actual, selection),
      .pin_reason = std::string(pin_reason),
  };
  std::lock_guard<std::mutex> lock(placement_mutex);
  for (OrtPlacement& existing : placements) {
    if (existing.model == entry.model && existing.requested == entry.requested &&
        existing.actual == entry.actual && existing.device == entry.device &&
        existing.pin_reason == entry.pin_reason) {
      ++existing.sessions;
      return;
    }
  }
  entry.sessions = 1;
  placements.push_back(std::move(entry));
}

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

// GRPARSE_CUDA_TF32: "off" (the default) computes convolutions and matmuls in
// full FP32; "on" allows TF32, the ONNX Runtime default on Ampere and newer.
// On 101 ACM papers TF32 moved layout detections near the threshold on an
// RTX 4080 (3 papers with one picture more or less than the CPU and B70
// runs); FP32 removed those differences for +18% total time.  Any other value
// throws.
bool cuda_tf32_setting() {
  const char* value = std::getenv("GRPARSE_CUDA_TF32");
  if (value == nullptr || *value == '\0') return false;
  const std::string mode = value;
  if (mode == "on") return true;
  if (mode == "off") return false;
  throw std::invalid_argument("GRPARSE_CUDA_TF32 must be on or off");
}

void append_cuda(Ort::SessionOptions& options, int device) {
  // The settings upstream RapidOcrOnnx used (the 2 GiB arena limit computed in
  // 64 bits, since the upstream int expression wrapped), set through the V2
  // struct, which has the use_tf32 key.
  Ort::CUDAProviderOptions cuda_options;
  cuda_options.Update({{"device_id", std::to_string(device)},
                       {"arena_extend_strategy", "kNextPowerOfTwo"},
                       {"gpu_mem_limit", std::to_string(2ULL * 1024ULL * 1024ULL * 1024ULL)},
                       {"cudnn_conv_algo_search", "EXHAUSTIVE"},
                       {"do_copy_in_default_stream", "1"},
                       {"use_tf32", cuda_tf32_setting() ? "1" : "0"}});
  options.AppendExecutionProvider_CUDA_V2(*cuda_options);
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

bool native_openvino_build() {
#ifdef GRPARSE_NATIVE_OPENVINO
  return true;
#else
  return false;
#endif
}

size_t ocr_batch_size() {
  static const size_t configured = [] {
    const char* value = std::getenv("GRPARSE_OCR_BATCH");
    if (value == nullptr || *value == '\0') return size_t{0};
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (*end != '\0' || parsed < 1 || parsed > 256) {
      throw std::invalid_argument("GRPARSE_OCR_BATCH must be an integer between 1 and 256");
    }
    return static_cast<size_t>(parsed);
  }();
  if (configured != 0) return configured;
  const OrtEp ep = ort_ep_selection().ep;
  if (ep == OrtEp::kCuda) return 16;
  // Native OpenVINO runs fixed batches of 8 (ocr_net_shapes).
  return ep == OrtEp::kOpenVino && native_openvino_build() ? 8 : 1;
}

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
      // Every node runs on the OpenVINO device or the session does not
      // build: without this, nodes the provider does not claim run on ONNX
      // Runtime's CPU provider with no error and no log line.
      apply_strict_placement(options, OrtEp::kOpenVino);
      options.AppendExecutionProvider_OpenVINO_V2(openvino_options);
      return;
    }
    case OrtEp::kCpu:
      return;
  }
}

std::runtime_error ep_refusal(std::string_view what, const std::filesystem::path& model_path,
                              std::string_view detail) {
  const OrtEpSelection selection = selection_copy();
  ep_fallbacks.fetch_add(1);
  std::string message = std::format(
      "gRParse {}: the {} execution provider ({}) refused {} ({}); GPU placement is strict "
      "and this model will not run on CPU",
      what, ort_ep_name(selection.ep), device_label(selection.ep, selection),
      model_path.string(), detail);
  std::println(stderr, "{}", message);
  return std::runtime_error(message);
}

void apply_strict_placement(Ort::SessionOptions& options, OrtEp ep) {
  if (ep == OrtEp::kOpenVino) options.AddConfigEntry(kDisableCpuEpFallbackKey, "1");
}

std::vector<OrtPlacement> ort_placements() {
  std::lock_guard<std::mutex> lock(placement_mutex);
  return placements;
}

void reset_ort_placements() {
  std::lock_guard<std::mutex> lock(placement_mutex);
  placements.clear();
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
    Ort::Session session = make_cpu_session(env, model_path, intra_op_threads);
    record_placement(what, OrtEp::kCpu, {});
    return session;
  }
  Ort::Session session{nullptr};
  try {
    session = build_with_ep_retries(what, ep, [&]() -> Ort::Session {
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
    // Strict placement: no CPU rebuild.  The model either runs on the
    // provider the process selected or the build fails, loudly and counted.
    throw ep_refusal(what, model_path, error.what());
  }
  record_placement(what, ep, {});
  return session;
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
  if (ep == OrtEp::kCpu && !build_failure_injection_pending()) {
    Ort::Session* session = cpu_build();
    record_placement(what, OrtEp::kCpu, {});
    return session;
  }
  // After the retries exhaust, the refusal propagates out of the net's
  // initModel and takes the engine down with a clear error.  No OCR model
  // ever degrades to CPU.
  Ort::Session* session = nullptr;
  try {
    session = build_with_ep_retries(what, ep, [&]() -> Ort::Session* {
      // Same gate discipline as make_session: one attempt under the gate at
      // a time, backoff sleeps outside it.
      const OvCompileGate compile_gate;
      if (ep == OrtEp::kCpu) {
        // Only reachable under test failure injection.
        throw_if_build_failure_injected();
        return cpu_build();
      }
      // The same options the patched nets used to configure themselves: the
      // central provider hook with the net's own thread count, exactly what
      // cpu_build() honours, so a provider never changes the threading.
      Ort::SessionOptions options;
      options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
      append_execution_provider(options, -1, OrtPrecision::kProviderDefault, num_thread);
      throw_if_build_failure_injected();
      return new Ort::Session(env, model_path.c_str(), options);
    });
  } catch (const std::exception& error) {
    if (ep == OrtEp::kCpu) throw;
    throw ep_refusal(what, model_path, error.what());
  }
  record_placement(what, ep, {});
  return session;
}

Ort::Session make_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                              int intra_op_threads) {
  Ort::SessionOptions cpu_options;
  configure_cpu_options(cpu_options, intra_op_threads);
  return Ort::Session(env, model_path.c_str(), cpu_options);
}

Ort::Session make_pinned_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                                     std::string_view what, std::string_view reason,
                                     int intra_op_threads) {
  if (reason.empty()) {
    throw std::invalid_argument(std::format("gRParse {}: a CPU pin needs a reason", what));
  }
  Ort::Session session = make_cpu_session(env, model_path, intra_op_threads);
  const OrtEp requested = ort_ep_selection().ep;
  if (requested != OrtEp::kCpu) {
    std::println(stderr, "gRParse {}: pinned to CPU under the {} execution provider ({})", what,
                 ort_ep_name(requested), reason);
  }
  record_placement(what, OrtEp::kCpu, requested == OrtEp::kCpu ? std::string_view{} : reason);
  return session;
}

}  // namespace grparse
