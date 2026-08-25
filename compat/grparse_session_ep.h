#pragma once

#include <cstdint>
#include <filesystem>
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

// Builds one session for a model file on the configured provider.
//
// A provider that refuses the graph - an unsupported operator, a device that
// will not initialize, an export the plugin rejects outright - costs that
// model its acceleration, not the whole server: the session is rebuilt on CPU
// and the reason is logged in full.  `what` names the model in that message.
// A model file that does not parse at all still throws, on both attempts.
Ort::Session make_session(Ort::Env& env, const std::filesystem::path& model_path,
                          std::string_view what,
                          OrtPrecision precision = OrtPrecision::kProviderDefault,
                          int intra_op_threads = kIntraOpProcessDefault);

// The CPU-only session make_session falls back to, exposed for callers that
// must retreat AFTER creation: some provider failures only surface at the
// first inference (a runtime kernel compile), which make_session cannot see.
Ort::Session make_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                              int intra_op_threads = kIntraOpProcessDefault);


}  // namespace grparse
