#pragma once

#include <string>

namespace grparse::trt {

// Gives every symbolic dimension named "?" in an ONNX model's graph inputs,
// outputs and value_info its own one-character name, in place (the same
// length, so no protobuf length changes). paddle2onnx names unknown height
// and width both "?", and TensorRT's ONNX parser takes equal names as equal
// dimensions: the PP-OCR detector then builds only for square inputs and
// the recognizer not at all. Returns how many dimensions were renamed.
// Throws std::invalid_argument for bytes that are not a well-formed
// protobuf on that path.
int name_unnamed_onnx_dims(std::string* model);

}  // namespace grparse::trt
