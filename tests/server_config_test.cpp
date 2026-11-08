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
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#include "grparse/chart_extraction_policy.h"
#include "server_config.h"
#include "grparse/gpu_selection.h"
#include "grparse_session_ep.h"
#include "support/check.h"

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

void verify_auto_selection_uses_hardware_before_initialization() {
  using grparse::GpuHardware;
  using grparse::OrtEp;
  require(grparse::choose_auto_execution_provider({}, true, true) == OrtEp::kCpu,
          "compiled GPU providers do not invent a GPU on a CPU-only host");
  require(grparse::choose_auto_execution_provider(GpuHardware{.nvidia = true}, true, false) ==
              OrtEp::kCuda, "a visible NVIDIA GPU selects CUDA");
  require(grparse::choose_auto_execution_provider(GpuHardware{.intel = true}, false, true) ==
              OrtEp::kOpenVino, "a visible Intel GPU selects OpenVINO");
  require(grparse::choose_auto_execution_provider(
              GpuHardware{.nvidia = true, .intel = true, .intel_b70 = true}, true, true) ==
              OrtEp::kOpenVino, "B70 is preferred even when CUDA is available too");
  for (const GpuHardware hardware : {GpuHardware{.nvidia = true}, GpuHardware{.intel = true},
                                     GpuHardware{.amd = true}, GpuHardware{.other = true},
                                     GpuHardware{.intel = true, .intel_b70 = true}}) {
    bool threw = false;
    try {
      (void)grparse::choose_auto_execution_provider(hardware, false, false);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    require(threw, "present GPU without a matching provider must not select CPU");
  }
  require(grparse::is_b70_openvino_device_name("Intel(R) Arc(TM) Pro B70 Graphics (dGPU)"),
          "newer drivers expose the B70 marketing name");
  require(grparse::is_b70_openvino_device_name("Intel(R) Graphics [0xe223] (dGPU)"),
          "the shipped older driver exposes the same B70 by its PCI device ID");
  require(!grparse::is_b70_openvino_device_name("Intel(R) Graphics [0xe20b] (dGPU)"),
          "a different Intel GPU is not mistaken for B70");
  require(grparse::preferred_openvino_device("CPU") == "CPU",
          "explicit CPU device remains intentional CPU operation");
  require(grparse::preferred_openvino_device("GPU.3") == "GPU.3",
          "explicit GPU indices are not replaced by automatic preference");
}

void verify_gpu_discovery_distinguishes_missing_drivers_from_no_hardware() {
  namespace fs = std::filesystem;
  char pattern[] = "/tmp/grparse-gpu-selection-XXXXXX";
  const char* created = ::mkdtemp(pattern);
  require(created != nullptr, "the PCI fixture directory is created");
  const fs::path root(created);
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
  } cleanup{root};
  const auto device = [&](const char* name, const char* pci_class, const char* vendor,
                          const char* id) {
    const auto directory = root / name;
    fs::create_directory(directory);
    std::ofstream(directory / "class") << pci_class;
    std::ofstream(directory / "vendor") << vendor;
    std::ofstream(directory / "device") << id;
  };
  require(!grparse::read_gpu_hardware(root).intel, "an empty PCI tree has no Intel GPU");
  device("network", "0x020000", "0x8086", "0xe223");
  require(!grparse::read_gpu_hardware(root).intel_b70, "non-display PCI devices do not select a GPU");
  device("virtual-vga", "0x030000", "0x1234", "0x1111");
  device("bmc", "0x030000", "0x1a03", "0x2000");
  require(grparse::choose_auto_execution_provider(grparse::read_gpu_hardware(root), true, true) ==
              grparse::OrtEp::kCpu,
          "VM and BMC console displays do not invent a compute GPU on CPU-only hosts");
  device("b70", "0x030000", "0x8086", "0xe223");
  const auto hardware = grparse::read_gpu_hardware(root);
  require(hardware.intel && hardware.intel_b70,
          "B70 is detected without requiring a driver symlink or usable render node");
  device("unknown-gpu", "0x030200", "0x9999", "0x5678");
  require(grparse::read_gpu_hardware(root).other,
          "an unrecognized display vendor is not silently treated as no GPU");
  bool threw = false;
  try { (void)grparse::read_gpu_hardware(root / "missing"); }
  catch (const std::runtime_error&) { threw = true; }
  require(threw, "failed hardware discovery is not treated as proof of no GPU");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("server-config-test", "all checks passed", {
      verify_every_reported_endpoint_is_redacted,
      verify_retired_poi_target_is_reported,
      verify_remote_fetch_concurrency_setting,
      verify_collector_channels_setting,
      verify_collector_keepalive_settings,
      verify_auto_selection_uses_hardware_before_initialization,
      verify_gpu_discovery_distinguishes_missing_drivers_from_no_hardware,
  });
}
