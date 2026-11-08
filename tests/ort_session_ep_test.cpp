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

void verify_legacy_cuda_forbids_cpu_nodes() {
  Ort::SessionOptions options;
  try {
    grparse::append_execution_provider(options, 999999);
  } catch (const std::exception&) {
    // The legacy caller still selected CUDA even when this package lacks it.
  }
  require(options.HasConfigEntry("session.disable_cpu_ep_fallback") &&
              options.GetConfigEntry("session.disable_cpu_ep_fallback") == "1",
          "legacy CUDA selection cannot silently place graph nodes on CPU");
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

// With GRPARSE_OCR_BATCH unset, line crops go 16 to a call under CUDA and
// one at a time under CPU and OpenVINO.
void verify_ocr_batch_size_follows_provider() {
  require(std::getenv("GRPARSE_OCR_BATCH") == nullptr, "the test runs without GRPARSE_OCR_BATCH");
  const grparse::OrtEpSelection kept = grparse::ort_ep_selection();
  require(kept.ep == grparse::OrtEp::kCpu, "the test starts on the CPU provider");
  require_equal(grparse::ocr_batch_size(), size_t{1}, "CPU reads one crop per call");
  grparse::OrtEpSelection other = kept;
  other.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(other);
  require_equal(grparse::ocr_batch_size(), size_t{1}, "OpenVINO reads one crop per call");
  other.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(other);
  require_equal(grparse::ocr_batch_size(), size_t{16}, "CUDA reads 16 crops per call");
  grparse::set_ort_ep_selection(kept);
}

// A minimal ONNX model that can load on CPU, so silent fallback would succeed: ir_version 8, opset
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

void verify_make_session_rejects_cuda_failure() {
  const std::filesystem::path model = write_tiny_model();
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCuda;
  selection.cuda_device = 999999;  // invalid even on a GPU-enabled test host
  grparse::set_ort_ep_selection(selection);
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  bool threw = false;
  try {
    (void)grparse::make_session(env, model, "tiny test model");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a refused CUDA device must not return a working CPU session");
  require_equal(grparse::ep_fallback_count(), before, "no CPU fallback took place");
  require_equal(grparse::ep_build_retry_count(), retries_before,
                "a refused CUDA build is not retried");
  std::filesystem::remove(model);
}

void verify_make_session_retries_on_openvino_then_throws() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  selection.openvino_device = "GPU.999999";
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = write_tiny_model();
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  bool threw = false;
  try {
    (void)grparse::make_session(env, model, "table structure");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a refused OpenVINO device must not rebuild the table model on CPU");
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "a refused OpenVINO build retries twice before failing");
  require_equal(grparse::ep_fallback_count(), before, "no CPU fallback took place");
  std::filesystem::remove(model);
}

void verify_accelerator_options_forbid_cpu_nodes() {
  for (const auto& selection : {
           grparse::OrtEpSelection{.ep = grparse::OrtEp::kCuda},
           grparse::OrtEpSelection{.ep = grparse::OrtEp::kOpenVino},
           grparse::OrtEpSelection{.ep = grparse::OrtEp::kOpenVino, .openvino_device = "NPU"}}) {
    grparse::set_ort_ep_selection(selection);
    Ort::SessionOptions options;
    try {
      grparse::append_execution_provider(options, -1);
    } catch (const std::exception&) {
      // A provider may be absent from this build; its CPU-placement guard
      // must already be present before trying to initialize it.
    }
    require(options.HasConfigEntry("session.disable_cpu_ep_fallback"),
            "accelerator options prohibit ORT's implicit CPU node placement");
    require(options.GetConfigEntry("session.disable_cpu_ep_fallback") == "1",
            "CPU node placement is disabled, not only reported");
  }
  grparse::OrtEpSelection cpu;
  grparse::set_ort_ep_selection(cpu);
  Ort::SessionOptions options;
  grparse::append_execution_provider(options, -1);
  require(!options.HasConfigEntry("session.disable_cpu_ep_fallback"),
          "intentional CPU operation remains available");

  grparse::OrtEpSelection ov_cpu;
  ov_cpu.ep = grparse::OrtEp::kOpenVino;
  ov_cpu.openvino_device = "CPU";
  grparse::set_ort_ep_selection(ov_cpu);
  Ort::SessionOptions cpu_device;
  try { grparse::append_execution_provider(cpu_device, -1); }
  catch (const std::exception&) { /* OpenVINO is absent in CPU-only builds. */ }
  require(!cpu_device.HasConfigEntry("session.disable_cpu_ep_fallback"),
          "an explicitly selected OpenVINO CPU device remains intentional CPU operation");

  grparse::OrtEpSelection mixed;
  mixed.ep = grparse::OrtEp::kOpenVino;
  mixed.openvino_device = "AUTO:GPU,CPU";
  grparse::set_ort_ep_selection(mixed);
  bool refused = false;
  try {
    Ort::SessionOptions automatic;
    grparse::append_execution_provider(automatic, -1);
  } catch (const std::invalid_argument& error) {
    refused = std::string(error.what()).contains("implicit CPU fallback");
  }
  require(refused, "OpenVINO AUTO must not conceal CPU fallback inside the provider");
  grparse::set_ort_ep_selection(cpu);
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
  selection.openvino_device = "GPU.999999";
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
      verify_legacy_cuda_forbids_cpu_nodes,
      verify_selection_round_trips_and_copies,
      verify_explicit_cpu_overrides_legacy_gpu_index,
      verify_latest_selection_wins,
      verify_ocr_batch_size_follows_provider,
      verify_make_session_rejects_cuda_failure,
      verify_cpu_selection_failure_does_not_count,
      verify_make_session_retries_on_openvino_then_throws,
      verify_accelerator_options_forbid_cpu_nodes,
      verify_make_session_retries_then_succeeds,
      verify_injected_failures_exhaust_and_throw_on_cpu,
      verify_make_rapidocr_session_retries_then_succeeds,
      verify_make_rapidocr_session_fails_loudly_on_openvino,
  });
}
