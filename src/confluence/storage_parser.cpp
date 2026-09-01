#include "storage_parser.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "storage_node.h"
#include "storage_text.h"

namespace grparse::confluence {
namespace {

// The qualified spelling a warning names an element by.
std::string qualified_name(const std::string& prefix, const std::string& name) {
  return prefix.empty() ? name : prefix + ":" + name;
}

void append_text(std::vector<Node*>* open, std::string text) {
  Node node;
  node.text_node = true;
  node.text = std::move(text);
  open->back()->children.push_back(std::move(node));
}

}  // namespace

void StorageParser::skip_space() {
  while (position_ < input_.size() && is_space(input_[position_])) ++position_;
}

void StorageParser::read_name(std::string* prefix, std::string* name) {
  const size_t begin = position_;
  while (position_ < input_.size()) {
    const char letter = input_[position_];
    if (is_space(letter) || letter == '>' || letter == '/' || letter == '=') break;
    ++position_;
  }
  std::string_view qualified = input_.substr(begin, position_ - begin);
  const size_t colon = qualified.find(':');
  if (colon == std::string_view::npos) {
    *prefix = std::string();
    *name = lowercase(std::string(qualified));
    return;
  }
  *prefix = lowercase(std::string(qualified.substr(0, colon)));
  *name = lowercase(std::string(qualified.substr(colon + 1)));
}

void StorageParser::read_attributes(Node* node, bool* self_closing,
                                    bool* terminated) {
  *self_closing = false;
  *terminated = false;
  while (position_ < input_.size()) {
    skip_space();
    if (position_ >= input_.size()) break;
    if (input_[position_] == '>') {
      ++position_;
      *terminated = true;
      return;
    }
    if (input_[position_] == '/') {
      ++position_;
      skip_space();
      if (position_ < input_.size() && input_[position_] == '>') {
        ++position_;
        *self_closing = true;
        *terminated = true;
      }
      return;
    }
    Attribute attribute;
    read_name(&attribute.prefix, &attribute.name);
    if (attribute.name.empty() && attribute.prefix.empty()) {
      // No progress is possible on this character; drop it so a malformed
      // tag cannot spin here.
      ++position_;
      continue;
    }
    skip_space();
    if (position_ < input_.size() && input_[position_] == '=') {
      ++position_;
      skip_space();
      if (position_ < input_.size() &&
          (input_[position_] == '"' || input_[position_] == '\'')) {
        const char quote = input_[position_++];
        const size_t begin = position_;
        while (position_ < input_.size() && input_[position_] != quote) ++position_;
        attribute.value = decode_entities(input_.substr(begin, position_ - begin));
        if (position_ < input_.size()) ++position_;
      } else {
        const size_t begin = position_;
        while (position_ < input_.size() && !is_space(input_[position_]) &&
               input_[position_] != '>' && input_[position_] != '/') {
          ++position_;
        }
        attribute.value = decode_entities(input_.substr(begin, position_ - begin));
      }
    }
    node->attributes.push_back(std::move(attribute));
  }
}

void StorageParser::read_text_run(OpenStack* open) {
  const size_t begin = position_;
  while (position_ < input_.size() && input_[position_] != '<') ++position_;
  append_text(open, decode_entities(input_.substr(begin, position_ - begin)));
}

void StorageParser::skip_comment() {
  const size_t end = input_.find("-->", position_ + 4);
  if (end == std::string_view::npos) {
    warn("a comment ran to the end of the body without closing");
    position_ = input_.size();
    return;
  }
  position_ = end + 3;
}

void StorageParser::read_cdata(OpenStack* open) {
  const size_t begin = position_ + 9;
  const size_t end = input_.find("]]>", begin);
  if (end == std::string_view::npos) {
    warn("a CDATA section ran to the end of the body without closing");
    append_text(open, std::string(input_.substr(begin)));
    position_ = input_.size();
    return;
  }
  append_text(open, std::string(input_.substr(begin, end - begin)));
  position_ = end + 3;
}

void StorageParser::skip_instruction() {
  const size_t end = input_.find("?>", position_ + 2);
  position_ = end == std::string_view::npos ? input_.size() : end + 2;
}

void StorageParser::skip_doctype() {
  // A doctype, internal subset included: the subset is bracketed, so the
  // scan ends at the bracket's own close when there is one.
  size_t end = input_.find('>', position_ + 2);
  const size_t bracket = input_.find('[', position_ + 2);
  if (bracket != std::string_view::npos &&
      (end == std::string_view::npos || bracket < end)) {
    const size_t close = input_.find(']', bracket + 1);
    end = close == std::string_view::npos ? std::string_view::npos
                                          : input_.find('>', close + 1);
  }
  position_ = end == std::string_view::npos ? input_.size() : end + 1;
}

void StorageParser::close_element(OpenStack* open) {
  position_ += 2;
  std::string prefix;
  std::string name;
  read_name(&prefix, &name);
  const size_t end = input_.find('>', position_);
  position_ = end == std::string_view::npos ? input_.size() : end + 1;
  size_t depth = open->size();
  while (depth > 1 && !((*open)[depth - 1]->prefix == prefix &&
                        (*open)[depth - 1]->name == name)) {
    --depth;
  }
  if (depth == 1) {
    warn("end tag </" + qualified_name(prefix, name) +
         "> closes nothing that is open; it was ignored");
    return;
  }
  if (depth != open->size()) {
    warn("end tag </" + qualified_name(prefix, name) + "> closed " +
         std::to_string(open->size() - depth) +
         " element(s) that had no end tag of their own");
  }
  // depth indexes the matched element itself, so the resize drops it along
  // with anything still open inside it.
  open->resize(depth - 1);
}

void StorageParser::read_element(OpenStack* open) {
  ++position_;
  Node element;
  read_name(&element.prefix, &element.name);
  if (element.name.empty()) {
    // Not a tag after all; keep the character as text.
    append_text(open, "<");
    return;
  }
  bool self_closing = false;
  bool terminated = false;
  read_attributes(&element, &self_closing, &terminated);
  saw_element_ = true;
  if (!terminated) {
    warn("start tag <" + element.name + "> ran to the end of the body");
    self_closing = true;
  }
  // The XHTML void elements never carry an end tag; treating them as open
  // would swallow the rest of the body.
  static constexpr std::string_view kVoid[] = {"br", "hr",  "img",  "input",
                                               "col", "area", "base", "meta"};
  if (element.prefix.empty() &&
      std::ranges::find(kVoid, element.name) != std::end(kVoid)) {
    self_closing = true;
  }
  open->back()->children.push_back(std::move(element));
  if (!self_closing) open->push_back(&open->back()->children.back());
}

Node StorageParser::parse() {
  Node root;
  root.name = "#document";
  // Only the innermost open element is ever appended to, so the ancestor
  // pointers on this stack stay valid across the appends below.
  OpenStack open{&root};

  while (position_ < input_.size()) {
    if (input_[position_] != '<') {
      read_text_run(&open);
    } else if (starts_with("<!--")) {
      skip_comment();
    } else if (starts_with("<![CDATA[")) {
      read_cdata(&open);
    } else if (starts_with("<?")) {
      skip_instruction();
    } else if (starts_with("<!")) {
      skip_doctype();
    } else if (starts_with("</")) {
      close_element(&open);
    } else {
      read_element(&open);
    }
  }
  if (open.size() > 1) {
    warn(std::to_string(open.size() - 1) +
         " element(s) were still open at the end of the body");
  }
  return root;
}

}  // namespace grparse::confluence
