#pragma once

// An ONNX model file read straight from its protobuf wire format, enough for
// a native runtime to take a model's weights and graph from the file the
// models image already ships, without an ONNX library: the graph's nodes
// (op type, inputs, outputs, attributes), its initializers and its Constant
// nodes' tensors, FLOAT, INT64 and INT32 elements.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace grparse::levelzero {

struct OnnxTensor {
  std::vector<int64_t> dims;
  int data_type = 0;            // TensorProto.DataType: 1 FLOAT, 6 INT32, 7 INT64
  std::vector<float> values;    // FLOAT
  std::vector<int64_t> ints;    // INT64 and INT32
};

struct OnnxAttribute {
  float f = 0.0F;
  int64_t i = 0;
  std::string s;
  std::vector<float> floats;
  std::vector<int64_t> ints;
  std::optional<OnnxTensor> t;
};

struct OnnxNode {
  std::string op_type;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::map<std::string, OnnxAttribute> attributes;

  // The attribute, or nullptr.
  const OnnxAttribute* attribute(const std::string& name) const;
};

struct OnnxGraph {
  std::vector<OnnxNode> nodes;  // Constant nodes included, in file order
  std::map<std::string, OnnxTensor> constants;  // initializers and Constant outputs
  std::vector<std::string> inputs;   // graph inputs that are not initializers
  std::vector<std::string> outputs;
};

// The model's graph.  Throws naming the file on a malformed file.
OnnxGraph read_onnx_graph(const std::filesystem::path& path);
OnnxGraph parse_onnx_graph(const std::string& bytes, const std::string& what);

// Every FLOAT tensor of the model (initializers and Constant outputs) by name.
std::map<std::string, OnnxTensor> read_onnx_initializers(const std::filesystem::path& path);
std::map<std::string, OnnxTensor> parse_onnx_initializers(const std::string& bytes,
                                                         const std::string& what);

}  // namespace grparse::levelzero
