#pragma once

// The minimal ONNX model the session-builder tests load: ir_version 8, opset
// 13, one Identity from input "x" to output "y", hand-encoded protobuf so the
// fixture carries no generator dependency.

#include <filesystem>
#include <format>
#include <fstream>
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

// Writes the model to a per-process temp file and returns its path.
inline std::filesystem::path write_tiny_model() {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("grparse-tiny-{}.onnx", ::getpid());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kTinyModelBytes), sizeof(kTinyModelBytes));
  out.close();
  require(out.good(), "the tiny model fixture is written");
  return path;
}

}  // namespace grparse_test
