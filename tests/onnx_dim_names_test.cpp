// Proves name_unnamed_onnx_dims (src/tensorrt/onnx_dim_names.cpp) on a
// hand-built ModelProto: the "?" height and width of an input and an output
// get their own names in place, other dimensions and bytes stay, and
// malformed bytes throw.

#include <cstdint>
#include <stdexcept>
#include <string>

#include "../src/tensorrt/onnx_dim_names.h"
#include "support/check.h"

namespace {

using grparse_test::require;

std::string varint(uint64_t v) {
  std::string out;
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
  return out;
}

std::string field(int number, const std::string& payload) {
  return varint(static_cast<uint64_t>(number) << 3 | 2) + varint(payload.size()) + payload;
}

std::string value_field(int number, uint64_t value) { return varint(static_cast<uint64_t>(number) << 3) + varint(value); }

// ValueInfoProto{name, type{tensor_type{elem_type 1, shape{dims}}}}.
std::string value_info(const std::string& name, const std::string& dims) {
  const std::string shape = field(2, dims);
  const std::string tensor = value_field(1, 1) + shape;
  return field(1, name) + field(2, field(1, tensor));
}

std::string dim_value(uint64_t v) { return field(1, value_field(1, v)); }
std::string dim_param(const std::string& p) { return field(1, field(2, p)); }

std::string model() {
  const std::string input = value_info("x", dim_value(1) + dim_value(3) + dim_param("?") + dim_param("?"));
  const std::string output = value_info("y", dim_param("batch") + dim_param("?"));
  const std::string graph = field(1, "node-bytes") + field(11, input) + field(12, output);
  return value_field(1, 8) + field(7, graph);
}

void verify_renames() {
  std::string bytes = model();
  const std::string before = bytes;
  require(grparse::trt::name_unnamed_onnx_dims(&bytes) == 3, "three unnamed dimensions are renamed");
  require(bytes.size() == before.size(), "the model keeps its length");
  require(bytes.find("?") == std::string::npos, "no dimension is left named ?");
  require(bytes.find("batch") != std::string::npos, "a named dimension stays");
  int changed = 0;
  for (size_t i = 0; i < bytes.size(); ++i) changed += bytes[i] != before[i];
  require(changed == 3, "only the three name bytes change");
}

void verify_malformed() {
  std::string truncated = model().substr(0, 9);
  bool threw = false;
  try {
    grparse::trt::name_unnamed_onnx_dims(&truncated);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a truncated model throws");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("onnx-dim-names-test", "all checks passed", {
      verify_renames,
      verify_malformed,
  });
}
