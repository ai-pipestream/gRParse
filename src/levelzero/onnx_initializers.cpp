#include "levelzero/onnx_initializers.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace grparse::levelzero {
namespace {

constexpr int kFloat = 1, kInt32 = 6, kInt64 = 7;

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

  std::string_view message() { return take(varint()); }

  float fixed32() {
    float value = 0;
    std::memcpy(&value, take(4).data(), 4);
    return value;
  }

  void skip(uint32_t type) {
    switch (type) {
      case 0: (void)varint(); return;
      case 1: (void)take(8); return;
      case 2: (void)take(varint()); return;
      case 5: (void)take(4); return;
      default: fail("an unsupported wire type");
    }
  }

  // The next field's (number, wire type).
  std::pair<uint32_t, uint32_t> key() {
    const uint64_t key = varint();
    return {static_cast<uint32_t>(key >> 3), static_cast<uint32_t>(key & 7)};
  }

  [[noreturn]] void fail(std::string_view why) const {
    throw std::runtime_error(what_ + ": not a readable ONNX model (" + std::string(why) + ")");
  }

  const std::string& what() const { return what_; }

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

// A varint field, packed (wire type 2) or one value (wire type 0).
void append_varints(Reader& reader, uint32_t type, std::vector<int64_t>* out) {
  if (type == 0) {
    out->push_back(static_cast<int64_t>(reader.varint()));
    return;
  }
  Reader packed(reader.message(), reader.what());
  while (!packed.done()) out->push_back(static_cast<int64_t>(packed.varint()));
}

OnnxTensor read_tensor(std::string_view message, const std::string& what, std::string* name) {
  Reader reader(message, what);
  OnnxTensor tensor;
  std::string raw;
  while (!reader.done()) {
    const auto [field, type] = reader.key();
    if (field == 1) {
      append_varints(reader, type, &tensor.dims);
    } else if (field == 2 && type == 0) {
      tensor.data_type = static_cast<int>(reader.varint());
    } else if (field == 4 && type == 2) {
      append_floats(reader.message(), &tensor.values, reader);
    } else if (field == 4 && type == 5) {
      tensor.values.push_back(reader.fixed32());
    } else if ((field == 5 || field == 7) && (type == 0 || type == 2)) {  // int32_data, int64_data
      append_varints(reader, type, &tensor.ints);
    } else if (field == 8 && type == 2) {
      *name = std::string(reader.message());
    } else if (field == 9 && type == 2) {
      raw = std::string(reader.message());
    } else {
      reader.skip(type);
    }
  }
  if (!raw.empty() && tensor.data_type == kFloat) append_floats(raw, &tensor.values, reader);
  if (!raw.empty() && (tensor.data_type == kInt64 || tensor.data_type == kInt32)) {
    const size_t width = tensor.data_type == kInt64 ? 8 : 4;
    if (raw.size() % width != 0) reader.fail("integer data of a partial element");
    for (size_t at = 0; at < raw.size(); at += width) {
      int64_t value = 0;
      if (width == 8) {
        std::memcpy(&value, raw.data() + at, 8);
      } else {
        int32_t narrow = 0;
        std::memcpy(&narrow, raw.data() + at, 4);
        value = narrow;
      }
      tensor.ints.push_back(value);
    }
  }
  size_t expected = 1;
  for (const int64_t dim : tensor.dims) expected *= static_cast<size_t>(dim);
  const size_t held = tensor.data_type == kFloat ? tensor.values.size() : tensor.ints.size();
  const bool typed = tensor.data_type == kFloat || tensor.data_type == kInt64 || tensor.data_type == kInt32;
  if (typed && held != expected) {
    reader.fail("tensor " + *name + " holds " + std::to_string(held) + " values for " +
                std::to_string(expected) + " elements");
  }
  return tensor;
}

std::string read_value_info_name(std::string_view message, const std::string& what) {
  Reader reader(message, what);
  while (!reader.done()) {
    const auto [field, type] = reader.key();
    if (field == 1 && type == 2) return std::string(reader.message());
    reader.skip(type);
  }
  return {};
}

void read_attribute(std::string_view message, const std::string& what, OnnxNode* node) {
  Reader reader(message, what);
  std::string name;
  OnnxAttribute attribute;
  while (!reader.done()) {
    const auto [field, type] = reader.key();
    if (field == 1 && type == 2) {
      name = std::string(reader.message());
    } else if (field == 2 && type == 5) {
      attribute.f = reader.fixed32();
    } else if (field == 3 && type == 0) {
      attribute.i = static_cast<int64_t>(reader.varint());
    } else if (field == 4 && type == 2) {
      attribute.s = std::string(reader.message());
    } else if (field == 5 && type == 2) {
      std::string unused;
      attribute.t = read_tensor(reader.message(), what, &unused);
    } else if (field == 7 && type == 2) {
      append_floats(reader.message(), &attribute.floats, reader);
    } else if (field == 7 && type == 5) {
      attribute.floats.push_back(reader.fixed32());
    } else if (field == 8 && (type == 0 || type == 2)) {
      append_varints(reader, type, &attribute.ints);
    } else {
      reader.skip(type);
    }
  }
  node->attributes[name] = std::move(attribute);
}

OnnxNode read_node(std::string_view message, const std::string& what) {
  Reader reader(message, what);
  OnnxNode node;
  while (!reader.done()) {
    const auto [field, type] = reader.key();
    if (field == 1 && type == 2) {
      node.inputs.emplace_back(reader.message());
    } else if (field == 2 && type == 2) {
      node.outputs.emplace_back(reader.message());
    } else if (field == 4 && type == 2) {
      node.op_type = std::string(reader.message());
    } else if (field == 5 && type == 2) {
      read_attribute(reader.message(), what, &node);
    } else {
      reader.skip(type);
    }
  }
  return node;
}

}  // namespace

