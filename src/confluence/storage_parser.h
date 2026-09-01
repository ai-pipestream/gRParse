// The pull parser for the wiki storage dialect. Internal to the storage
// handler; include/grparse/confluence_storage.h stays the only public
// surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_PARSER_H
#define GRPARSE_CONFLUENCE_STORAGE_PARSER_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "storage_node.h"

namespace grparse::confluence {

// A single pass over the body: comments, processing instructions and the
// doctype are skipped, CDATA is taken verbatim, everything else becomes a
// node. Damage is recoverable by design (a stray or missing end tag closes
// what it can and warns) because half a page is worth more than none.
class StorageParser {
 public:
  StorageParser(std::string_view input, std::vector<std::string>* warnings)
      : input_(input), warnings_(warnings) {}

  Node parse();

  // True once any element has been seen: a body without one is not markup.
  bool saw_element() const { return saw_element_; }

 private:
  // The elements still open, innermost last; the root is always element 0.
  using OpenStack = std::vector<Node*>;

  bool starts_with(std::string_view token) const {
    return input_.compare(position_, token.size(), token) == 0;
  }
  void skip_space();
  // Reads a qualified name at the cursor into prefix and local name.
  void read_name(std::string* prefix, std::string* name);
  void read_attributes(Node* node, bool* self_closing, bool* terminated);

  // One construct each, cursor left on the character after it.
  void read_text_run(OpenStack* open);
  void skip_comment();
  void read_cdata(OpenStack* open);
  void skip_instruction();
  void skip_doctype();
  void close_element(OpenStack* open);
  void read_element(OpenStack* open);

  void warn(std::string message) {
    if (warnings_ != nullptr) warnings_->push_back(std::move(message));
  }

  std::string_view input_;
  std::vector<std::string>* warnings_ = nullptr;
  size_t position_ = 0;
  bool saw_element_ = false;
};

}  // namespace grparse::confluence

#endif
