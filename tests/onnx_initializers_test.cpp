// The ONNX reader against hand-encoded model bytes: raw and packed float
// data, integer tensors kept out of the float initializers, nodes with their
// attributes, a Constant node's tensor, and malformed input refused by name.
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "levelzero/onnx_initializers.h"
#include "support/check.h"

namespace {

using grparse::levelzero::parse_onnx_graph;
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

// A NodeProto: inputs (1), outputs (2), op_type (4), attributes (5).
std::string node(const std::string& op, const std::vector<std::string>& inputs, const std::string& output,
                 const std::vector<std::string>& attributes = {}) {
  std::string out;
  for (const std::string& input : inputs) out += bytes_field(1, input);
  out += bytes_field(2, output) + bytes_field(4, op);
  for (const std::string& attribute : attributes) out += bytes_field(5, attribute);
  return out;
}

std::string model_with(const std::vector<std::string>& tensors, const std::vector<std::string>& nodes = {}) {
  std::string graph;
  for (const std::string& one : nodes) graph += bytes_field(1, one);
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

void reads_nodes_and_constants() {
  // Conv(x, w) with strides [2, 1] and group 3; a Constant holding int64 [0, -1]; graph input x, output y.
  const std::string weights = field(1, 0) + varint(1) + field(2, 0) + varint(1) + bytes_field(8, "w") +
                              bytes_field(9, float_bytes({2.5F}));
  const std::string strides = bytes_field(1, "strides") + field(8, 0) + varint(2) + field(8, 0) + varint(1);
  const std::string group = bytes_field(1, "group") + field(3, 0) + varint(3);
  const std::string shape_tensor = field(1, 0) + varint(2) + field(2, 0) + varint(7) +
                                   bytes_field(7, varint(0) + varint(static_cast<uint64_t>(-1)));
  const std::string value = bytes_field(1, "value") + bytes_field(5, shape_tensor);
  std::string graph = bytes_field(1, node("Constant", {}, "shape", {value})) +
                      bytes_field(1, node("Conv", {"x", "w"}, "y", {strides, group})) + bytes_field(5, weights) +
                      bytes_field(11, bytes_field(1, "x")) + bytes_field(11, bytes_field(1, "w")) +
                      bytes_field(12, bytes_field(1, "y"));
  const auto read = parse_onnx_graph(bytes_field(7, graph), "test model");
  require_equal(read.nodes.size(), size_t(2), "both nodes are read");
  const auto& conv = read.nodes[1];
  require(conv.op_type == "Conv" && conv.inputs == std::vector<std::string>{"x", "w"} &&
              conv.outputs == std::vector<std::string>{"y"},
          "the Conv keeps its op, inputs and output");
  require(conv.attribute("strides") != nullptr && conv.attribute("strides")->ints == std::vector<int64_t>{2, 1},
          "strides are read as ints");
  require(conv.attribute("group") != nullptr && conv.attribute("group")->i == 3, "group is read as an int");
  require(conv.attribute("pads") == nullptr, "an absent attribute is null");
  require(read.constants.at("shape").ints == std::vector<int64_t>{0, -1}, "the Constant's int64 tensor is kept");
  require(read.constants.at("w").values == std::vector<float>{2.5F}, "the initializer is a constant");
  require(read.inputs == std::vector<std::string>{"x"}, "an initializer listed as an input is not a graph input");
  require(read.outputs == std::vector<std::string>{"y"}, "the graph output is read");
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
                                     {reads_raw_and_packed_floats, reads_nodes_and_constants,
                                      refuses_malformed_models});
}
