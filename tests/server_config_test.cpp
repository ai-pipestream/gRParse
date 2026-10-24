// The startup report must never print an endpoint's credentials: VLM
// endpoints carry them as userinfo or in the query, and container logs are
// no place for either. Each endpoint the report names is configured with
// both here, and the captured output must carry neither.
// A setting the server no longer reads is named in a startup warning.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

#include "grparse/chart_extraction_policy.h"
#include "server_config.h"
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

}  // namespace

int main() {
  return grparse_test::run_test_main("server-config-test", "all checks passed", {
      verify_every_reported_endpoint_is_redacted,
      verify_retired_poi_target_is_reported,
      verify_remote_fetch_concurrency_setting,
  });
}
