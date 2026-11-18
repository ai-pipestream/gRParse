#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "grparse_session_ep.h"
#include "support/check.h"
#include "support/tiny_onnx_model.h"

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

// Runs `build` and returns the message of the exception it must throw.
template <typename Build>
std::string refusal_message(Build&& build) {
  try {
    build();
  } catch (const std::exception& error) {
    return error.what();
  }
  require(false, "a refused provider must throw, never return a session");
  return {};
}

const grparse::OrtPlacement* find_placement(const std::vector<grparse::OrtPlacement>& placements,
                                            std::string_view model) {
  for (const auto& placement : placements) {
    if (placement.model == model) return &placement;
  }
  return nullptr;
}

void verify_make_session_refuses_and_counts() {
  const std::filesystem::path model = grparse_test::write_tiny_model();
  // CUDA is never available in the test binaries (CPU and OpenVINO ONNX
  // Runtime packages reject the provider outright; CI has no GPU for the
  // CUDA package), so a CUDA selection is a refused build every time.  Strict
  // placement: the build throws, counts one refusal, records no placement,
  // and never hands back a CPU session.  Retries are scoped to OpenVINO, so
  // CUDA makes exactly one attempt.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(selection);
  grparse::reset_ort_placements();
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  const std::string message =
      refusal_message([&] { (void)grparse::make_session(env, model, "tiny test model"); });
  require(message.contains("CUDA execution provider") && message.contains("tiny test model") &&
              message.contains("will not run on CPU"),
          "the refusal names the provider, the model and the strict policy: " + message);
  require_equal(grparse::ep_fallback_count(), before + 1, "a refused provider counts once");
  require_equal(grparse::ep_build_retry_count(), retries_before,
                "a refused CUDA build is not retried");
  require(grparse::ort_placements().empty(), "a refused build records no placement");
  std::filesystem::remove(model);
}

void verify_make_session_retries_on_openvino_then_refuses() {
  // The OpenVINO provider is unusable in the test binaries too (absent from
  // the CPU package, present but device-less in the OpenVINO package), but
  // it is the retryable provider: three attempts, then the refusal.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  grparse::reset_ort_placements();
  const std::filesystem::path model = grparse_test::write_tiny_model();
  const uint64_t before = grparse::ep_fallback_count();
  const uint64_t retries_before = grparse::ep_build_retry_count();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  const std::string message =
      refusal_message([&] { (void)grparse::make_session(env, model, "tiny test model"); });
  require(message.contains("OpenVINO execution provider (GPU)"),
          "the refusal names the OpenVINO device: " + message);
  require_equal(grparse::ep_build_retry_count(), retries_before + 2,
                "a refused OpenVINO build retries twice before refusing");
  require_equal(grparse::ep_fallback_count(), before + 1, "the refusal counts once");
  require(grparse::ort_placements().empty(), "a refused build records no placement");
  std::filesystem::remove(model);
}

void verify_strict_placement_options() {
  // The switch that stops ONNX Runtime from running unclaimed nodes on its
  // CPU provider: on for OpenVINO, off for CPU (nothing to fall back from)
  // and CUDA (whose provider keeps shape ops on CPU by design).
  Ort::SessionOptions openvino;
  grparse::apply_strict_placement(openvino, grparse::OrtEp::kOpenVino);
  require(openvino.HasConfigEntry(grparse::kDisableCpuEpFallbackKey) &&
              openvino.GetConfigEntry(grparse::kDisableCpuEpFallbackKey) == "1",
          "OpenVINO sessions refuse CPU-placed nodes");
  for (const grparse::OrtEp ep : {grparse::OrtEp::kCpu, grparse::OrtEp::kCuda}) {
    Ort::SessionOptions options;
    grparse::apply_strict_placement(options, ep);
    require(!options.HasConfigEntry(grparse::kDisableCpuEpFallbackKey),
            "only OpenVINO sessions set the CPU-node switch");
  }
  require(std::string_view(grparse::kDisableCpuEpFallbackKey) == "session.disable_cpu_ep_fallback",
          "the key is ONNX Runtime's own spelling");
}

void verify_cpu_builds_record_placement() {
  // Two builds of one model on one provider are one entry with two sessions:
  // the ledger is what the startup log and grparse_ort_sessions read.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  grparse::reset_ort_placements();
  const std::filesystem::path model = grparse_test::write_tiny_model();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  (void)grparse::make_session(env, model, "pooled model");
  (void)grparse::make_session(env, model, "pooled model");
  std::unique_ptr<Ort::Session> net(grparse::make_rapidocr_session(env, model, "ocr net", 1));
  const auto placements = grparse::ort_placements();
  require_equal(placements.size(), size_t{2}, "one entry per model");
  require(placements[0].model == "pooled model" && placements[0].sessions == 2 &&
              placements[0].requested == grparse::OrtEp::kCpu &&
              placements[0].actual == grparse::OrtEp::kCpu && placements[0].pin_reason.empty(),
          "the pooled model has two CPU sessions, first-built order");
  require(placements[1].model == "ocr net" && placements[1].sessions == 1,
          "the OCR builder records its net too");
  std::filesystem::remove(model);
}

