#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

using grparse_test::require;
using grparse_test::require_equal;

// The selection state and the hook counter are process-global and the
// explicit-selection latch never resets, so the checks run in one fixed
// order: default state first, then the legacy (never-selected) path, then
// explicit selections.

void verify_default_selection_is_cpu() {
  grparse::OrtEpSelection selection = grparse::ort_ep_selection();
  require(selection.ep == grparse::OrtEp::kCpu,
          "the default execution provider is CPU");
  require(selection.cuda_device == 0,
          "the default CUDA device index is zero");
  require(selection.openvino_device == "GPU",
          "the default OpenVINO device type is GPU");
  require(grparse::ep_hook_invocations() == 0,
          "no session has passed through the hook yet");
}

void verify_legacy_negative_index_appends_nothing_but_counts() {
  // Upstream semantics before any explicit selection: a negative GPU index
  // means CPU. The options must stay usable and the hook must still count,
  // because a zero count is how the server detects an unpatched dependency.
  Ort::SessionOptions options;
  grparse::append_execution_provider(options, -1);
  require(grparse::ep_hook_invocations() == 1,
          "the legacy CPU path counts one hook invocation");
  grparse::append_execution_provider(options, -1);
  require(grparse::ep_hook_invocations() == 2,
          "every call increments the invocation count by one");
}

void verify_selection_round_trips_and_copies() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  selection.cuda_device = 3;
  selection.openvino_device = "NPU";
  grparse::set_ort_ep_selection(selection);
  // Mutating the caller's struct after the set must not leak through.
  selection.cuda_device = 9;
  selection.openvino_device = "CPU";
  grparse::OrtEpSelection stored = grparse::ort_ep_selection();
  require(stored.ep == grparse::OrtEp::kCpu,
          "the stored selection keeps its provider");
  require(stored.cuda_device == 3,
          "the stored selection keeps its CUDA device index");
  require(stored.openvino_device == "NPU",
          "the stored selection keeps its OpenVINO device type");
}

void verify_explicit_cpu_overrides_legacy_gpu_index() {
  // An explicit CPU selection wins over a legacy GPU index: nothing is
  // appended (appending CUDA here would throw on a CPU-only ONNX Runtime),
  // and the hook still counts the session.
  uint64_t before = grparse::ep_hook_invocations();
  Ort::SessionOptions options;
  grparse::append_execution_provider(options, 5);
  require(grparse::ep_hook_invocations() == before + 1,
          "the explicit CPU path counts its hook invocation");
}

void verify_latest_selection_wins() {
  grparse::OrtEpSelection replacement;
  replacement.ep = grparse::OrtEp::kCpu;
  replacement.cuda_device = 1;
  replacement.openvino_device = "GPU.1";
  grparse::set_ort_ep_selection(replacement);
  grparse::OrtEpSelection stored = grparse::ort_ep_selection();
  require(stored.cuda_device == 1 && stored.openvino_device == "GPU.1",
          "a later selection replaces the earlier one");
  uint64_t before = grparse::ep_hook_invocations();
  Ort::SessionOptions options;
  grparse::append_execution_provider(options, -1);
  require(grparse::ep_hook_invocations() == before + 1,
          "the replaced selection still routes through the counting hook");
}

// The minimal ONNX model a fallback can rebuild on CPU: ir_version 8, opset
// 13, one Identity from input "x" to output "y", hand-encoded protobuf so the
// fixture carries no generator dependency.
constexpr unsigned char kTinyModelBytes[] = {
    0x08, 0x08,                                           // ir_version = 8
    0x42, 0x02, 0x10, 0x0D,                               // opset_import: version 13
    0x3A, 0x37,                                           // graph, 55 bytes
    0x12, 0x01, 0x67,                                     // name = "g"
    0x0A, 0x10,                                           // node
    0x0A, 0x01, 0x78,                                     //   input "x"
    0x12, 0x01, 0x79,                                     //   output "y"
    0x22, 0x08, 0x49, 0x64, 0x65, 0x6E, 0x74, 0x69, 0x74, 0x79,  // op_type Identity
    0x5A, 0x0F, 0x0A, 0x01, 0x78,                         // input "x"
    0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x04, 0x0A, 0x02, 0x08, 0x01,                   //   shape [1]
    0x62, 0x0F, 0x0A, 0x01, 0x79,                         // output "y"
    0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x04, 0x0A, 0x02, 0x08, 0x01,                   //   shape [1]
};

std::filesystem::path write_tiny_model() {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("grparse-tiny-{}.onnx", ::getpid());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kTinyModelBytes), sizeof(kTinyModelBytes));
  out.close();
  require(out.good(), "the tiny model fixture is written");
  return path;
}

