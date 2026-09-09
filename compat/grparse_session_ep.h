#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

// Execution-provider selection for every ONNX Runtime session this process
// creates.  RapidOcrOnnx's det/cls/rec nets each own a private
// Ort::SessionOptions; patches/rapidocr-session-ep.patch rewires their
// setGpuIndex bodies to call append_execution_provider, so CUDA, OpenVINO,
// and CPU all flow through this one decision point.
//
// This header lives in compat/ because that directory is already on the
// RapidOcrOnnx include path.

namespace grparse {

enum class OrtEp { kCpu, kCuda, kOpenVino };

// Numeric precision a session asks its provider for.  kProviderDefault leaves
// the provider's own choice alone, which for the OpenVINO GPU plugin is half
// precision.  That is fine for the OCR nets and wrong for the layout
// detector: at FP16 it drops real detections and its boxes drift by tens of
// pixels, while at FP32 its GPU output is bit-identical to CPU.
enum class OrtPrecision { kProviderDefault, kFloat32 };

struct OrtEpSelection {
  OrtEp ep = OrtEp::kCpu;
  int cuda_device = 0;
  // OpenVINO device_type: GPU, GPU.<n>, CPU, NPU, or an AUTO:/HETERO: list.
  std::string openvino_device = "GPU";
  // Directory the OpenVINO plugin may keep compiled blobs in, so a session
  // create reuses the previous compile instead of repeating it.  Empty
  // disables the cache.
  std::string openvino_cache_dir = {};
};

// Human-readable name of a provider, for logs.
std::string_view ort_ep_name(OrtEp ep);

// How many threads one session may use inside a single operator.
//
// ONNX Runtime's default is every core, which is right for one session and
// wrong for a pool of them: N pooled sessions each claiming every core is N
// times oversubscribed, and on small machines that is measurably slower than
// a single worker.  The process-wide value is set once from the worker count;
// a session that is not pooled asks for kIntraOpAllCores instead.
inline constexpr int kIntraOpProcessDefault = -1;
inline constexpr int kIntraOpAllCores = 0;

// Sets the value pooled sessions take when they ask for the process default.
// Call before building any engine; 0 leaves ONNX Runtime's own default alone.
void set_ort_intra_op_threads(int threads);
int ort_intra_op_threads();

// Must be called before any OCR engine is constructed.  Later sessions use
// the newest selection; sessions already built keep the provider they bound.
void set_ort_ep_selection(OrtEpSelection selection);
OrtEpSelection ort_ep_selection();

// How many sessions have passed through the hook.  Zero after building an
// engine means the RapidOcrOnnx patch was not applied (for example a stale
// dependency cache) and the process must not pretend the configured provider
// is active.
uint64_t ep_hook_invocations();

// How many sessions make_session has rebuilt on CPU after the selected
// execution provider refused the graph or the device failed.  A monitoring
// surface: every increment is also a stderr line, and a rising count on a
// deployment that was promised GPU acceleration is the earliest signal that
// the promise broke.
uint64_t ep_fallback_count();

// How many session builds were retried after a transient OpenVINO
// execution-provider failure before either succeeding or giving up: the
// cost of the policy that keeps intermittent JIT compiler failures from
// killing the process.
uint64_t ep_build_retry_count();

// Test-only: makes the next `failures` session builds throw a synthetic error
// from inside the build, standing in for the OpenVINO toolchain's
// intermittent IGC failures so the retry paths are testable without a GPU.
// Only tests call this; production builds never inject.
void ort_ep_test_inject_build_failures(int failures);

// Called by the patched RapidOcrOnnx nets.  When no explicit selection was
// made, legacy_gpu_index keeps upstream semantics: >= 0 appends CUDA for that
// device, negative appends nothing (CPU).
//
// This two-argument form is a real overload rather than defaulted parameters
// on the one below, and must stay that way: it is the exact signature
// patches/rapidocr-session-ep.patch compiles against, and giving it default
// arguments instead would rename the symbol every time this file grows a
// knob - which links fine from a clean tree and fails only against a warm
// dependency cache, at the worst possible moment.
void append_execution_provider(Ort::SessionOptions& options, int legacy_gpu_index);
void append_execution_provider(Ort::SessionOptions& options, int legacy_gpu_index,
                               OrtPrecision precision, int intra_op_threads);

// Process-wide serialization for execution-provider work that compiles GPU
// kernels.  The OpenVINO toolchain the Intel image ships (IGC through NEO)
// is not safe under concurrent JIT compiles from a cold kernel cache: the
// colliding compiles escape as heap corruption or an abort and take the whole
// process down.  Two kinds of work therefore take this gate while the
// OpenVINO provider is selected:
//   - Ort::Session creation, because the OpenVINO provider compiles the
//     model inside the Session constructor, and
//   - each engine's first inference, because per-shape kernels compile on
//     demand and every pooled worker can hit its cold cache at once.
// A cold cache costs startup latency this way, never a crash; once the cache
// is warm the gate is never contended, so steady-state throughput is
// unchanged.  For any other provider (or no selection yet) the gate is a
// no-op, and single-threaded behavior is untouched everywhere.
class OvCompileGate final {
 public:
  OvCompileGate();
  ~OvCompileGate();
  OvCompileGate(const OvCompileGate&) = delete;
  OvCompileGate& operator=(const OvCompileGate&) = delete;