const OnnxAttribute* OnnxNode::attribute(const std::string& name) const {
  const auto found = attributes.find(name);
  return found == attributes.end() ? nullptr : &found->second;
}

OnnxGraph parse_onnx_graph(const std::string& bytes, const std::string& what) {
  OnnxGraph out;
  Reader model(bytes, what);
  bool saw_graph = false;
  std::vector<std::string> declared_inputs;
  while (!model.done()) {
    const auto [field, type] = model.key();
    if (field != 7 || type != 2) {
      model.skip(type);
      continue;
    }
    saw_graph = true;
    Reader graph(model.message(), what);
    while (!graph.done()) {
      const auto [graph_field, graph_type] = graph.key();
      if (graph_field == 1 && graph_type == 2) {
        out.nodes.push_back(read_node(graph.message(), what));
      } else if (graph_field == 5 && graph_type == 2) {
        std::string name;
        OnnxTensor tensor = read_tensor(graph.message(), what, &name);
        out.constants[name] = std::move(tensor);
      } else if (graph_field == 11 && graph_type == 2) {
        declared_inputs.push_back(read_value_info_name(graph.message(), what));
      } else if (graph_field == 12 && graph_type == 2) {
        out.outputs.push_back(read_value_info_name(graph.message(), what));
      } else {
        graph.skip(graph_type);
      }
    }
  }
  if (!saw_graph) model.fail("no graph");
  for (const OnnxNode& node : out.nodes) {
    if (node.op_type != "Constant") continue;
    const OnnxAttribute* value = node.attribute("value");
    if (value == nullptr || !value->t || node.outputs.size() != 1) model.fail("a Constant without a tensor");
    out.constants[node.outputs[0]] = *value->t;
  }
  for (std::string& name : declared_inputs) {
    if (!out.constants.contains(name)) out.inputs.push_back(std::move(name));
  }
  return out;
}

OnnxGraph read_onnx_graph(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(path.string() + ": cannot be opened");
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return parse_onnx_graph(bytes, path.string());
}

std::map<std::string, OnnxTensor> parse_onnx_initializers(const std::string& bytes,
                                                         const std::string& what) {
  std::map<std::string, OnnxTensor> out;
  for (auto& [name, tensor] : parse_onnx_graph(bytes, what).constants) {
    if (tensor.data_type == kFloat) out.emplace(name, std::move(tensor));
  }
  return out;
}

std::map<std::string, OnnxTensor> read_onnx_initializers(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(path.string() + ": cannot be opened");
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return parse_onnx_initializers(bytes, path.string());
}

}  // namespace grparse::levelzero
