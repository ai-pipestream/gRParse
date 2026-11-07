// bench-baseline-test (#251): tests/bench/baseline.textproto parses as a
// BenchBaseline, says why it was recorded and on what, and names every case
// grparse-bench runs exactly once with positive numbers. A unit test, so a
// broken or stale baseline fails the image build instead of the nightly.
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <google/protobuf/text_format.h>

#include "bench/bench_cases.h"
#include "bench_baseline.pb.h"
#include "support/check.h"

#ifndef GRPARSE_BENCH_SOURCE_DIR
#error "GRPARSE_BENCH_SOURCE_DIR names the source tree (CMakeLists.txt sets it)"
#endif

namespace {

using grparse_test::require;

grparse::test::bench::BenchBaseline load() {
  const std::filesystem::path path =
      std::filesystem::path(GRPARSE_BENCH_SOURCE_DIR) / "tests/bench/baseline.textproto";
  std::ifstream in(path, std::ios::binary);
  require(static_cast<bool>(in), "cannot read " + path.string());
  std::ostringstream text;
  text << in.rdbuf();
  grparse::test::bench::BenchBaseline baseline;
  require(google::protobuf::TextFormat::ParseFromString(text.str(), &baseline),
          path.string() + " does not parse as a grparse.test.bench.BenchBaseline");
  return baseline;
}

void verify_baseline_names_every_case() {
  const auto baseline = load();
  require(!baseline.reason().empty(), "the baseline says why it was recorded");
  require(!baseline.host().empty(), "the baseline names the machine it came from");
  std::set<std::string> named;
  for (const auto& one : baseline.cases()) {
    require(named.insert(one.name()).second, "the baseline names " + one.name() + " twice");
    require(one.ns_per_op() > 0 && one.items_per_second() > 0 && !one.item().empty(),
            one.name() + " carries positive ns/op and items/s and says what an item is");
  }
  for (const auto name : grparse_bench::kCaseNames) {
    require(named.erase(std::string(name)) == 1, "the baseline has no entry for " + std::string(name));
  }
  require(named.empty(), "the baseline names a case grparse-bench does not run: " +
                             (named.empty() ? std::string() : *named.begin()));
}

}  // namespace

int main() {
  return grparse_test::run_test_main("bench-baseline-test", "baseline names every case",
                                     {verify_baseline_names_every_case});
}
