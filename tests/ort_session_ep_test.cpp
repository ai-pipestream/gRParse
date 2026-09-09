#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
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
  // load the model.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(selection);
  const uint64_t before = grparse::ep_fallback_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  Ort::Session session = grparse::make_session(env, model, "tiny test model");
  require_equal(grparse::ep_fallback_count(), before + 1,
                "a refused provider counts one fallback");
  require(session.GetInputCount() == 1 && session.GetOutputCount() == 1,
          "the CPU fallback session loads the tiny model");
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
  });
}
