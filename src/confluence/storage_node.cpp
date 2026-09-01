#include "storage_node.h"

#include <string>
#include <string_view>

namespace grparse::confluence {
namespace {

void append_raw_text(const Node& node, std::string* out) {
  if (node.text_node) {
    out->append(node.text);
    return;
  }
  for (const Node& child : node.children) append_raw_text(child, out);
}

}  // namespace

bool element_is(const Node& node, std::string_view prefix, std::string_view name) {
  return !node.text_node && node.prefix == prefix && node.name == name;
}

bool html_is(const Node& node, std::string_view name) {
  return element_is(node, "", name);
}

const std::string* attribute(const Node& node, std::string_view prefix,
                             std::string_view name) {
  const std::string* fallback = nullptr;
  for (const Attribute& candidate : node.attributes) {
    if (candidate.name != name) continue;
    if (candidate.prefix == prefix) return &candidate.value;
    if (candidate.prefix.empty()) fallback = &candidate.value;
  }
  return fallback;
}

std::string attribute_or_empty(const Node& node, std::string_view prefix,
                               std::string_view name) {
  const std::string* value = attribute(node, prefix, name);
  return value != nullptr ? *value : std::string();
}

const Node* find_child(const Node& node, std::string_view prefix,
                       std::string_view name) {
  for (const Node& child : node.children) {
    if (element_is(child, prefix, name)) return &child;
  }
  return nullptr;
}

std::string raw_text(const Node& node) {
  std::string out;
  append_raw_text(node, &out);
  return out;
}

}  // namespace grparse::confluence
