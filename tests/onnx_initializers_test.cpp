// The ONNX initializer reader against hand-encoded model bytes: raw and
// packed float data, a non-float initializer skipped, and malformed input
// refused by name.
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "levelzero/onnx_initializers.h"
#include "support/check.h"

namespace {

using grparse::levelzero::parse_onnx_initializers;
using grparse_test::require;
using grparse_test::require_equal;

std::string varint(uint64_t value) {
  std::string out;
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value != 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (value != 0);
  return out;
}

std::string field(uint32_t number, uint32_t type) { return varint((uint64_t(number) << 3) | type); }
std::string bytes_field(uint32_t number, const std::string& value) {
  return field(number, 2) + varint(value.size()) + value;
}
std::string float_bytes(const std::vector<float>& values) {
  std::string out(values.size() * sizeof(float), '\0');
  std::memcpy(out.data(), values.data(), out.size());
  return out;
}

std::string model_with(const std::vector<std::string>& tensors) {
  std::string graph = bytes_field(1, "node-bytes-ignored");
  for (const std::string& tensor : tensors) graph += bytes_field(5, tensor);
  return field(1, 0) + varint(8) + bytes_field(7, graph);  // ir_version, then the graph
}

void reads_raw_and_packed_floats() {
  const std::string raw = field(1, 0) + varint(2) + field(1, 0) + varint(3) + field(2, 0) + varint(1) +
                          bytes_field(8, "w") + bytes_field(9, float_bytes({1, 2, 3, 4, 5, 6}));
  const std::string packed = bytes_field(1, varint(2)) + field(2, 0) + varint(1) + bytes_field(8, "b") +
                             bytes_field(4, float_bytes({0.5F, -0.25F}));
  const std::string ints = field(1, 0) + varint(3) + field(2, 0) + varint(7) + bytes_field(8, "shape") +
                           bytes_field(9, std::string(24, '\1'));
  const auto read = parse_onnx_initializers(model_with({raw, ints, packed}), "test model");
  require_equal(read.size(), size_t(2), "the int64 initializer is skipped");
  require(read.at("w").dims == std::vector<int64_t>{2, 3}, "w keeps its dims");
  require(read.at("w").values == std::vector<float>{1, 2, 3, 4, 5, 6}, "w keeps its raw values");
  require(read.at("b").values == std::vector<float>{0.5F, -0.25F}, "b keeps its packed values");
}

void refuses(const std::string& bytes, const std::string& why) {
  bool threw = false;
  try {
    (void)parse_onnx_initializers(bytes, "broken.onnx");
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()).starts_with("broken.onnx: not a readable ONNX model");
  }
  require(threw, "refused, naming the file: " + why);
}

void refuses_malformed_models() {
  refuses(field(1, 0) + varint(8), "a model with no graph");
  refuses(field(7, 2) + varint(50) + "short", "a graph past the end");
  const std::string wrong_count = field(1, 0) + varint(4) + field(2, 0) + varint(1) + bytes_field(8, "w") +
                                  bytes_field(9, float_bytes({1, 2}));
  refuses(model_with({wrong_count}), "an initializer with fewer values than its dims");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("onnx-initializers-test", "onnx initializers ok",
                                     {reads_raw_and_packed_floats, refuses_malformed_models});
}
