// ONNX model bytes written by hand, for tests of the reader
// (src/levelzero/onnx_initializers.h) and of the graph lowering
// (src/levelzero/graph_ir.h): the protobuf wire encoding of the few
// messages those read, so a test states a graph in a dozen lines and needs
// no model file.  Header only, and test-only.
#ifndef GRPARSE_TESTS_SUPPORT_ONNX_BYTES_H
#define GRPARSE_TESTS_SUPPORT_ONNX_BYTES_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace grparse_test::onnx {

inline std::string varint(uint64_t value) {
  std::string out;
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value != 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (value != 0);
  return out;
}

inline std::string field(uint32_t number, uint32_t type) { return varint((uint64_t(number) << 3) | type); }

inline std::string bytes_field(uint32_t number, const std::string& value) {
  return field(number, 2) + varint(value.size()) + value;
}

inline std::string float_bytes(const std::vector<float>& values) {
  std::string out(values.size() * sizeof(float), '\0');
  std::memcpy(out.data(), values.data(), out.size());
  return out;
}

// A TensorProto of FLOAT: dims (1), data_type (2) = 1, name (8), raw_data (9).
inline std::string float_tensor(const std::string& name, const std::vector<int64_t>& dims,
                                const std::vector<float>& values) {
  std::string out;
  for (const int64_t dim : dims) out += field(1, 0) + varint(static_cast<uint64_t>(dim));
  return out + field(2, 0) + varint(1) + bytes_field(8, name) + bytes_field(9, float_bytes(values));
}

// A TensorProto of INT64: data_type (2) = 7, int64_data (7), packed.
inline std::string int64_tensor(const std::string& name, const std::vector<int64_t>& dims,
                                const std::vector<int64_t>& values) {
  std::string out, packed;
  for (const int64_t dim : dims) out += field(1, 0) + varint(static_cast<uint64_t>(dim));
  for (const int64_t value : values) packed += varint(static_cast<uint64_t>(value));
  return out + field(2, 0) + varint(7) + bytes_field(8, name) + bytes_field(7, packed);
}

// AttributeProtos: name (1), then f (2), i (3), s (4) or ints (8).
inline std::string float_attribute(const std::string& name, float value) {
  std::string bits(sizeof(float), '\0');
  std::memcpy(bits.data(), &value, sizeof(float));
  return bytes_field(1, name) + field(2, 5) + bits;
}
inline std::string int_attribute(const std::string& name, int64_t value) {
  return bytes_field(1, name) + field(3, 0) + varint(static_cast<uint64_t>(value));
}
inline std::string string_attribute(const std::string& name, const std::string& value) {
  return bytes_field(1, name) + bytes_field(4, value);
}
inline std::string ints_attribute(const std::string& name, const std::vector<int64_t>& values) {
  std::string out = bytes_field(1, name);
  for (const int64_t value : values) out += field(8, 0) + varint(static_cast<uint64_t>(value));
  return out;
}

// A NodeProto: inputs (1), outputs (2), op_type (4), attributes (5).
inline std::string node(const std::string& op, const std::vector<std::string>& inputs, const std::string& output,
                        const std::vector<std::string>& attributes = {}) {
  std::string out;
  for (const std::string& input : inputs) out += bytes_field(1, input);
  out += bytes_field(2, output) + bytes_field(4, op);
  for (const std::string& attribute : attributes) out += bytes_field(5, attribute);
  return out;
}

// A ModelProto of initializers and nodes alone: ir_version (1), graph (7).
inline std::string model_with(const std::vector<std::string>& tensors, const std::vector<std::string>& nodes = {}) {
  std::string graph;
  for (const std::string& one : nodes) graph += bytes_field(1, one);
  for (const std::string& tensor : tensors) graph += bytes_field(5, tensor);
  return field(1, 0) + varint(8) + bytes_field(7, graph);
}

// A whole ModelProto: the graph's nodes (1) in order, initializers (5),
// inputs (11) and outputs (12) by name.
inline std::string model(const std::vector<std::string>& nodes, const std::vector<std::string>& tensors,
                         const std::vector<std::string>& inputs, const std::vector<std::string>& outputs) {
  std::string graph;
  for (const std::string& one : nodes) graph += bytes_field(1, one);
  for (const std::string& tensor : tensors) graph += bytes_field(5, tensor);
  for (const std::string& input : inputs) graph += bytes_field(11, bytes_field(1, input));
  for (const std::string& output : outputs) graph += bytes_field(12, bytes_field(1, output));
  return field(1, 0) + varint(8) + bytes_field(7, graph);
}

}  // namespace grparse_test::onnx

#endif  // GRPARSE_TESTS_SUPPORT_ONNX_BYTES_H
