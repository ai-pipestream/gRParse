#pragma once

// The minimal ONNX model the session-builder tests load: ir_version 8, opset
// 13, one Identity from input "x" to output "y", hand-encoded protobuf so the
// fixture carries no generator dependency.

#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <unistd.h>

#include "support/check.h"

namespace grparse_test {

inline constexpr unsigned char kTinyModelBytes[] = {
    0x08, 0x08,                                           // ir_version = 8
    0x42, 0x02, 0x10, 0x0D,                               // opset_import: version 13
    0x3A, 0x37,                                           // graph, 55 bytes
    0x12, 0x01, 0x67,                                     // name = "g"
    0x0A, 0x10,                                           // node
    0x0A, 0x01, 0x78,                                     //   input "x"
    0x12, 0x01, 0x79,                                     //   output "y"
    0x22, 0x08, 0x49, 0x64, 0x65, 0x6E, 0x74, 0x69, 0x74, 0x79,  // op_type Identity
    0x5A, 0x0F, 0x0A, 0x01, 0x78,                         // input "x"
    0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x04, 0x0A, 0x02, 0x08, 0x01,                   //   shape [1]
    0x62, 0x0F, 0x0A, 0x01, 0x79,                         // output "y"
    0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x04, 0x0A, 0x02, 0x08, 0x01,                   //   shape [1]
};

// The same model with a free dimension: input and output shape ["n"], the
// way an exported model names a dynamic batch.
inline constexpr unsigned char kTinyFreeDimModelBytes[] = {
    0x08, 0x08,                                           // ir_version = 8
    0x42, 0x02, 0x10, 0x0D,                               // opset_import: version 13
    0x3A, 0x39,                                           // graph, 57 bytes
    0x12, 0x01, 0x67,                                     // name = "g"
    0x0A, 0x10,                                           // node
    0x0A, 0x01, 0x78,                                     //   input "x"
    0x12, 0x01, 0x79,                                     //   output "y"
    0x22, 0x08, 0x49, 0x64, 0x65, 0x6E, 0x74, 0x69, 0x74, 0x79,  // op_type Identity
    0x5A, 0x10, 0x0A, 0x01, 0x78,                         // input "x"
    0x12, 0x0B, 0x0A, 0x09, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x05, 0x0A, 0x03, 0x12, 0x01, 0x6E,             //   shape [dim_param "n"]
    0x62, 0x10, 0x0A, 0x01, 0x79,                         // output "y"
    0x12, 0x0B, 0x0A, 0x09, 0x08, 0x01,                   //   tensor elem_type float
    0x12, 0x05, 0x0A, 0x03, 0x12, 0x01, 0x6E,             //   shape [dim_param "n"]
};

// Writes `bytes` to a per-process temp file named after `stem`.
template <size_t N>
std::filesystem::path write_model_bytes(const unsigned char (&bytes)[N], std::string_view stem) {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("grparse-{}-{}.onnx", stem, ::getpid());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes), N);
  out.close();
  require(out.good(), "the tiny model fixture is written");
  return path;
}

// Writes the model to a per-process temp file and returns its path.
inline std::filesystem::path write_tiny_model() {
  return write_model_bytes(kTinyModelBytes, "tiny");
}

// The free-dimension variant, written the same way.
inline std::filesystem::path write_tiny_free_dim_model() {
  return write_model_bytes(kTinyFreeDimModelBytes, "tiny-free-dim");
}

}  // namespace grparse_test
