// name_unnamed_onnx_dims: walks ModelProto.graph (7) -> input/output/
// value_info (11, 12, 13) -> type (2) -> tensor_type (1) -> shape (2) ->
// dim (1) -> dim_param (2) with a minimal protobuf reader and rewrites a
// one-byte "?" in place.

#include "onnx_dim_names.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace grparse::trt {

namespace {

struct Field {
  uint32_t number = 0;
  uint32_t wire = 0;
  size_t value = 0;  // start of the payload
  size_t end = 0;    // one past the field
};

uint64_t read_varint(const std::string& b, size_t* at, size_t end) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (*at >= end) throw std::invalid_argument("ONNX model: truncated varint");
    const auto byte = static_cast<uint8_t>(b[(*at)++]);
    v |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) return v;
  }
  throw std::invalid_argument("ONNX model: varint too long");
}

Field next_field(const std::string& b, size_t* at, size_t end) {
  Field f;
  const uint64_t key = read_varint(b, at, end);
  f.number = static_cast<uint32_t>(key >> 3);
  f.wire = static_cast<uint32_t>(key & 7);
  f.value = *at;
  switch (f.wire) {
    case 0: read_varint(b, at, end); break;
    case 1: *at += 8; break;
    case 5: *at += 4; break;
    case 2: {
      const uint64_t length = read_varint(b, at, end);
      f.value = *at;
      if (length > end - *at) throw std::invalid_argument("ONNX model: field runs past its message");
      *at += static_cast<size_t>(length);
      break;
    }
    default: throw std::invalid_argument("ONNX model: unsupported wire type");
  }
  if (*at > end) throw std::invalid_argument("ONNX model: truncated field");
  f.end = *at;
  return f;
}

// Calls visit(field) for each length-delimited field `number` of the
// message spanning [begin, end).
template <typename Visit>
void each(const std::string& b, size_t begin, size_t end, uint32_t number, Visit visit) {
  for (size_t at = begin; at < end;) {
    const Field f = next_field(b, &at, end);
    if (f.wire == 2 && f.number == number) visit(f);
  }
}

constexpr char kNames[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

}  // namespace

int name_unnamed_onnx_dims(std::string* model) {
  std::string& b = *model;
  int renamed = 0;
  each(b, 0, b.size(), 7, [&](const Field& graph) {
    for (const uint32_t list : {11U, 12U, 13U}) {
      each(b, graph.value, graph.end, list, [&](const Field& info) {
        each(b, info.value, info.end, 2, [&](const Field& type) {
          each(b, type.value, type.end, 1, [&](const Field& tensor) {
            each(b, tensor.value, tensor.end, 2, [&](const Field& shape) {
              each(b, shape.value, shape.end, 1, [&](const Field& dim) {
                each(b, dim.value, dim.end, 2, [&](const Field& param) {
                  if (param.end - param.value != 1 || b[param.value] != '?') return;
                  if (renamed >= static_cast<int>(sizeof(kNames) - 1))
                    throw std::invalid_argument("ONNX model: too many unnamed dimensions");
                  b[param.value] = kNames[renamed++];
                });
              });
            });
          });
        });
      });
    }
  });
  return renamed;
}

}  // namespace grparse::trt
