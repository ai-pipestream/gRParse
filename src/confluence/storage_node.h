// The parse tree the storage handler builds and the accessors that read it.
// Internal to the storage handler; include/grparse/confluence_storage.h stays
// the only public surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_NODE_H
#define GRPARSE_CONFLUENCE_STORAGE_NODE_H

#include <string>
#include <string_view>
#include <vector>

namespace grparse::confluence {

struct Attribute {
  std::string prefix;
  std::string name;
  std::string value;
};

// One element or one run of character data. Namespace prefixes are kept as
// written ("ac", "ri", empty for the XHTML layer) rather than resolved
// against declarations: the dialect fixes both prefixes, and resolving would
// buy nothing a body in the wild does not already guarantee.
struct Node {
  bool text_node = false;
  std::string prefix;
  std::string name;
  std::string text;
  std::vector<Attribute> attributes;
  std::vector<Node> children;
};

bool element_is(const Node& node, std::string_view prefix, std::string_view name);

// True for an element of the XHTML layer with this local name.
bool html_is(const Node& node, std::string_view name);

// The attribute's value, or nullptr. A prefixed lookup also accepts the
// unprefixed spelling of the same local name: some producers drop the prefix
// on attributes, and the local names in this dialect do not collide.
const std::string* attribute(const Node& node, std::string_view prefix,
                             std::string_view name);

std::string attribute_or_empty(const Node& node, std::string_view prefix,
                               std::string_view name);

const Node* find_child(const Node& node, std::string_view prefix,
                       std::string_view name);

// Every character of the subtree, concatenated verbatim: what a plain-text
// macro body means, CDATA included.
std::string raw_text(const Node& node);

}  // namespace grparse::confluence

#endif
