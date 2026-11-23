// The case names grparse-bench runs, in order. bench-baseline-test holds
// tests/bench/baseline.textproto to naming every one, so a case added here
// without a recorded baseline fails the unit tier instead of the nightly.
#ifndef GRPARSE_TESTS_BENCH_BENCH_CASES_H
#define GRPARSE_TESTS_BENCH_BENCH_CASES_H

#include <array>
#include <string_view>

namespace grparse_bench {

inline constexpr std::array<std::string_view, 6> kCaseNames{
    "ttfp_inspector_fast_path", "assembly_200_pages", "fold_and_delta",
    "render_exports",           "sniff_corpus",       "base64_10mib",
};

// A case is slower than its baseline when its ns/op exceeds the baseline's
// by this factor, or its items/s falls below the baseline's by it.
inline constexpr double kSlowdownLimit = 1.5;

}  // namespace grparse_bench

#endif  // GRPARSE_TESTS_BENCH_BENCH_CASES_H
