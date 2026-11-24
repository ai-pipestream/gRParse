#pragma once

// The float initializers of an ONNX model file, read straight from its
// protobuf wire format: ModelProto.graph (7) -> GraphProto.initializer (5) ->
// TensorProto name (8), dims (1), data_type (2), float_data (4) or raw_data
// (9).  Enough for a native runtime to take a model's weights from the file
// the models image already ships, without an ONNX library.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace grparse::levelzero {

struct OnnxTensor {
  std::vector<int64_t> dims;
  std::vector<float> values;
};

// Every FLOAT initializer of the model by name.  Throws naming the file on a
// malformed file or an initializer of another element type.
std::map<std::string, OnnxTensor> read_onnx_initializers(const std::filesystem::path& path);

// The same from the file's bytes, for tests.
std::map<std::string, OnnxTensor> parse_onnx_initializers(const std::string& bytes,
                                                         const std::string& what);

}  // namespace grparse::levelzero
