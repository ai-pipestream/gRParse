// The startup report must never print an endpoint's credentials: VLM
// endpoints carry them as userinfo or in the query, and container logs are
// no place for either. Each endpoint the report names is configured with
// both here, and the captured output must carry neither.
// A setting the server no longer reads is named in a startup warning.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

#include "grparse/chart_extraction_policy.h"
#include "grparse_session_ep.h"
#include "server_config.h"
#include "support/check.h"
#include "support/tiny_onnx_model.h"

namespace {

using grparse_test::require;

// Runs `body` with one standard stream redirected into a pipe and returns
// what it wrote. The report is a few lines, well under a pipe's buffer.
template <typename Body>
std::string captured(std::FILE* stream, int fd, Body body) {
  int ends[2];
  require(::pipe(ends) == 0, "a pipe for the capture");
  std::fflush(stream);
  const int saved = ::dup(fd);
  ::dup2(ends[1], fd);
  ::close(ends[1]);
  body();
  std::fflush(stream);
  ::dup2(saved, fd);
  ::close(saved);
  std::string out;
  char buffer[4096];
  for (ssize_t got; (got = ::read(ends[0], buffer, sizeof buffer)) > 0;) {
    out.append(buffer, static_cast<size_t>(got));
  }
  ::close(ends[0]);
  return out;
}

template <typename Body>
std::string captured_stdout(Body body) {
  return captured(stdout, STDOUT_FILENO, body);
}

template <typename Body>
std::string captured_stderr(Body body) {
  return captured(stderr, STDERR_FILENO, body);
}

void verify_every_reported_endpoint_is_redacted() {
  const std::map<std::string, std::string> env = {
      {"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS",
       R"(presets { key: "mine" value { model: "m" url: "https://preset-user:preset-key@vlm.example/v1?api_key=preset-q" } })"},
      {"GRPARSE_DEFAULT_CHART_EXTRACTION_PRESET", "mine"},
  };
  grparse::CollectorTargets targets;
  targets.chart_policy = grparse::chart_extraction_policy_from_env(
      [&env](const char* name) -> const char* {
        const auto found = env.find(name);
        return found == env.end() ? nullptr : found->second.c_str();
      });
  targets.derender.target = "enrich:50051";
  targets.derender.vlm_endpoint = "https://enrich-user:enrich-key@vlm.example/v1?token=enrich-q";
  targets.vlm.target = "vlm:50051";
  targets.vlm.endpoint = "http://vlm-user:vlm-key@vlm.example/v1#vlm-q";

  const std::string out =
      captured_stdout([&] { grparse::report_collector_targets(targets, false, false); });
  require(out.contains("chart extraction policy"), "the report ran: " + out);
  for (const char* secret : {"preset-user", "preset-key", "preset-q", "enrich-user",
                             "enrich-key", "enrich-q", "vlm-user", "vlm-key", "vlm-q"}) {
    require(!out.contains(secret), std::string("the report leaked ") + secret + ":\n" + out);
  }
  require(out.contains("<redacted>@vlm.example/v1"),
          "the endpoints are still named, with their credentials redacted:\n" + out);
}

// GRPARSE_POI_TARGET is no longer read: a deployment that still sets it
// is told so in one line naming the variable, and one that does not hears
// nothing about it.
void verify_retired_poi_target_is_reported() {
  ::unsetenv("GRPARSE_POI_TARGET");
  require(captured_stderr([] { grparse::report_retired_settings(); }).empty(),
          "nothing is reported when no retired setting is set");
  ::setenv("GRPARSE_POI_TARGET", "poic:50051", 1);
  const std::string out = captured_stderr([] { grparse::report_retired_settings(); });
  ::unsetenv("GRPARSE_POI_TARGET");
  require(out.contains("GRPARSE_POI_TARGET") && out.contains("grPOIc is no longer used"),
          "the warning names the variable and says grPOIc is not used:\n" + out);
  require(std::ranges::count(out, '\n') == 1, "the warning is one line:\n" + out);
}

// GRPARSE_REMOTE_FETCH_CONCURRENCY defaults to four slots, takes a count,
// and refuses a value that would leave no fetch able to run.
void verify_remote_fetch_concurrency_setting() {
  ::unsetenv("GRPARSE_REMOTE_FETCH_CONCURRENCY");
  require(grparse::read_remote_fetch_concurrency() == 4, "unset, four fetches run at once");
  ::setenv("GRPARSE_REMOTE_FETCH_CONCURRENCY", "2", 1);
  require(grparse::read_remote_fetch_concurrency() == 2, "the setting sizes the slots");
  for (const char* refused : {"0", "-1", "four", "2048"}) {
    ::setenv("GRPARSE_REMOTE_FETCH_CONCURRENCY", refused, 1);
    bool threw = false;
    try {
      grparse::read_remote_fetch_concurrency();
    } catch (const std::invalid_argument& error) {
      threw = std::string(error.what()).contains("GRPARSE_REMOTE_FETCH_CONCURRENCY");
    }
    require(threw, std::string("'") + refused + "' is refused at startup, naming the variable");
  }
  ::unsetenv("GRPARSE_REMOTE_FETCH_CONCURRENCY");
}

// GRPARSE_COLLECTOR_CHANNELS (issue #250) defaults to a pool of two per
// collector, takes a count up to 64, refuses anything else naming the
// variable, and the startup report says what the pools are.
void verify_collector_channels_setting() {
  ::unsetenv("GRPARSE_COLLECTOR_CHANNELS");
  const grparse::CollectorTargets defaults = grparse::read_collector_targets();
  require(defaults.channel_options.channels == 2, "unset, two channels per collector");
  require(defaults.channel_options.keepalive_time.count() == 330000 &&
              defaults.channel_options.keepalive_timeout.count() == 20000,
          "the keepalive timings default to 330 s / 20 s");
  ::setenv("GRPARSE_COLLECTOR_CHANNELS", "4", 1);
  const grparse::CollectorTargets four = grparse::read_collector_targets();
  require(four.channel_options.channels == 4, "the setting sizes the pools");
  const std::string out =
      captured_stdout([&] { grparse::report_collector_targets(four, false, false); });
  require(out.contains("gRParse collector channels: 4 per collector "
                       "(GRPARSE_COLLECTOR_CHANNELS)") &&
              out.contains("keepalive 330000 ms (GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS), "
                           "20000 ms timeout (GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS)"),
          "the startup report names the pool size and the keepalive:\n" + out);
  for (const char* refused : {"0", "-1", "two", "65", ""}) {
    ::setenv("GRPARSE_COLLECTOR_CHANNELS", refused, 1);
    bool threw = false;
    try {
      grparse::read_collector_targets();
    } catch (const std::invalid_argument& error) {
      threw = std::string(error.what()).contains("GRPARSE_COLLECTOR_CHANNELS");
    }
    require(threw, std::string("'") + refused + "' is refused at startup, naming the variable");
  }
  ::unsetenv("GRPARSE_COLLECTOR_CHANNELS");
}

// GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS and GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS
// (issue #250 follow-up): unset, 330 s and 20 s, above every default server
// ping policy; set, milliseconds inside 10 s..2 h and 1 s..10 min, shown in
// the startup report; anything else refuses startup naming the variable.
void verify_collector_keepalive_settings() {
  ::unsetenv("GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS");
  ::unsetenv("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS");
  const grparse::CollectorTargets defaults = grparse::read_collector_targets();
  require(defaults.channel_options.keepalive_time == std::chrono::milliseconds{330000},
          "unset, the keepalive time is 330 s");
  require(defaults.channel_options.keepalive_timeout == std::chrono::milliseconds{20000},
          "unset, the keepalive timeout is 20 s");

  ::setenv("GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS", "30000", 1);
  ::setenv("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS", "5000", 1);
  const grparse::CollectorTargets set = grparse::read_collector_targets();
  require(set.channel_options.keepalive_time == std::chrono::milliseconds{30000},
          "the time setting is the channels' keepalive time");
  require(set.channel_options.keepalive_timeout == std::chrono::milliseconds{5000},
          "the timeout setting is the channels' keepalive timeout");
  const std::string out =
      captured_stdout([&] { grparse::report_collector_targets(set, false, false); });
  require(out.contains("keepalive 30000 ms (GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS), "
                       "5000 ms timeout (GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS)"),
          "the startup report shows both settings:\n" + out);

  ::setenv("GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS", "10000", 1);
  ::setenv("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS", "1000", 1);
  const grparse::CollectorTargets floors = grparse::read_collector_targets();
  require(floors.channel_options.keepalive_time == std::chrono::milliseconds{10000} &&
              floors.channel_options.keepalive_timeout == std::chrono::milliseconds{1000},
          "10 s and 1 s are the lowest accepted values");

  const auto refused = [](const char* name, const char* value) {
    ::setenv(name, value, 1);
    bool threw = false;
    try {
      grparse::read_collector_targets();
    } catch (const std::invalid_argument& error) {
      threw = std::string(error.what()).contains(name);
    }
    require(threw, std::string(name) + "='" + value + "' is refused, naming the variable");
  };
  ::unsetenv("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS");
  for (const char* value : {"9999", "0", "-1", "330s", "", "7200001"}) {
    refused("GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS", value);
  }
  ::unsetenv("GRPARSE_COLLECTOR_KEEPALIVE_TIME_MS");
  for (const char* value : {"999", "0", "20s", "", "600001"}) {
    refused("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS", value);
  }
  ::unsetenv("GRPARSE_COLLECTOR_KEEPALIVE_TIMEOUT_MS");
}

// One device, never a list that lets OpenVINO move work to its CPU plugin.
void verify_openvino_device_setting() {
  ::unsetenv("GRPARSE_OPENVINO_DEVICE");
  require(grparse::openvino_device_setting() == "GPU", "the default device is GPU");
  for (const char* accepted : {"GPU", "GPU.0", "GPU.12", "NPU", "NPU.1", "CPU"}) {
    ::setenv("GRPARSE_OPENVINO_DEVICE", accepted, 1);
    require(grparse::openvino_device_setting() == accepted,
            std::string("a single device is accepted: ") + accepted);
  }
  for (const char* refused : {"AUTO:GPU,CPU", "AUTO", "HETERO:GPU,CPU", "MULTI:GPU,CPU",
                              "BATCH:GPU", "GPU.", "GPU.x", "GPUX", "gpu", "GPU;rm"}) {
    ::setenv("GRPARSE_OPENVINO_DEVICE", refused, 1);
    bool threw = false;
    try {
      (void)grparse::openvino_device_setting();
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw, std::string("a device list or malformed device is refused: ") + refused);
  }
  ::unsetenv("GRPARSE_OPENVINO_DEVICE");
}

// The startup report names each model's provider and device, and a CPU pin
// under a GPU selection carries its reason.
void verify_model_placements_are_reported() {
  grparse::reset_ort_placements();
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  grparse::set_ort_ep_selection(selection);
  require(captured_stdout([] { grparse::report_model_placements(); }).empty(),
          "no sessions, no placement lines");
  const std::filesystem::path model = grparse_test::write_tiny_model();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  for (int session = 0; session < 2; ++session) {
    (void)grparse::make_pinned_cpu_session(env, model, "table structure", "test pin");
  }
  const std::string out = captured_stdout([] { grparse::report_model_placements(); });
  require(out == "gRParse model placement: table structure on CPU, 2 sessions, pinned to CPU "
                 "under OpenVINO: test pin\n",
          "a pinned model reports its provider, count and reason: " + out);
  std::filesystem::remove(model);
  grparse::reset_ort_placements();
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
}

// Health tells an engine the operator switched off from one startup was
// allowed to build and did not: only the second is a missing part. An engine
// that works on layout's regions follows GRPARSE_LAYOUT=off.
void verify_engine_inventory_tells_switched_off_from_not_built() {
  using grparse::EngineState;
  for (const char* name : {"GRPARSE_LAYOUT", "GRPARSE_TABLE_STRUCTURE", "GRPARSE_FIGURE_CLASSES"}) {
    ::unsetenv(name);
  }
  grparse::EmbeddingConfig embeddings;
  const auto all = grparse::read_engine_inventory(
      {.layout = true, .table_structure = true, .figure_classifier = true, .embeddings = false},
      embeddings);
  require(all.ocr == EngineState::kLoaded && all.layout == EngineState::kLoaded &&
              all.table_structure == EngineState::kLoaded &&
              all.figure_classifier == EngineState::kLoaded,
          "built engines are loaded");
  require(all.embeddings == EngineState::kDisabled,
          "embeddings with no backend selected are disabled");
  embeddings.backend = grparse::EmbeddingBackend::cpu;
  require(grparse::read_engine_inventory({.embeddings = true}, embeddings).embeddings ==
              EngineState::kLoaded,
          "a built embedding engine is loaded");

  // Every setting on and nothing built. Startup cannot leave it so (a
  // missing model file stops it), and an inventory that says so still
  // reports the engine as missing, never as switched off.
  const auto none = grparse::read_engine_inventory({}, embeddings);
  require(none.ocr == EngineState::kLoaded, "OCR is loaded whenever startup got this far");
  require(none.layout == EngineState::kNotLoaded &&
              none.table_structure == EngineState::kNotLoaded &&
              none.figure_classifier == EngineState::kNotLoaded,
          "an engine its setting allowed and startup did not build is not loaded");

  ::setenv("GRPARSE_TABLE_STRUCTURE", "off", 1);
  const auto table_off = grparse::read_engine_inventory({.layout = true}, embeddings);
  require(table_off.table_structure == EngineState::kDisabled &&
              table_off.figure_classifier == EngineState::kNotLoaded,
          "an engine's own off setting disables that engine only");
  ::unsetenv("GRPARSE_TABLE_STRUCTURE");

  ::setenv("GRPARSE_LAYOUT", "off", 1);
  const auto layout_off = grparse::read_engine_inventory({}, embeddings);
  require(layout_off.layout == EngineState::kDisabled &&
              layout_off.table_structure == EngineState::kDisabled &&
              layout_off.figure_classifier == EngineState::kDisabled,
          "GRPARSE_LAYOUT=off disables the engines that work on layout's regions");
  ::unsetenv("GRPARSE_LAYOUT");
}

constexpr const char* kPassSettings[] = {"GRPARSE_LAYOUT", "GRPARSE_TABLE_STRUCTURE",
                                         "GRPARSE_FIGURE_CLASSES"};

// A models directory that holds none of the three optional models.
std::filesystem::path empty_models_dir() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("grparse-no-models-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// What building one engine did: the startup failure's words, or how it
// went on without one.
template <typename Build>
std::string startup_failure(Build build) {
  try {
    return build() == nullptr ? "it went on without the engine" : "it built an engine";
  } catch (const std::exception& error) {
    return std::string("stopped: ") + error.what();
  }
}

// Layout, table structure and the figure classifier default to on (#410):
// a model file that is not there stops startup, and the message names the
// setting and the path it looked at. Each used to default to auto, which
// printed one line and served every document without the pass.
void verify_a_missing_model_file_stops_startup_by_default() {
  for (const char* name : kPassSettings) ::unsetenv(name);
  ::unsetenv("GRPARSE_LAYOUT_MODEL");
  const std::filesystem::path models = empty_models_dir();
  const auto stops = [&](const char* setting, const char* file, const std::string& outcome) {
    const std::string path = (models / file).string();
    require(outcome.starts_with("stopped: ") && outcome.contains(setting) &&
                outcome.contains(path) && outcome.contains(std::string(setting) + "=off"),
            std::string(setting) + " unset with no " + file +
                " must stop startup naming the setting, the path and the way to run "
                "without the pass; " +
                outcome);
  };
  std::string layout;
  std::string table;
  std::string figures;
  // The builders print their startup line; it is not this test's output.
  (void)captured_stdout([&] {
    layout = startup_failure([&] { return grparse::build_layout_engine(models); });
    table = startup_failure(
        [&] { return grparse::build_table_structure_pool(models, 1, /*layout_active=*/true); });
    figures = startup_failure(
        [&] { return grparse::build_figure_classifier_pool(models, 1, /*layout_active=*/true); });
  });
  stops("GRPARSE_LAYOUT", "layout_heron.onnx", layout);
  stops("GRPARSE_TABLE_STRUCTURE", "slanet_plus.onnx", table);
  stops("GRPARSE_FIGURE_CLASSES", "figure_classifier.onnx", figures);

  // The explicit spelling of the default says the same.
  for (const char* name : kPassSettings) ::setenv(name, "on", 1);
  (void)captured_stdout([&] {
    layout = startup_failure([&] { return grparse::build_layout_engine(models); });
  });
  stops("GRPARSE_LAYOUT", "layout_heron.onnx", layout);
  for (const char* name : kPassSettings) ::unsetenv(name);
  std::filesystem::remove_all(models);
}

// off is the only way to run without a pass. GRPARSE_LAYOUT=off takes the
// regions away from the two passes that work on them, so they follow it
// when their own setting is unset, and say so in the startup log whether or
// not their model file is there. auto, the old default that let a missing
// file through, is refused by name.
void verify_off_is_the_only_way_to_run_without_a_pass() {
  const std::filesystem::path models = empty_models_dir();
  for (const char* name : kPassSettings) ::setenv(name, "off", 1);
  // A check that fails inside the capture would leave stdout redirected, so
  // the captured bodies only record what happened.
  bool none_built = false;
  std::string out = captured_stdout([&] {
    none_built = grparse::build_layout_engine(models) == nullptr &&
                 grparse::build_table_structure_pool(models, 1, true) == nullptr &&
                 grparse::build_figure_classifier_pool(models, 1, true) == nullptr;
  });
  require(none_built, "a pass whose setting is off builds no engine and needs no model file");
  require(out.contains("gRParse layout: disabled (GRPARSE_LAYOUT=off)") &&
              out.contains("gRParse table structure: disabled (GRPARSE_TABLE_STRUCTURE=off)") &&
              out.contains("gRParse figure classes: disabled (GRPARSE_FIGURE_CLASSES=off)"),
          "each switched-off pass says so in the startup log:\n" + out);

  ::unsetenv("GRPARSE_TABLE_STRUCTURE");
  ::unsetenv("GRPARSE_FIGURE_CLASSES");
  out = captured_stdout([&] {
    none_built =
        grparse::build_table_structure_pool(models, 1, /*layout_active=*/false) == nullptr &&
        grparse::build_figure_classifier_pool(models, 1, /*layout_active=*/false) == nullptr;
  });
  require(none_built, "with layout off the passes that work on its regions are off too");
  require(out.contains("gRParse table structure: disabled (GRPARSE_LAYOUT=off)") &&
              out.contains("gRParse figure classes: disabled (GRPARSE_LAYOUT=off)"),
          "a pass that follows GRPARSE_LAYOUT=off says so, model file or not:\n" + out);

  // Asking for a pass by name while switching off the layout it needs is a
  // contradiction, not a pass to drop quietly.
  ::setenv("GRPARSE_TABLE_STRUCTURE", "on", 1);
  const std::string contradiction = startup_failure(
      [&] { return grparse::build_table_structure_pool(models, 1, /*layout_active=*/false); });
  require(contradiction.starts_with("stopped: ") &&
              contradiction.contains("GRPARSE_TABLE_STRUCTURE=on"),
          "GRPARSE_TABLE_STRUCTURE=on with layout off stops startup; " + contradiction);

  for (const char* name : kPassSettings) {
    for (const char* other : kPassSettings) ::unsetenv(other);
    ::setenv(name, "auto", 1);
    std::string outcome;
    (void)captured_stdout([&] {
      outcome = std::string(name) == "GRPARSE_LAYOUT"
                    ? startup_failure([&] { return grparse::build_layout_engine(models); })
                : std::string(name) == "GRPARSE_TABLE_STRUCTURE"
                    ? startup_failure(
                          [&] { return grparse::build_table_structure_pool(models, 1, true); })
                    : startup_failure(
                          [&] { return grparse::build_figure_classifier_pool(models, 1, true); });
    });
    require(outcome.starts_with("stopped: ") && outcome.contains(name) &&
                outcome.contains("on or off"),
            std::string(name) + "=auto is refused naming the variable and what it accepts; " +
                outcome);
  }
  for (const char* name : kPassSettings) ::unsetenv(name);
  std::filesystem::remove_all(models);
}

}  // namespace

int main() {
  return grparse_test::run_test_main("server-config-test", "all checks passed", {
      verify_every_reported_endpoint_is_redacted,
      verify_retired_poi_target_is_reported,
      verify_remote_fetch_concurrency_setting,
      verify_collector_channels_setting,
      verify_collector_keepalive_settings,
      verify_openvino_device_setting,
      verify_model_placements_are_reported,
      verify_engine_inventory_tells_switched_off_from_not_built,
      verify_a_missing_model_file_stops_startup_by_default,
      verify_off_is_the_only_way_to_run_without_a_pass,
  });
}
