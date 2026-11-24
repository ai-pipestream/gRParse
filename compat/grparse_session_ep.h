#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// kTensorRt and kLevelZero are never selections.  kTensorRt is where a model
// actually runs when the selection is kCuda in a build that carries native
// TensorRT (native_tensorrt_build); kLevelZero is where the table structure
// decoder runs under a kOpenVino GPU selection (src/levelzero/).  Both
// appear only as a placement's `actual`.
enum class OrtEp { kCpu, kCuda, kOpenVino, kTensorRt, kLevelZero };

// Numeric precision a session asks its provider for.  kProviderDefault leaves
// the provider's own choice alone, which for the OpenVINO GPU plugin is half
// precision.  That is fine for the OCR nets and wrong for the layout
// detector: at FP16 it drops real detections and its boxes drift by tens of
// pixels, while at FP32 its GPU output is bit-identical to CPU.
enum class OrtPrecision { kProviderDefault, kFloat32 };

struct OrtEpSelection {
  OrtEp ep = OrtEp::kCpu;
  int cuda_device = 0;
  // OpenVINO device_type: GPU, GPU.<n>, NPU, NPU.<n> or CPU; never a device
  // list (server_config.cpp rejects AUTO:/HETERO:/MULTI:/BATCH:).
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

// How many line crops the patched angle and recognition nets run per call
// (patches/rapidocr-batched-nets.patch).  GRPARSE_OCR_BATCH sets it, 1 to
// 256; the default is 16 under CUDA and 1 otherwise.  On the CPU provider
// the padding costs more than the batch saves (0.44 pages per second at 16
// against 0.74 at 1 on a 32-core host), and the OpenVINO GPU plugin
// compiles kernels for each new input shape, which a batch dimension
// multiplies.  1 reads one crop per call, the upstream behavior.  A
// malformed value throws std::invalid_argument.
size_t ocr_batch_size();

// How many crops the direction net classifies per call
// (patches/rapidocr-direction-batch.patch).  Every crop is resized to the
// same 192 x 48, so a batch needs no padding and classifies each crop as a
// single call would.  GRPARSE_OCR_BATCH sets it when present; otherwise 256
// on the Level Zero runtime, which takes any batch without compiling for
// it and pays its launch latency once per call, and ocr_batch_size()
// elsewhere.
size_t ocr_direction_batch_size();

// True when this build runs the cuda flavor's models on native TensorRT
// (src/tensorrt/) instead of ONNX Runtime's CUDA provider.  In such a build
// a CUDA selection never creates an ONNX Runtime session for an image model.
bool native_tensorrt_build();

// GRPARSE_CUDA_TF32: "off" (the default) computes FP32 convolutions and
// matrix products exactly; "on" allows TF32.  Applies to ONNX Runtime's CUDA
// provider and to native TensorRT alike.  Any other value throws
// std::invalid_argument.
bool cuda_tf32_enabled();

// Fixed input shapes for the OCR nets (patches/rapidocr-shape-buckets.patch,
// src/ocr_shape_buckets.cpp).  The OpenVINO GPU plugin compiles the network
// for every new input shape; with dynamic page and line sizes one 101-paper
// run left 16,822 compiled blobs and made OCR 11x slower than CUDA.  When on,
// the detector input is padded with white to a bucketed size and its map is
// cropped back, and a line is padded to a bucketed width whose extra time
// steps are not decoded.  GRPARSE_OCR_SHAPE_BUCKETS=auto (default: the
// detector only, for the OpenVINO provider on a GPU device), on, det, rec or
// off.
bool ocr_shape_buckets();
// The smallest detector bucket (a multiple of GRPARSE_OCR_DET_STEP, default
// 64) at least `side` pixels.
int ocr_det_bucket(int side);
// The detector's longest side: `configured` on every backend of this build.
int ocr_det_max_side(int configured);
// The detector input shape (width, height) a `width` x `height` resize is
// padded to: unchanged without buckets, each side bucketed on its own
// otherwise.
std::pair<int, int> ocr_det_bucket_shape(int width, int height);
// The smallest recognizer width bucket at least `width` pixels.
int ocr_rec_bucket(int width);
// How bucket padding is filled: true repeats the edge pixels
// (GRPARSE_OCR_BUCKET_PAD=replicate, the default), false pads the detector
// with white and the recognizer with zeros (=zero).
bool ocr_pad_replicate();

// Must be called before any OCR engine is constructed.  Later sessions use
// the newest selection; sessions already built keep the provider they bound.
void set_ort_ep_selection(OrtEpSelection selection);
OrtEpSelection ort_ep_selection();

// How many sessions have passed through the hook.  Zero after building an
// engine means the RapidOcrOnnx patch was not applied (for example a stale
// dependency cache) and the process must not pretend the configured provider
// is active.
uint64_t ep_hook_invocations();

// How many session builds the selected execution provider refused (after the
// OpenVINO retries ran out), plus first inferences that failed on the
// provider.  Placement is strict: each one of these failed the build and
// threw; none was rebuilt on CPU.  Exported as grparse_ort_ep_fallbacks_total
// (the name predates strict placement); a healthy GPU deployment reads 0.
uint64_t ep_fallback_count();

// Counts one refusal, logs it loudly, and returns the exception a builder
// throws: "<what>: the <ep> execution provider (<device>) refused <model>
// (<detail>); GPU placement is strict and this model will not run on CPU".
// For callers whose provider failure surfaces after the session exists (the
// layout engine's first inference).
std::runtime_error ep_refusal(std::string_view what, const std::filesystem::path& model_path,
                              std::string_view detail);

// The same for a model native TensorRT refused (an engine that does not
// build, a failed first inference): "<what>: TensorRT (CUDA device <n>)
// refused <model> (<detail>); GPU placement is strict and this model will
// not run on ONNX Runtime or on CPU".  Also what make_session and
// make_rapidocr_session throw under a CUDA selection in a build that
// carries native TensorRT, where no ONNX Runtime session may stand in for
// an engine.
std::runtime_error tensorrt_refusal(std::string_view what, const std::filesystem::path& model_path,
                                    std::string_view detail);

// Where one model's sessions actually run, as the builders recorded them.
// `requested` is the process selection when the session was built, `actual`
// the provider the session was built for.  The two differ for a model that
// pins itself to CPU in code (make_pinned_cpu_session), which carries its
// reason, and for a model native TensorRT runs under a CUDA selection
// (actual kTensorRt); there is no other way for them to differ.  `device` is
// the OpenVINO device type or the CUDA device index; empty on CPU.
struct OrtPlacement {
  std::string model;
  OrtEp requested = OrtEp::kCpu;
  OrtEp actual = OrtEp::kCpu;
  std::string device;
  std::string pin_reason;
  uint64_t sessions = 0;
};

// Records one model built on the selection's native backend (TensorRT under
// a CUDA selection, src/inference_model.cpp) in the same placements as the
// ONNX Runtime sessions, so grparse_ort_sessions shows every model.
void record_native_placement(std::string_view what);

// Records one model run by the native Level Zero decoder on the selection's
// OpenVINO GPU device: actual kLevelZero, device the OpenVINO device name.
void record_level_zero_placement(std::string_view what);

// Every placement recorded so far, one entry per (model, requested, actual,
// device), in first-built order.  Deterministic for a fixed build sequence.
std::vector<OrtPlacement> ort_placements();

// Forgets every recorded placement.  The server calls it before each
// GRPARSE_ORT_EP=auto attempt, so a provider that built some OCR nets and
// then refused one leaves no entries behind; tests call it between cases.
void reset_ort_placements();

// The session config key that makes ONNX Runtime refuse to place any node
// on its own CPU provider when another provider was appended.  Without it,
// ops the OpenVINO provider does not claim run on CPU silently and the GPU
// does a fraction of the work.  Set on every OpenVINO session.
inline constexpr const char* kDisableCpuEpFallbackKey = "session.disable_cpu_ep_fallback";

// Sets kDisableCpuEpFallbackKey on options for a session that will run on
// `ep` when that provider is OpenVINO; leaves CPU and CUDA options alone (the
// CUDA provider leaves shape ops on CPU by design).  append_execution_provider
// calls it before appending the provider.
void apply_strict_placement(Ort::SessionOptions& options, OrtEp ep);

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
// Placement is strict: a provider that refuses the graph - an unsupported
// operator, a node it will not claim (OpenVINO sessions set
// kDisableCpuEpFallbackKey, so ONNX Runtime cannot quietly run such a node on
// its CPU provider), a device that will not initialize, the OpenVINO
// toolchain's JIT compiler failing - fails the build with ep_refusal's
// exception.  Nothing is rebuilt on CPU: a GPU deployment whose model cannot
// run on the GPU must not come up looking healthy and run at CPU speed.  An
// OpenVINO build first retries a small bounded number of times with short
// jittered backoff (the JIT failures are intermittent and usually pass on a
// later attempt, still on the GPU).  CPU selections build once, with no
// retries: a CPU failure is deterministic.  Every successful build records
// its placement (ort_placements).
//
// A model that must run on CPU under a GPU selection says so in code with
// make_pinned_cpu_session; that is the only way to get a CPU session here.
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

// A plain CPU session with the process threading.  Records no placement;
// the builders above record theirs.  Under a GPU selection use
// make_pinned_cpu_session, which says why.
Ort::Session make_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                              int intra_op_threads = kIntraOpProcessDefault);

// A model that cannot run on the selected GPU provider and is placed on CPU
// on purpose.  `reason` is mandatory and lands in the startup log and the
// grparse_ort_sessions metric (pin label), so the CPU placement is visible,
// counted per session, and reviewable in the diff that added it.  Not a
// fallback: no GPU build is attempted.
Ort::Session make_pinned_cpu_session(Ort::Env& env, const std::filesystem::path& model_path,
                                     std::string_view what, std::string_view reason,
                                     int intra_op_threads = kIntraOpProcessDefault);


}  // namespace grparse
