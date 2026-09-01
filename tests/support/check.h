// The assertion and main() boilerplate every test under tests/ used to spell
// out for itself: a throwing `require`, a value-printing `require_equal`, and
// the runner that turns a list of cases into an exit code.  Header only, and
// test-only: nothing under src/ or include/ may include it.
#ifndef GRPARSE_TESTS_SUPPORT_CHECK_H
#define GRPARSE_TESTS_SUPPORT_CHECK_H

#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <initializer_list>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace grparse_test {

// Fails the test with `message` when `condition` does not hold.  The message
// is the whole diagnostic, so spell out what was expected rather than which
// line it was on.
inline void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// The printable form of a value: strings and string-like types go through
// unquoted, everything else through std::format.
template <typename Value>
std::string shown(const Value& value) {
  if constexpr (std::is_convertible_v<const Value&, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::format("{}", value);
  }
}

// Fails with `what` plus both values when they differ, so a failure names the
// difference instead of only the expectation.
template <typename Actual, typename Expected>
void require_equal(const Actual& actual, const Expected& expected, std::string_view what) {
  if (actual == expected) return;
  throw std::runtime_error(std::format("{}\nexpected: {}\nactual:   {}", what,
                                       shown(expected), shown(actual)));
}

using test_case = std::function<void()>;

// The two lines a run can print, spelled out in full for a test whose
// failure prefix and success line do not share one name.  An empty line is
// not printed.
struct report_lines {
  std::string_view on_failure;
  std::string_view on_success;
};

// Runs every case in order.  The first std::exception prints
// "<on_failure>: <message>" on stderr and ends the process with
// EXIT_FAILURE; a clean run prints `on_success`.
inline int run_test_main(report_lines report, std::initializer_list<test_case> cases) {
  try {
    for (const test_case& one : cases) one();
  } catch (const std::exception& error) {
    std::println(stderr, "{}: {}", report.on_failure, error.what());
    return EXIT_FAILURE;
  }
  if (!report.on_success.empty()) std::println("{}", report.on_success);
  return EXIT_SUCCESS;
}

// The common shape: both lines carry the test's own name, so a clean run
// prints "<name>: <done>".
inline int run_test_main(std::string_view name, std::string_view done,
                         std::initializer_list<test_case> cases) {
  const std::string success = std::format("{}: {}", name, done);
  return run_test_main({.on_failure = name, .on_success = success}, cases);
}

// The same runner for a test that says nothing on success.
inline int run_test_main(std::string_view name, std::initializer_list<test_case> cases) {
  return run_test_main({.on_failure = name, .on_success = {}}, cases);
}

}  // namespace grparse_test

#endif
