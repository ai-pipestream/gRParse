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
    require(grparse::encode_base64("memory", 6) == "bWVtb3J5", "basic encode");
    require(grparse::encode_base64("a", 1) == "YQ==", "encode pads one char");
    require(grparse::encode_base64("ab", 2) == "YWI=", "encode pads two chars");
    require(grparse::encode_base64("abc", 3) == "YWJj", "encode without padding");
    require(grparse::encode_base64("", 0).empty(), "encode empty input");
    const unsigned char binary[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    require(grparse::encode_base64(binary, sizeof(binary)) == "iVBORw0KGgo=",
            "encode binary bytes");
    require(grparse::decode_base64(grparse::encode_base64(binary, sizeof(binary))) ==
                std::string("\x89PNG\r\n\x1A\n", 8),
            "encode and decode round-trip");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "base64-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