void verify_pinned_cpu_session_is_declared() {
  // The one way to run on CPU under a GPU selection: a pin with a reason,
  // visible in the ledger as requested=OpenVINO, actual=CPU.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  grparse::reset_ort_placements();
  const std::filesystem::path model = grparse_test::write_tiny_model();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  const uint64_t before = grparse::ep_fallback_count();
  Ort::Session session =
      grparse::make_pinned_cpu_session(env, model, "pinned model", "test reason");
  require(session.GetInputCount() == 1, "the pinned session loads the model");
  const auto placements = grparse::ort_placements();
  const grparse::OrtPlacement* pinned = find_placement(placements, "pinned model");
  require(pinned != nullptr && pinned->requested == grparse::OrtEp::kOpenVino &&
              pinned->actual == grparse::OrtEp::kCpu && pinned->pin_reason == "test reason" &&
              pinned->device.empty(),
          "the pin is recorded with what was asked for and why");
  require_equal(grparse::ep_fallback_count(), before, "a pin is not a refusal");
  const std::string message = refusal_message(
      [&] { (void)grparse::make_pinned_cpu_session(env, model, "pinned model", ""); });
  require(message.contains("needs a reason"), "a pin without a reason is rejected");
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
  const std::filesystem::path model = grparse_test::write_tiny_model();
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
  const std::filesystem::path model = grparse_test::write_tiny_model();
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
  const std::filesystem::path model = grparse_test::write_tiny_model();
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

int64_t first_input_dim(const Ort::Session& session) {
  return session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape().front();
}

bool openvino_provider_built_in() {
  const std::vector<std::string> providers = Ort::GetAvailableProviders();
  return std::ranges::find(providers, "OpenVINOExecutionProvider") != providers.end();
}

// An engine that runs one item per call fixes the model's free batch
// dimension, so the OpenVINO provider compiles a fixed network at session
// build.  The name comes from the model; CPU sessions stay as they were.
void verify_fixed_shape_session_fixes_the_free_batch() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  const std::filesystem::path fixed = grparse_test::write_tiny_model();
  const std::filesystem::path free_dim = grparse_test::write_tiny_free_dim_model();

  require(grparse::unit_batch_overrides(env, fixed).empty(),
          "a model with a fixed batch needs no override");
  const std::vector<grparse::OrtDimOverride> overrides =
      grparse::unit_batch_overrides(env, free_dim);
  require(overrides.size() == 1 && overrides.front().name == "n" && overrides.front().value == 1,
          "the free batch dimension is found by its own name and fixed to 1");

  grparse::reset_ort_placements();
  const Ort::Session on_cpu =
      grparse::make_fixed_shape_session(env, free_dim, "free dim model", overrides);
  require_equal(first_input_dim(on_cpu), int64_t{-1}, "a CPU session keeps the free dimension");
  const auto placements = grparse::ort_placements();
  require(placements.size() == 1 && placements.front().model == "free dim model" &&
              placements.front().actual == grparse::OrtEp::kCpu,
          "the fixed-shape builder records its placement like make_session");

  if (openvino_provider_built_in()) {
    // The OpenVINO CPU device: the same provider path the GPU takes, with no
    // GPU in the test.
    selection.ep = grparse::OrtEp::kOpenVino;
    selection.openvino_device = "CPU";
    grparse::set_ort_ep_selection(selection);
    const Ort::Session on_openvino =
        grparse::make_fixed_shape_session(env, free_dim, "free dim model", overrides);
    require_equal(first_input_dim(on_openvino), int64_t{1},
                  "an OpenVINO session has the batch fixed to 1");
    selection.ep = grparse::OrtEp::kCpu;
    grparse::set_ort_ep_selection(selection);
  }
  std::filesystem::remove(fixed);
  std::filesystem::remove(free_dim);
}

void verify_make_rapidocr_session_fails_loudly_on_openvino() {
  // The no-degradation contract: with the OpenVINO provider unusable, the
  // OCR builder must retry and then THROW - never return a CPU session.  A
  // returned CPU session here would be the openvino flavor silently running
  // OCR on CPU.
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  const std::filesystem::path model = grparse_test::write_tiny_model();
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
  require_equal(grparse::ep_fallback_count(), before + 1,
                "the OCR builder counts its refusal like every other builder");
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
  require(threw, "a missing model on CPU throws");
  require_equal(grparse::ep_fallback_count(), before,
                "a CPU failure is not an execution-provider refusal");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ort-session-ep-test", "all checks passed", {
      verify_default_selection_is_cpu,
      verify_legacy_negative_index_appends_nothing_but_counts,
      verify_selection_round_trips_and_copies,
      verify_explicit_cpu_overrides_legacy_gpu_index,
      verify_latest_selection_wins,
      verify_ocr_batch_size_follows_provider,
      verify_make_session_refuses_and_counts,
      verify_cpu_selection_failure_does_not_count,
      verify_make_session_retries_on_openvino_then_refuses,
      verify_strict_placement_options,
      verify_cpu_builds_record_placement,
      verify_pinned_cpu_session_is_declared,
      verify_make_session_retries_then_succeeds,
      verify_injected_failures_exhaust_and_throw_on_cpu,
      verify_make_rapidocr_session_retries_then_succeeds,
      verify_fixed_shape_session_fixes_the_free_batch,
      verify_make_rapidocr_session_fails_loudly_on_openvino,
  });
}
