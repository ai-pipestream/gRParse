#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "grparse/base64.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void expect_invalid(const std::string& value) {
  try {
    grparse::decode_base64(value);
    throw std::runtime_error("expected invalid base64: " + value);
  } catch (const std::invalid_argument&) {
  }
}

}  // namespace

int main() {
  try {
    require(grparse::decode_base64("bWVtb3J5") == "memory", "basic decode");
    require(grparse::decode_base64("bWVt\nb3J5") == "memory", "whitespace ignored");
    require(grparse::decode_base64("YQ==") == "a", "padding one char");
    require(grparse::decode_base64("YWI=") == "ab", "padding two chars");
    require(grparse::decode_base64("YWJj") == "abc", "no padding");
    expect_invalid("");
    expect_invalid("abc");  // bad length
    expect_invalid("====");
    expect_invalid("a===");
    expect_invalid("ab=c");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "base64-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
