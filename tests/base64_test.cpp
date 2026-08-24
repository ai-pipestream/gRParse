#include <cstdio>
#include <cstdlib>
#include <print>
#include <random>
#include <stdexcept>
#include <string>

#include "../src/base64_scalar.h"
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

    // The vector fast path and the scalar contract oracle must agree on
    // every input: identical encodings, identical decodings, and identical
    // accept/reject decisions, including on corrupted and whitespace-laced
    // strings. Fixed seed keeps failures reproducible.
    std::mt19937_64 rng(0xba5e64);
    for (int round = 0; round < 4000; ++round) {
      const std::size_t size = rng() % 512;
      std::string payload(size, '\0');
      for (auto& byte : payload) byte = static_cast<char>(rng());

      const std::string encoded = grparse::encode_base64(payload.data(), payload.size());
      require(encoded == grparse::detail::scalar_encode_base64(payload.data(), payload.size()),
              "vector and scalar encodings agree");
      if (!payload.empty()) {
        require(grparse::decode_base64(encoded) == payload, "fuzz round-trip");
      }

      // Lace with whitespace at random positions: still decodable, same
      // bytes out of both paths.
      std::string laced = encoded;
      for (int i = 0; i < 4 && !laced.empty(); ++i) {
        laced.insert(rng() % (laced.size() + 1), 1, " \t\n\r"[rng() % 4]);
      }
      if (!payload.empty()) {
        require(grparse::decode_base64(laced) == payload, "whitespace-laced decode");
      }

      // Corrupt one position with an arbitrary byte; both paths must agree
      // on acceptance, and when both accept, on the decoded bytes.
      std::string corrupted = laced.empty() ? std::string("A") : laced;
      corrupted[rng() % corrupted.size()] = static_cast<char>(rng());
      bool fast_ok = true;
      std::string fast_out;
      try {
        fast_out = grparse::decode_base64(corrupted);
      } catch (const std::invalid_argument&) {
        fast_ok = false;
      }
      bool scalar_ok = true;
      std::string scalar_out;
      try {
        scalar_out = grparse::detail::scalar_decode_base64(corrupted);
      } catch (const std::invalid_argument&) {
        scalar_ok = false;
      }
      require(fast_ok == scalar_ok, "accept/reject decisions agree on corrupted input");
      if (fast_ok) require(fast_out == scalar_out, "accepted corrupted decodes agree");
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "base64-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
