#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

using grparse_test::require;

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

}  // namespace

int main() {
  return grparse_test::run_test_main("ort-session-ep-test", "all checks passed", {
      verify_default_selection_is_cpu,
      verify_legacy_negative_index_appends_nothing_but_counts,
      verify_selection_round_trips_and_copies,
      verify_explicit_cpu_overrides_legacy_gpu_index,
      verify_latest_selection_wins,
  });
}