 private:
  std::unique_lock<std::mutex> lock_;
};

// Builds one session for a model file on the configured provider.
//
// A provider that refuses the graph - an unsupported operator, a device that
// will not initialize, an export the plugin rejects outright, the OpenVINO
// toolchain's JIT compiler failing intermittently - costs that model its
// acceleration, not the whole server: an OpenVINO build retries a small
// bounded number of times with short jittered backoff (the failures are
// intermittent and usually pass on a later attempt, still on the GPU), then
// the session is rebuilt on CPU with the error logged in full.  Other
// providers keep their single-attempt behavior.  `what` names the model in
// that message.  A model file that does not parse at all still throws, on
// every attempt.  CPU selections build once, with no retries: a CPU failure
// is deterministic.
//
// This fallback is pre-existing policy and applies to the models that build
// through make_session (table structure, layout, figure classification).  The
// OCR nets are deliberately different: they build through
// make_rapidocr_session, which never retreats to CPU.
//
// The OpenVINO toolchain's other failure class - heap corruption or a
// longjmp across the stack inside the compile - kills the process outright
// and is out of scope here; no in-process policy can recover from it.
Ort::Session make_session(Ort::Env& env, const std::filesystem::path& model_path,
                          std::string_view what,
                          OrtPrecision precision = OrtPrecision::kProviderDefault,
                          int intra_op_threads = kIntraOpProcessDefault);

// Builds one Ort::Session for the patched RapidOcrOnnx nets.  On the
// OpenVINO provider the build retries a bounded number of times with short
// jittered backoff, exactly like make_session; unlike make_session it never
// retreats to CPU: after the retries exhaust the exception propagates out of
// the net's initModel and fails the engine loudly.  The openvino flavor must
// never silently degrade an OCR model to CPU.  CPU selections build once and
// throw on broken models.  `num_thread` is the net's own thread count (its
// setNumThread value), `what` names the net in log lines.  Returns a heap
// session the net owns; when this throws, the net's session pointer stays
// null and its destructor is safe.
Ort::Session* make_rapidocr_session(Ort::Env& env, const std::filesystem::path& model_path,
                                    std::string_view what, int num_thread);

// The CPU-only session make_session falls back to, exposed for callers that
// must retreat AFTER creation: some provider failures only surface at the
// first inference (a runtime kernel compile), which make_session cannot see.
Ort::Session make_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                              int intra_op_threads = kIntraOpProcessDefault);


}  // namespace grparse
