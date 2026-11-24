#include "levelzero/onnx_initializers.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace grparse::levelzero {
namespace {

constexpr int kFloat = 1;  // TensorProto.DataType.FLOAT

class Reader {
 public:
  Reader(std::string_view bytes, const std::string& what) : bytes_(bytes), what_(what) {}

  bool done() const { return at_ == bytes_.size(); }

  uint64_t varint() {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (done()) fail("a truncated varint");
      const auto byte = static_cast<uint8_t>(bytes_[at_++]);
      value |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) return value;
    }
    fail("an overlong varint");
  }

  std::string_view take(uint64_t size) {
    if (size > bytes_.size() - at_) fail("a field past the end of its message");
    const std::string_view out = bytes_.substr(at_, size);
    at_ += size;
    return out;
  }

  // Skips one field's value of wire type `type`.
  void skip(uint32_t type) {
    switch (type) {
      case 0: (void)varint(); return;
      case 1: (void)take(8); return;
      case 2: (void)take(varint()); return;
      case 5: (void)take(4); return;
      default: fail("an unsupported wire type");
    }
  }

  [[noreturn]] void fail(std::string_view why) const {
    throw std::runtime_error(what_ + ": not a readable ONNX model (" + std::string(why) + ")");
  }

 private:
  std::string_view bytes_;
  const std::string& what_;
  size_t at_ = 0;
};

void append_floats(std::string_view raw, std::vector<float>* out, const Reader& reader) {
  if (raw.size() % sizeof(float) != 0) reader.fail("float data whose size is not a multiple of 4");
  const size_t start = out->size();
  out->resize(start + raw.size() / sizeof(float));
  std::memcpy(out->data() + start, raw.data(), raw.size());  // little-endian, as ONNX stores it
}

void read_tensor(std::string_view message, const std::string& what,
                 std::map<std::string, OnnxTensor>* out) {
  Reader reader(message, what);
  std::string name;
  OnnxTensor tensor;
  int64_t data_type = 0;
  while (!reader.done()) {
    const uint64_t key = reader.varint();
    const auto field = static_cast<uint32_t>(key >> 3);
    const auto type = static_cast<uint32_t>(key & 7);
    if (field == 1 && type == 0) {
      tensor.dims.push_back(static_cast<int64_t>(reader.varint()));
    } else if (field == 1 && type == 2) {
      Reader packed(reader.take(reader.varint()), what);
      while (!packed.done()) tensor.dims.push_back(static_cast<int64_t>(packed.varint()));
    } else if (field == 2 && type == 0) {
      data_type = static_cast<int64_t>(reader.varint());
    } else if (field == 4 && type == 2) {
      append_floats(reader.take(reader.varint()), &tensor.values, reader);
    } else if (field == 4 && type == 5) {
      append_floats(reader.take(4), &tensor.values, reader);
    } else if (field == 8 && type == 2) {
      name = std::string(reader.take(reader.varint()));
    } else if (field == 9 && type == 2) {
      append_floats(reader.take(reader.varint()), &tensor.values, reader);
    } else {
      reader.skip(type);
    }
  }
  if (data_type != kFloat) return;  // shape constants and the like
  size_t expected = 1;
  for (const int64_t dim : tensor.dims) expected *= static_cast<size_t>(dim);
  if (tensor.values.size() != expected) {
    reader.fail("initializer " + name + " holds " + std::to_string(tensor.values.size()) +
                " floats for " + std::to_string(expected) + " elements");
  }
  (*out)[name] = std::move(tensor);
}

}  // namespace

std::map<std::string, OnnxTensor> parse_onnx_initializers(const std::string& bytes,
                                                         const std::string& what) {
  std::map<std::string, OnnxTensor> out;
  Reader model(bytes, what);
  bool saw_graph = false;
  while (!model.done()) {
    const uint64_t key = model.varint();
    const auto type = static_cast<uint32_t>(key & 7);
    if ((key >> 3) != 7 || type != 2) {
      model.skip(type);
      continue;
    }
    saw_graph = true;
    Reader graph(model.take(model.varint()), what);
    while (!graph.done()) {
      const uint64_t graph_key = graph.varint();
      const auto graph_type = static_cast<uint32_t>(graph_key & 7);
      if ((graph_key >> 3) == 5 && graph_type == 2) {
        read_tensor(graph.take(graph.varint()), what, &out);
      } else {
        graph.skip(graph_type);
      }
    }
  }
  if (!saw_graph) model.fail("no graph");
  return out;
}

std::map<std::string, OnnxTensor> read_onnx_initializers(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(path.string() + ": cannot be opened");
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return parse_onnx_initializers(bytes, path.string());
}

}  // namespace grparse::levelzero