void verify_make_session_falls_back_and_counts() {
  const std::filesystem::path model = write_tiny_model();
  // CUDA is never available in the test binaries (CPU and OpenVINO ONNX
  // Runtime packages reject the provider outright; CI has no GPU for the
  // CUDA package), so a CUDA selection always drives make_session down its
  // catch block: the fallback counter must move and the CPU rebuild must
  // load the model.  Retries are scoped to OpenVINO, so CUDA must not
  // retry: one attempt, one fallback.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(selection);
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  Ort::Session session = grparse::make_session(env, model, "tiny test model");
  require_equal(grparse::ep_fallback_count(), before + 1,
                "a refused provider counts one fallback");
  require_equal(grparse::ep_build_retry_count(), retries_before,
                "a refused CUDA build is not retried");
  require(session.GetInputCount() == 1 && session.GetOutputCount() == 1,
          "the CPU fallback session loads the tiny model");
  std::filesystem::remove(model);
}

void verify_make_session_retries_on_openvino_then_falls_back() {
  // The OpenVINO provider is unusable in the test binaries too (absent from
  // the CPU package, present but device-less in the OpenVINO package), but
  // it is the retryable provider: the build must be attempted three times
  // before the pre-existing CPU fallback fires.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  Ort::Session session = grparse::make_session(env, model, "tiny test model");
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "a refused OpenVINO build retries twice before falling back");
  require_equal(grparse::ep_fallback_count(), before + 1,
                "the pre-existing fallback still counts once");
  require(session.GetInputCount() == 1 && session.GetOutputCount() == 1,
          "the CPU fallback session loads the tiny model");
  std::filesystem::remove(model);
}

void verify_make_session_retries_then_succeeds() {
  // The injection hook makes the next N builds throw from inside the build,
  // standing in for the OpenVINO toolchain's intermittent failures.  With a
  // CPU selection (no GPU exists in the test binaries) two injected failures
  // must be retried away and the third attempt must return a working
  // session.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  grparse::ort_ep_test_inject_build_failures(2);
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  Ort::Session session = grparse::make_session(env, model, "tiny test model");
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "two transient failures count two retries");
  require(session.GetInputCount() == 1 && session.GetOutputCount() == 1,
          "the retried build loads the tiny model");
  std::filesystem::remove(model);
}

void verify_injected_failures_exhaust_and_throw_on_cpu() {
  // More injected failures than attempts: the loop gives up and the CPU
  // policy rethrows the last error.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  grparse::ort_ep_test_inject_build_failures(5);
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  bool threw = false;
  try {
    (void)grparse::make_session(env, model, "tiny test model");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "exhausted injected failures rethrow");
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "every attempt but the last counts a retry");
  std::filesystem::remove(model);
}

void verify_make_rapidocr_session_retries_then_succeeds() {
  // The patched OCR nets build through this helper; it must retry transient
  // failures the same way make_session does.  Injected failures under a CPU
  // selection exercise the loop without a GPU.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  grparse::ort_ep_test_inject_build_failures(1);
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  std::unique_ptr<Ort::Session> session =
      std::unique_ptr<Ort::Session>(grparse::make_rapidocr_session(env, model, "test net", 1));
  require_equal(grparse::ep_build_retry_count(), retries_before + 1,
                "the OCR builder retries a transient failure");
  require(session->GetInputCount() == 1 && session->GetOutputCount() == 1,
          "the retried OCR build loads the tiny model");
  std::filesystem::remove(model);
}

void verify_make_rapidocr_session_fails_loudly_on_openvino() {
  // The no-degradation contract: with the OpenVINO provider unusable, the
  // OCR builder must retry and then THROW - never return a CPU session.  A
  // returned CPU session here would be the openvino flavor silently running
  // OCR on CPU.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  bool threw = false;
  try {
    std::unique_ptr<Ort::Session> session(
        grparse::make_rapidocr_session(env, model, "test net", 1));
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "an exhausted OCR build throws instead of degrading to CPU");
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "the OCR builder retries twice before giving up");
  require_equal(grparse::ep_fallback_count(), before,
                "the OCR builder never counts a CPU fallback");
  std::filesystem::remove(model);
}

void verify_cpu_selection_failure_does_not_count() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  const uint64_t before = grparse::ep_fallback_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  bool threw = false;
  try {
    (void)grparse::make_session(env, "/nonexistent/model.onnx", "tiny test model");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing model on CPU throws instead of falling back");
  require_equal(grparse::ep_fallback_count(), before,
                "a CPU failure is not an execution-provider fallback");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ort-session-ep-test", "all checks passed", {
      verify_default_selection_is_cpu,
      verify_legacy_negative_index_appends_nothing_but_counts,
      verify_selection_round_trips_and_copies,
      verify_explicit_cpu_overrides_legacy_gpu_index,
      verify_latest_selection_wins,
      verify_make_session_falls_back_and_counts,
      verify_cpu_selection_failure_does_not_count,
      verify_make_session_retries_on_openvino_then_falls_back,
      verify_make_session_retries_then_succeeds,
      verify_injected_failures_exhaust_and_throw_on_cpu,
      verify_make_rapidocr_session_retries_then_succeeds,
      verify_make_rapidocr_session_fails_loudly_on_openvino,
  });
}
