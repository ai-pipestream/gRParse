#include "grparse/confluence_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "ai/pipestream/document/v1/document.pb.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

// The attribution every item this handler creates carries. "native" says the
// parse ran in this process, with no model and no remote collector behind it.
constexpr char kCollector[] = "confluence-storage";
constexpr char kModel[] = "native";

// The grid ceiling the office fold uses: a declared row/column product above
// it keeps the placed cells and skips the materialized grid, so a bogus
// header can never allocate an arbitrary rectangle.
constexpr int kMaxGridCells = 4096;

// Spans are clamped to the table's own extent; anything larger is malformed
// markup, and the clamp keeps offsets inside the grid it describes. The
// column ceiling bounds the placement walk itself, so a body full of wide
// column spans cannot grow the row occupancy without limit.
constexpr int kMaxSpan = 1024;
constexpr int kMaxColumns = 4096;

std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char letter) {
    return static_cast<char>(std::tolower(letter));
  });
  return value;
}

std::string uppercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char letter) {
    return static_cast<char>(std::toupper(letter));
  });
  return value;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_space(char letter) {
  return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r' ||
         letter == '\f' || letter == '\v';
}

bool blank(std::string_view text) {
  return std::ranges::all_of(text, is_space);
}

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) ++begin;
  size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

// Characters, not bytes: the character spans the Document plane carries are
// counted in code points everywhere else, so they are here too.
long long code_points(std::string_view text) {
  long long count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) ++count;
  }
  return count;
}

google::protobuf::Value str_value(const std::string& text) {
  google::protobuf::Value value;
  value.set_string_value(text);
  return value;
}

google::protobuf::Value num_value(double number) {
  google::protobuf::Value value;
  value.set_number_value(number);
  return value;
}

void append_code_point(uint32_t code_point, std::string* out) {
  // Lone surrogates and out-of-range values have no encoding; the reference
  // is kept verbatim by the caller instead of being replaced by a guess.
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

// Resolves the entity references the dialect uses: the five XML predefined
// ones, the numeric forms, and the non-breaking space that authoring tools
// emit constantly. Anything else stays verbatim, because a reference this
// parser does not know is still the author's text.
std::string decode_entities(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  size_t index = 0;
  while (index < raw.size()) {
    if (raw[index] != '&') {
      out.push_back(raw[index++]);
      continue;
    }
    const size_t semicolon = raw.find(';', index + 1);
    if (semicolon == std::string_view::npos || semicolon - index > 16) {
      out.push_back(raw[index++]);
      continue;
    }
    const std::string_view name = raw.substr(index + 1, semicolon - index - 1);
    bool resolved = true;
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (name == "nbsp") {
      append_code_point(0x00A0, &out);
    } else if (name.size() > 1 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const std::string_view digits = name.substr(hex ? 2 : 1);
      uint32_t code_point = 0;
      resolved = !digits.empty();
      for (const char digit : digits) {
        const int value = std::isdigit(static_cast<unsigned char>(digit)) != 0
                              ? digit - '0'
                          : (hex && std::isxdigit(static_cast<unsigned char>(digit)) != 0)
                              ? std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10
                              : -1;
        if (value < 0) {
          resolved = false;
          break;
        }
        code_point = code_point * (hex ? 16 : 10) + static_cast<uint32_t>(value);
        if (code_point > 0x10FFFF) {
          resolved = false;
          break;
        }
      }
      resolved = resolved && code_point != 0 &&
                 !(code_point >= 0xD800 && code_point <= 0xDFFF);
      if (resolved) append_code_point(code_point, &out);
    } else {
      resolved = false;
    }
    if (!resolved) {
      out.push_back(raw[index++]);
      continue;
    }
    index = semicolon + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

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

bool element_is(const Node& node, std::string_view prefix, std::string_view name) {
  return !node.text_node && node.prefix == prefix && node.name == name;
}

// True for an element of the XHTML layer with this local name.
bool html_is(const Node& node, std::string_view name) {
  return element_is(node, "", name);
}

// The attribute's value, or nullptr. A prefixed lookup also accepts the
// unprefixed spelling of the same local name: some producers drop the prefix
// on attributes, and the local names in this dialect do not collide.
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

// Every character of the subtree, concatenated verbatim: what a plain-text
// macro body means, CDATA included.
void append_raw_text(const Node& node, std::string* out) {
  if (node.text_node) {
    out->append(node.text);
    return;
  }
  for (const Node& child : node.children) append_raw_text(child, out);
}

std::string raw_text(const Node& node) {
  std::string out;
  append_raw_text(node, &out);
  return out;
}

// ---------------------------------------------------------------------------
// The pull parser
// ---------------------------------------------------------------------------

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
  bool starts_with(std::string_view token) const {
    return input_.compare(position_, token.size(), token) == 0;
  }
  void skip_space() {
    while (position_ < input_.size() && is_space(input_[position_])) ++position_;
  }
  // Reads a qualified name at the cursor into prefix and local name.
  void read_name(std::string* prefix, std::string* name);
  void read_attributes(Node* node, bool* self_closing, bool* terminated);
  void warn(std::string message) {
    if (warnings_ != nullptr) warnings_->push_back(std::move(message));
  }

  std::string_view input_;
  std::vector<std::string>* warnings_ = nullptr;
  size_t position_ = 0;
  bool saw_element_ = false;
};

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

Node StorageParser::parse() {
  Node root;
  root.name = "#document";
  // Only the innermost open element is ever appended to, so the ancestor
  // pointers on this stack stay valid across the appends below.
  std::vector<Node*> open{&root};
  const auto top = [&open]() -> Node* { return open.back(); };

  while (position_ < input_.size()) {
    if (input_[position_] != '<') {
      const size_t begin = position_;
      while (position_ < input_.size() && input_[position_] != '<') ++position_;
      Node text;
      text.text_node = true;
      text.text = decode_entities(input_.substr(begin, position_ - begin));
      top()->children.push_back(std::move(text));
      continue;
    }
    if (starts_with("<!--")) {
      const size_t end = input_.find("-->", position_ + 4);
      if (end == std::string_view::npos) {
        warn("a comment ran to the end of the body without closing");
        position_ = input_.size();
        continue;
      }
      position_ = end + 3;
      continue;
    }
    if (starts_with("<![CDATA[")) {
      const size_t end = input_.find("]]>", position_ + 9);
      const size_t begin = position_ + 9;
      if (end == std::string_view::npos) {
        warn("a CDATA section ran to the end of the body without closing");
        Node text;
        text.text_node = true;
        text.text = std::string(input_.substr(begin));
        top()->children.push_back(std::move(text));
        position_ = input_.size();
        continue;
      }
      Node text;
      text.text_node = true;
      text.text = std::string(input_.substr(begin, end - begin));
      top()->children.push_back(std::move(text));
      position_ = end + 3;
      continue;
    }
    if (starts_with("<?")) {
      const size_t end = input_.find("?>", position_ + 2);
      position_ = end == std::string_view::npos ? input_.size() : end + 2;
      continue;
    }
    if (starts_with("<!")) {
      // A doctype, internal subset included: the subset is bracketed, so the
      // scan ends at the bracket's own close when there is one.
      size_t end = input_.find('>', position_ + 2);
      const size_t bracket = input_.find('[', position_ + 2);
      if (bracket != std::string_view::npos && (end == std::string_view::npos || bracket < end)) {
        const size_t close = input_.find(']', bracket + 1);
        end = close == std::string_view::npos ? std::string_view::npos
                                              : input_.find('>', close + 1);
      }
      position_ = end == std::string_view::npos ? input_.size() : end + 1;
      continue;
    }
    if (starts_with("</")) {
      position_ += 2;
      std::string prefix;
      std::string name;
      read_name(&prefix, &name);
      const size_t end = input_.find('>', position_);
      position_ = end == std::string_view::npos ? input_.size() : end + 1;
      size_t depth = open.size();
      while (depth > 1 && !(open[depth - 1]->prefix == prefix &&
                            open[depth - 1]->name == name)) {
        --depth;
      }
      if (depth == 1) {
        warn("end tag </" + (prefix.empty() ? name : prefix + ":" + name) +
             "> closes nothing that is open; it was ignored");
        continue;
      }
      if (depth != open.size()) {
        warn("end tag </" + (prefix.empty() ? name : prefix + ":" + name) +
             "> closed " + std::to_string(open.size() - depth) +
             " element(s) that had no end tag of their own");
      }
      // depth indexes the matched element itself, so the resize drops it
      // along with anything still open inside it.
      open.resize(depth - 1);
      continue;
    }
    ++position_;
    Node element;
    read_name(&element.prefix, &element.name);
    if (element.name.empty()) {
      // Not a tag after all; keep the character as text.
      Node text;
      text.text_node = true;
      text.text = "<";
      top()->children.push_back(std::move(text));
      continue;
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
    if (element.prefix.empty() && std::ranges::find(kVoid, element.name) !=
                                      std::end(kVoid)) {
      self_closing = true;
    }
    top()->children.push_back(std::move(element));
    if (!self_closing) open.push_back(&top()->children.back());
  }
  if (open.size() > 1) {
    warn(std::to_string(open.size() - 1) +
         " element(s) were still open at the end of the body");
  }
  return root;
}

// ---------------------------------------------------------------------------
// The fold
// ---------------------------------------------------------------------------

// The inline state one run of characters was written in.
struct InlineStyle {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  docv1::Script script = docv1::SCRIPT_UNSPECIFIED;
  std::string hyperlink;
};

bool same_formatting(const InlineStyle& left, const InlineStyle& right) {
  return left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline &&
         left.strikethrough == right.strikethrough && left.script == right.script;
}

bool plain_formatting(const InlineStyle& style) {
  return !style.bold && !style.italic && !style.underline &&
         !style.strikethrough && style.script == docv1::SCRIPT_UNSPECIFIED;
}

struct InlineRun {
  std::string text;
  InlineStyle style;
};

// The block constructs of the dialect, plus the XHTML block elements a page
// body can carry. Anything else is inline until it turns out to contain a
// block, at which point it is a transparent container.
bool named_block(const Node& node) {
  if (node.text_node) return false;
  if (node.prefix == "ac") {
    return node.name == "structured-macro" || node.name == "task-list" ||
           node.name == "image" || node.name == "layout" ||
           node.name == "layout-section" || node.name == "layout-cell" ||
           node.name == "adf-extension" || node.name == "adf-node";
  }
  if (!node.prefix.empty()) return false;
  // A line break is deliberately absent: it belongs to the text around it,
  // not beside it, so it stays inline and folds into the item as a newline.
  static constexpr std::string_view kBlocks[] = {
      "h1",    "h2",     "h3",     "h4",   "h5",  "h6",     "p",
      "ul",    "ol",     "li",     "table", "thead", "tbody", "tfoot",
      "tr",    "td",     "th",     "div",  "pre", "hr",     "blockquote",
      "section", "article", "aside", "header", "footer", "main", "nav",
      "figure", "figcaption", "dl", "dt", "dd"};
  return std::ranges::find(kBlocks, node.name) != std::end(kBlocks);
}

bool is_block(const Node& node) {
  if (named_block(node)) return true;
  if (node.text_node) return false;
  return std::ranges::any_of(node.children,
                             [](const Node& child) { return is_block(child); });
}

// True when a block element wraps another block: such a paragraph cannot be
// read as a run of inline text without losing what it wraps.
bool contains_block(const Node& node) {
  return std::ranges::any_of(node.children,
                             [](const Node& child) { return is_block(child); });
}

class StorageFold {
 public:
  StorageFold(docv1::Document* document, std::vector<std::string>* warnings)
      : document_(document), warnings_(warnings) {}

  // Walks a container's children: inline content accumulates into one text
  // item, block content is emitted where it appears.
  void fold_blocks(const Node& container, const std::string& parent_ref);

  int emitted() const { return emitted_; }

 private:
  struct TextHandle {
    docv1::BaseTextItem* item = nullptr;
    docv1::TextItemBase* base = nullptr;
    std::string ref;
  };

  enum class TextKind { kSectionHeader, kList, kText };

  // A meta stamp (panel or macro name) that every item created inside the
  // scope carries, unwound when the scope ends.
  class StampScope {
   public:
    StampScope(StorageFold* fold, std::string key, std::string value)
        : fold_(fold) {
      if (value.empty()) {
        fold_ = nullptr;
        return;
      }
      fold_->stamps_.emplace_back(std::move(key), std::move(value));
    }
    ~StampScope() {
      if (fold_ != nullptr) fold_->stamps_.pop_back();
    }
    StampScope(const StampScope&) = delete;
    StampScope& operator=(const StampScope&) = delete;

   private:
    StorageFold* fold_;
  };

  docv1::GroupItem* group_by_ref(const std::string& ref);
  void link_child(const std::string& parent_ref, const std::string& child_ref);
  void stamp_source(
      google::protobuf::RepeatedPtrField<docv1::SourceType>* source);
  template <typename Meta>
  void stamp_meta(Meta* meta);

  docv1::GroupItem* add_group(const std::string& parent_ref,
                              docv1::GroupLabel label, const std::string& name);
  TextHandle add_text(TextKind kind, docv1::DocItemLabel label,
                      const std::string& parent_ref);

  void collect_inline_node(const Node& node, InlineStyle style,
                           std::vector<InlineRun>* runs);
  void collect_inline_children(const Node& node, const InlineStyle& style,
                               std::vector<InlineRun>* runs);
  void collect_link(const Node& node, InlineStyle style,
                    std::vector<InlineRun>* runs);
  void apply_inline(const std::vector<InlineRun>& runs, docv1::TextItemBase* base);
  void flush_inline(std::vector<InlineRun>* runs, const std::string& parent_ref);

  void emit_block(const Node& node, const std::string& parent_ref);
  void emit_heading(const Node& node, int level, const std::string& parent_ref);
  void emit_paragraph(const Node& node, const std::string& parent_ref);
  void emit_list(const Node& node, const std::string& parent_ref, bool ordered);
  void emit_list_item(const Node& node, const std::string& group_ref,
                      bool ordered, int position);
  void emit_task_list(const Node& node, const std::string& parent_ref);
  void emit_table(const Node& node, const std::string& parent_ref);
  void emit_macro(const Node& node, const std::string& parent_ref);
  void emit_code_macro(const Node& node, const std::string& parent_ref);
  void emit_image(const Node& node, const std::string& parent_ref);
  void warn(std::string message) {
    if (warnings_ != nullptr) warnings_->push_back(std::move(message));
  }

  docv1::Document* document_ = nullptr;
  std::vector<std::string>* warnings_ = nullptr;
  std::vector<std::pair<std::string, std::string>> stamps_;
  int emitted_ = 0;
};

docv1::GroupItem* StorageFold::group_by_ref(const std::string& ref) {
  if (ref == "#/furniture") return document_->mutable_furniture();
  static constexpr std::string_view kPrefix = "#/groups/";
  if (ref.starts_with(kPrefix)) {
    const int index = std::stoi(ref.substr(kPrefix.size()));
    if (index >= 0 && index < document_->groups_size()) {
      return document_->mutable_groups(index);
    }
  }
  return document_->mutable_body();
}

void StorageFold::link_child(const std::string& parent_ref,
                             const std::string& child_ref) {
  group_by_ref(parent_ref)->add_children()->set_ref(child_ref);
}

void StorageFold::stamp_source(
    google::protobuf::RepeatedPtrField<docv1::SourceType>* source) {
  docv1::CollectorSource* collector = source->Add()->mutable_collector();
  collector->set_collector(kCollector);
  collector->set_model(kModel);
}

template <typename Meta>
void StorageFold::stamp_meta(Meta* meta) {
  for (const auto& [key, value] : stamps_) {
    (*meta->mutable_custom_fields())[key] = str_value(value);
  }
}

docv1::GroupItem* StorageFold::add_group(const std::string& parent_ref,
                                         docv1::GroupLabel label,
                                         const std::string& name) {
  const int index = document_->groups_size();
  docv1::GroupItem* group = document_->add_groups();
  group->set_self_ref("#/groups/" + std::to_string(index));
  group->mutable_parent()->set_ref(parent_ref);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  if (!name.empty()) group->set_name(name);
  link_child(parent_ref, group->self_ref());
  return group;
}

StorageFold::TextHandle StorageFold::add_text(TextKind kind,
                                              docv1::DocItemLabel label,
                                              const std::string& parent_ref) {
  TextHandle handle;
  handle.ref = "#/texts/" + std::to_string(document_->texts_size());
  handle.item = document_->add_texts();
  switch (kind) {
    case TextKind::kSectionHeader:
      handle.base = handle.item->mutable_section_header()->mutable_base();
      break;
    case TextKind::kList:
      handle.base = handle.item->mutable_list_item()->mutable_base();
      break;
    case TextKind::kText:
      handle.base = handle.item->mutable_text()->mutable_base();
      break;
  }
  handle.base->set_self_ref(handle.ref);
  handle.base->mutable_parent()->set_ref(parent_ref);
  handle.base->set_label(label);
  handle.base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(handle.base->mutable_source());
  if (!stamps_.empty()) stamp_meta(handle.base->mutable_meta());
  link_child(parent_ref, handle.ref);
  ++emitted_;
  return handle;
}

void StorageFold::collect_inline_children(const Node& node,
                                          const InlineStyle& style,
                                          std::vector<InlineRun>* runs) {
  for (const Node& child : node.children) collect_inline_node(child, style, runs);
}

void StorageFold::collect_link(const Node& node, InlineStyle style,
                               std::vector<InlineRun>* runs) {
  // The dialect names link targets by resource rather than by URL. What has
  // no URL keeps a pointer in the same shape the attachment pointers use, so
  // a consumer can resolve it later; nothing is invented.
  std::string label;
  if (const Node* page = find_child(node, "ri", "page")) {
    label = attribute_or_empty(*page, "ri", "content-title");
    if (!label.empty()) style.hyperlink = "confluence-page:" + label;
  } else if (const Node* attachment = find_child(node, "ri", "attachment")) {
    label = attribute_or_empty(*attachment, "ri", "filename");
    if (!label.empty()) style.hyperlink = "confluence-attachment:" + label;
  } else if (const Node* url = find_child(node, "ri", "url")) {
    label = attribute_or_empty(*url, "ri", "value");
    style.hyperlink = label;
  } else if (const Node* space = find_child(node, "ri", "space")) {
    label = attribute_or_empty(*space, "ri", "space-key");
    if (!label.empty()) style.hyperlink = "confluence-space:" + label;
  }
  const Node* plain_body = find_child(node, "ac", "plain-text-link-body");
  const Node* rich_body = find_child(node, "ac", "link-body");
  if (plain_body != nullptr) {
    const std::string text = raw_text(*plain_body);
    if (!text.empty()) runs->push_back({text, style});
    return;
  }
  if (rich_body != nullptr) {
    collect_inline_children(*rich_body, style, runs);
    return;
  }
  // A bodiless link renders as its target's own name.
  if (!label.empty()) runs->push_back({label, style});
}

void StorageFold::collect_inline_node(const Node& node, InlineStyle style,
                                      std::vector<InlineRun>* runs) {
  if (node.text_node) {
    if (!node.text.empty()) runs->push_back({node.text, style});
    return;
  }
  if (node.prefix.empty()) {
    if (node.name == "strong" || node.name == "b") {
      style.bold = true;
    } else if (node.name == "em" || node.name == "i") {
      style.italic = true;
    } else if (node.name == "u" || node.name == "ins") {
      style.underline = true;
    } else if (node.name == "s" || node.name == "del" || node.name == "strike") {
      style.strikethrough = true;
    } else if (node.name == "sub") {
      style.script = docv1::SCRIPT_SUB;
    } else if (node.name == "sup") {
      style.script = docv1::SCRIPT_SUPER;
    } else if (node.name == "a") {
      const std::string href = attribute_or_empty(node, "", "href");
      if (!href.empty()) style.hyperlink = href;
    } else if (node.name == "br") {
      runs->push_back({"\n", style});
      return;
    }
    collect_inline_children(node, style, runs);
    return;
  }
  if (node.prefix == "ac" && node.name == "link") {
    collect_link(node, std::move(style), runs);
    return;
  }
  // Every other inline macro (inline comment markers, emoticons, placeholders)
  // contributes whatever text it wraps.
  collect_inline_children(node, style, runs);
}

// Drops empty runs and trims the outer whitespace of the item's text; interior
// spacing between runs is the author's and stays.
void trim_runs(std::vector<InlineRun>* runs) {
  std::erase_if(*runs, [](const InlineRun& run) { return run.text.empty(); });
  if (runs->empty()) return;
  std::string& first = runs->front().text;
  const std::string_view trimmed_first = trim(std::string_view(first));
  first.erase(0, static_cast<size_t>(trimmed_first.data() - first.data()));
  std::string& last = runs->back().text;
  size_t end = last.size();
  while (end > 0 && is_space(last[end - 1])) --end;
  last.resize(end);
  std::erase_if(*runs, [](const InlineRun& run) { return run.text.empty(); });
}

std::string runs_text(const std::vector<InlineRun>& runs) {
  std::string text;
  for (const InlineRun& run : runs) text.append(run.text);
  return text;
}

void StorageFold::apply_inline(const std::vector<InlineRun>& runs,
                               docv1::TextItemBase* base) {
  const std::string text = runs_text(runs);
  base->set_text(text);
  base->set_orig(text);
  if (runs.empty()) return;

  // Item-level formatting is only honest when every run agrees; mixed text
  // keeps its formatting unset, exactly as the office fold does.
  const InlineStyle& first = runs.front().style;
  const bool uniform = std::ranges::all_of(runs, [&first](const InlineRun& run) {
    return same_formatting(run.style, first);
  });
  if (uniform && !plain_formatting(first)) {
    docv1::Formatting* formatting = base->mutable_formatting();
    formatting->set_bold(first.bold);
    formatting->set_italic(first.italic);
    formatting->set_underline(first.underline);
    formatting->set_strikethrough(first.strikethrough);
    formatting->set_script(first.script);
  }

  // The first link lands in the docling hyperlink slot; every link keeps its
  // own character span in the "hyperlinks" custom field.
  google::protobuf::ListValue links;
  long long local = 0;
  long long start = 0;
  std::string url;
  const auto flush = [&links, &url, &start, &local]() {
    if (url.empty()) return;
    google::protobuf::Struct* link = links.add_values()->mutable_struct_value();
    (*link->mutable_fields())["url"] = str_value(url);
    (*link->mutable_fields())["char_start"] = num_value(static_cast<double>(start));
    (*link->mutable_fields())["char_end"] = num_value(static_cast<double>(local));
    url.clear();
  };
  for (const InlineRun& run : runs) {
    if (run.style.hyperlink != url) {
      flush();
      url = run.style.hyperlink;
      start = local;
    }
    local += code_points(run.text);
  }
  flush();
  if (links.values_size() == 0) return;
  base->set_hyperlink(
      links.values(0).struct_value().fields().at("url").string_value());
  google::protobuf::Value value;
  *value.mutable_list_value() = std::move(links);
  (*base->mutable_meta()->mutable_custom_fields())["hyperlinks"] = std::move(value);
}

void StorageFold::flush_inline(std::vector<InlineRun>* runs,
                               const std::string& parent_ref) {
  trim_runs(runs);
  if (runs->empty()) return;
  const TextHandle handle =
      add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, parent_ref);
  apply_inline(*runs, handle.base);
  runs->clear();
}

void StorageFold::fold_blocks(const Node& container,
                              const std::string& parent_ref) {
  std::vector<InlineRun> pending;
  for (const Node& child : container.children) {
    if (is_block(child)) {
      flush_inline(&pending, parent_ref);
      emit_block(child, parent_ref);
      continue;
    }
    collect_inline_node(child, InlineStyle{}, &pending);
  }
  flush_inline(&pending, parent_ref);
}

void StorageFold::emit_block(const Node& node, const std::string& parent_ref) {
  if (node.prefix == "ac") {
    if (node.name == "structured-macro") {
      emit_macro(node, parent_ref);
      return;
    }
    if (node.name == "task-list") {
      emit_task_list(node, parent_ref);
      return;
    }
    if (node.name == "image") {
      emit_image(node, parent_ref);
      return;
    }
    // Layout wrappers and unmapped container macros are transparent.
    fold_blocks(node, parent_ref);
    return;
  }
  if (!node.prefix.empty()) {
    fold_blocks(node, parent_ref);
    return;
  }
  if (node.name.size() == 2 && node.name[0] == 'h' && node.name[1] >= '1' &&
      node.name[1] <= '6') {
    emit_heading(node, node.name[1] - '0', parent_ref);
    return;
  }
  if (node.name == "p") {
    emit_paragraph(node, parent_ref);
    return;
  }
  if (node.name == "ul") {
    emit_list(node, parent_ref, false);
    return;
  }
  if (node.name == "ol") {
    emit_list(node, parent_ref, true);
    return;
  }
  if (node.name == "table") {
    emit_table(node, parent_ref);
    return;
  }
  // A horizontal rule separates without carrying anything to map.
  if (node.name == "hr") return;
  // Every other XHTML element is a transparent container: its blocks are
  // emitted where they stand and its loose text becomes a text item.
  fold_blocks(node, parent_ref);
}

void StorageFold::emit_heading(const Node& node, int level,
                               const std::string& parent_ref) {
  std::vector<InlineRun> runs;
  collect_inline_children(node, InlineStyle{}, &runs);
  trim_runs(&runs);
  if (runs.empty()) return;
  const TextHandle handle = add_text(
      TextKind::kSectionHeader, docv1::DOC_ITEM_LABEL_SECTION_HEADER, parent_ref);
  handle.item->mutable_section_header()->set_level(level);
  apply_inline(runs, handle.base);
}

void StorageFold::emit_paragraph(const Node& node,
                                 const std::string& parent_ref) {
  // A paragraph can wrap a block (an image or a macro is routinely written
  // inside one); those keep their own item and the loose text keeps its own.
  fold_blocks(node, parent_ref);
}

void StorageFold::emit_list(const Node& node, const std::string& parent_ref,
                            bool ordered) {
  docv1::GroupItem* group = add_group(
      parent_ref,
      ordered ? docv1::GROUP_LABEL_ORDERED_LIST : docv1::GROUP_LABEL_LIST,
      "list");
  const std::string group_ref = group->self_ref();
  int position = 0;
  for (const Node& child : node.children) {
    if (html_is(child, "li")) {
      emit_list_item(child, group_ref, ordered, ++position);
      continue;
    }
    if (html_is(child, "ul") || html_is(child, "ol")) {
      // A sublist written beside the items instead of inside one still
      // nests: the group is the list's own child.
      emit_list(child, group_ref, html_is(child, "ol"));
      continue;
    }
    if (child.text_node) {
      if (!blank(child.text)) {
        std::vector<InlineRun> runs{{child.text, InlineStyle{}}};
        flush_inline(&runs, group_ref);
      }
      continue;
    }
    emit_block(child, group_ref);
  }
}

void StorageFold::emit_list_item(const Node& node, const std::string& group_ref,
                                 bool ordered, int position) {
  std::vector<InlineRun> runs;
  std::vector<const Node*> blocks;
  for (const Node& child : node.children) {
    // The item's own text is the text of the paragraphs written directly in
    // it; a paragraph that wraps a block is a block, or the block it wraps
    // would be flattened into text and lost.
    if (html_is(child, "p") && !contains_block(child)) {
      std::vector<InlineRun> paragraph;
      collect_inline_children(child, InlineStyle{}, &paragraph);
      trim_runs(&paragraph);
      if (paragraph.empty()) continue;
      if (!runs.empty()) runs.push_back({"\n", InlineStyle{}});
      runs.insert(runs.end(), std::make_move_iterator(paragraph.begin()),
                  std::make_move_iterator(paragraph.end()));
      continue;
    }
    if (is_block(child)) {
      blocks.push_back(&child);
      continue;
    }
    collect_inline_node(child, InlineStyle{}, &runs);
  }
  trim_runs(&runs);
  const TextHandle handle =
      add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM, group_ref);
  apply_inline(runs, handle.base);
  docv1::ListItem* item = handle.item->mutable_list_item();
  item->set_enumerated(ordered);
  item->set_marker(ordered ? std::to_string(position) + "." : "-");
  // Nested lists and any other block the item carries hang off the list
  // group, so the item stays a leaf and the nesting stays a group tree.
  for (const Node* block : blocks) emit_block(*block, group_ref);
}

void StorageFold::emit_task_list(const Node& node,
                                 const std::string& parent_ref) {
  docv1::GroupItem* group =
      add_group(parent_ref, docv1::GROUP_LABEL_LIST, "task-list");
  const std::string group_ref = group->self_ref();
  for (const Node& child : node.children) {
    if (!element_is(child, "ac", "task")) continue;
    const Node* status = find_child(child, "ac", "task-status");
    const bool complete =
        status != nullptr && lowercase(std::string(trim(raw_text(*status)))) == "complete";
    std::vector<InlineRun> runs;
    if (const Node* body = find_child(child, "ac", "task-body")) {
      collect_inline_children(*body, InlineStyle{}, &runs);
    }
    trim_runs(&runs);
    const TextHandle handle =
        add_text(TextKind::kList,
                 complete ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                          : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED,
                 group_ref);
    apply_inline(runs, handle.base);
    docv1::ListItem* item = handle.item->mutable_list_item();
    item->set_enumerated(false);
    item->set_marker("-");
  }
}

// One row of a parsed table and whether it sits in the header section.
struct TableRowNode {
  const Node* row = nullptr;
  bool head = false;
};

void collect_rows(const Node& node, bool head, std::vector<TableRowNode>* rows) {
  for (const Node& child : node.children) {
    if (html_is(child, "tr")) {
      rows->push_back({&child, head});
      continue;
    }
    if (html_is(child, "thead")) {
      collect_rows(child, true, rows);
      continue;
    }
    if (html_is(child, "tbody") || html_is(child, "tfoot")) {
      collect_rows(child, false, rows);
      continue;
    }
    if (!child.text_node) collect_rows(child, head, rows);
  }
}

int span_attribute(const Node& cell, std::string_view name, bool* clamped) {
  const std::string* raw = attribute(cell, "", name);
  if (raw == nullptr) return 1;
  int value = 0;
  for (const char digit : *raw) {
    if (std::isdigit(static_cast<unsigned char>(digit)) == 0) return 1;
    value = value * 10 + (digit - '0');
    if (value > kMaxSpan) {
      *clamped = true;
      return kMaxSpan;
    }
  }
  return value > 0 ? value : 1;
}

void StorageFold::emit_table(const Node& node, const std::string& parent_ref) {
  std::vector<TableRowNode> rows;
  collect_rows(node, false, &rows);

  const std::string ref = "#/tables/" + std::to_string(document_->tables_size());
  docv1::TableItem* table = document_->add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent_ref);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(table->mutable_source());
  if (!stamps_.empty()) stamp_meta(table->mutable_meta());
  link_child(parent_ref, ref);
  ++emitted_;

  const int num_rows = static_cast<int>(rows.size());
  docv1::TableData* data = table->mutable_data();
  data->set_num_rows(num_rows);
  if (num_rows == 0) {
    data->set_num_cols(0);
    return;
  }

  // The placement walk of the HTML table model: a cell takes the first free
  // column of its row, and its spans reserve the slots below and to the
  // right for the rows that follow.
  std::vector<std::vector<bool>> occupied(static_cast<size_t>(num_rows));
  int num_cols = 0;
  bool clamped = false;
  for (int row = 0; row < num_rows; ++row) {
    int column = 0;
    for (const Node& cell : rows[static_cast<size_t>(row)].row->children) {
      if (!html_is(cell, "td") && !html_is(cell, "th")) continue;
      auto& slots = occupied[static_cast<size_t>(row)];
      while (column < static_cast<int>(slots.size()) &&
             slots[static_cast<size_t>(column)]) {
        ++column;
      }
      int row_span = span_attribute(cell, "rowspan", &clamped);
      int col_span = span_attribute(cell, "colspan", &clamped);
      if (row + row_span > num_rows) {
        row_span = num_rows - row;
        clamped = true;
      }
      if (column >= kMaxColumns) {
        // Past the ceiling there is no slot left to place into; the cell's
        // text would need a column that cannot be addressed.
        clamped = true;
        break;
      }
      if (column + col_span > kMaxColumns) {
        col_span = kMaxColumns - column;
        clamped = true;
      }
      for (int r = row; r < row + row_span; ++r) {
        auto& target = occupied[static_cast<size_t>(r)];
        if (target.size() < static_cast<size_t>(column + col_span)) {
          target.resize(static_cast<size_t>(column + col_span), false);
        }
        for (int c = column; c < column + col_span; ++c) {
          target[static_cast<size_t>(c)] = true;
        }
      }

      std::vector<InlineRun> runs;
      for (const Node& part : cell.children) {
        if (html_is(part, "p")) {
          std::vector<InlineRun> paragraph;
          collect_inline_children(part, InlineStyle{}, &paragraph);
          trim_runs(&paragraph);
          if (paragraph.empty()) continue;
          if (!runs.empty()) runs.push_back({"\n", InlineStyle{}});
          runs.insert(runs.end(), std::make_move_iterator(paragraph.begin()),
                      std::make_move_iterator(paragraph.end()));
          continue;
        }
        collect_inline_node(part, InlineStyle{}, &runs);
      }
      trim_runs(&runs);

      const bool header_cell = html_is(cell, "th");
      docv1::TableCell* out = data->add_table_cells();
      out->set_start_row_offset_idx(row);
      out->set_end_row_offset_idx(row + row_span);
      out->set_start_col_offset_idx(column);
      out->set_end_col_offset_idx(column + col_span);
      out->set_row_span(row_span);
      out->set_col_span(col_span);
      out->set_text(runs_text(runs));
      out->set_column_header(header_cell &&
                             (rows[static_cast<size_t>(row)].head || row == 0));
      out->set_row_header(header_cell && !out->column_header());
      column += col_span;
      num_cols = std::max(num_cols, column);
    }
  }
  data->set_num_cols(num_cols);
  if (clamped) {
    warn("a table cell declared a span beyond the table and was clamped to it");
  }

  // The materialized grid, under the same ceiling the office fold uses: a
  // spanning cell fills every slot it covers, and nothing writes outside the
  // rectangle the header declared.
  if (num_cols <= 0 ||
      static_cast<long long>(num_rows) * num_cols > kMaxGridCells) {
    return;
  }
  for (int row = 0; row < num_rows; ++row) {
    docv1::TableRow* out_row = data->add_grid();
    for (int column = 0; column < num_cols; ++column) {
      docv1::TableCell* slot = out_row->add_cells();
      slot->set_start_row_offset_idx(row);
      slot->set_end_row_offset_idx(row + 1);
      slot->set_start_col_offset_idx(column);
      slot->set_end_col_offset_idx(column + 1);
      slot->set_row_span(1);
      slot->set_col_span(1);
    }
  }
  for (const docv1::TableCell& cell : data->table_cells()) {
    for (int row = cell.start_row_offset_idx(); row < cell.end_row_offset_idx();
         ++row) {
      if (row < 0 || row >= data->grid_size()) continue;
      docv1::TableRow* out_row = data->mutable_grid(row);
      for (int column = cell.start_col_offset_idx();
           column < cell.end_col_offset_idx(); ++column) {
        if (column < 0 || column >= out_row->cells_size()) continue;
        *out_row->mutable_cells(column) = cell;
      }
    }
  }
}

void StorageFold::emit_macro(const Node& node, const std::string& parent_ref) {
  const std::string name = lowercase(attribute_or_empty(node, "ac", "name"));
  if (name == "code") {
    emit_code_macro(node, parent_ref);
    return;
  }
  // The panel family is a container with a body, not a construct of its own:
  // the body maps to whatever it contains, and the macro's own name rides
  // along on the items so nothing has to invent a label for it.
  static constexpr std::string_view kPanels[] = {"info", "note", "warning",
                                                 "tip", "panel"};
  const bool panel = std::ranges::find(kPanels, name) != std::end(kPanels);
  StampScope scope(this, panel ? "panel" : "macro", name);

  bool folded = false;
  for (const Node& child : node.children) {
    if (element_is(child, "ac", "rich-text-body")) {
      fold_blocks(child, parent_ref);
      folded = true;
      continue;
    }
    if (element_is(child, "ac", "plain-text-body")) {
      const std::string text = raw_text(child);
      if (!blank(text)) {
        std::vector<InlineRun> runs{{text, InlineStyle{}}};
        flush_inline(&runs, parent_ref);
      }
      folded = true;
    }
  }
  if (!folded) {
    // A bodiless macro (a table of contents, a status lozenge) carries only
    // its parameters, which are configuration rather than page content.
    warn("macro '" + (name.empty() ? std::string("unnamed") : name) +
         "' carried no body; its parameters were not mapped");
  }
}

void StorageFold::emit_code_macro(const Node& node,
                                  const std::string& parent_ref) {
  std::string language;
  for (const Node& child : node.children) {
    if (element_is(child, "ac", "parameter") &&
        lowercase(attribute_or_empty(child, "ac", "name")) == "language") {
      language = std::string(trim(raw_text(child)));
    }
  }
  std::string text;
  if (const Node* body = find_child(node, "ac", "plain-text-body")) {
    text = raw_text(*body);
  }

  const std::string ref = "#/texts/" + std::to_string(document_->texts_size());
  docv1::CodeItem* code = document_->add_texts()->mutable_code();
  code->set_self_ref(ref);
  code->mutable_parent()->set_ref(parent_ref);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_text(text);
  code->set_orig(text);
  stamp_source(code->mutable_source());
  if (!stamps_.empty()) stamp_meta(code->mutable_meta());
  link_child(parent_ref, ref);
  ++emitted_;

  if (language.empty()) return;
  // The declared language maps by enum name, case-insensitively and with "-"
  // read as "_". Anything the schema has no value for keeps its raw string;
  // the mapping never guesses a neighbour.
  docv1::CodeLanguageLabel label = docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED;
  std::string wanted = "CODE_LANGUAGE_LABEL_" + uppercase(language);
  std::ranges::replace(wanted, '-', '_');
  if (docv1::CodeLanguageLabel_Parse(wanted, &label) &&
      label != docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED) {
    code->set_code_language(label);
  } else {
    code->set_code_language_raw(language);
  }
}

void StorageFold::emit_image(const Node& node, const std::string& parent_ref) {
  // An attachment is a pointer, never bytes: the handler has the page body
  // and nothing else, so it names the resource and stops there.
  std::string uri;
  if (const Node* attachment = find_child(node, "ri", "attachment")) {
    const std::string filename = attribute_or_empty(*attachment, "ri", "filename");
    if (!filename.empty()) uri = "confluence-attachment:" + filename;
  } else if (const Node* url = find_child(node, "ri", "url")) {
    uri = attribute_or_empty(*url, "ri", "value");
  }

  const std::string ref =
      "#/pictures/" + std::to_string(document_->pictures_size());
  docv1::PictureItem* picture = document_->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent_ref);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  stamp_source(picture->mutable_source());
  if (!stamps_.empty()) stamp_meta(picture->mutable_meta());
  link_child(parent_ref, ref);
  ++emitted_;
  if (!uri.empty()) picture->mutable_image()->set_uri(uri);
  const std::string alt = attribute_or_empty(node, "ac", "alt");
  if (!alt.empty()) {
    (*picture->mutable_meta()->mutable_custom_fields())["alt"] = str_value(alt);
  }
}

// The parse proper. The exported entry point wraps it so that no failure
// mode, allocation included, leaves this handler by exception: a collector
// reports its failures as outcomes.
CollectorOutcome parse_storage_body(const std::string& bytes) {
  CollectorOutcome outcome;
  docv1::Document& document = outcome.document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  StorageParser parser(bytes, &outcome.warnings);
  const Node root = parser.parse();
  if (!parser.saw_element()) {
    outcome.warnings.clear();
    outcome.error =
        "confluence-storage: the body carries no markup, so it is not a "
        "storage-format document";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  StorageFold fold(&document, &outcome.warnings);
  fold.fold_blocks(root, "#/body");
  if (fold.emitted() == 0) {
    // Markup with nothing in it is a real page state (a stub, a page whose
    // only content is an unmapped bodiless macro); it parses, and says so.
    outcome.warnings.push_back(
        "the storage body carried markup but no mappable content");
  }
  outcome.success = true;
  return outcome;
}

}  // namespace

bool confluence_storage_format(const std::string& filename,
                               const std::string& content_type) {
  const std::string name = lowercase(filename);
  const std::string type = lowercase(content_type);
  return ends_with(name, ".confluence") || ends_with(name, ".storage.xhtml") ||
         type == kConfluenceStorageMimetype;
}

CollectorOutcome parse_confluence_storage(const std::string& bytes) {
  CollectorOutcome outcome;
  try {
    return parse_storage_body(bytes);
  } catch (const std::bad_alloc&) {
    outcome.error = "confluence-storage: the body did not fit in memory";
    outcome.code = grpc::StatusCode::RESOURCE_EXHAUSTED;
  } catch (const std::exception& failure) {
    outcome.error = std::string("confluence-storage: ") + failure.what();
    outcome.code = grpc::StatusCode::INTERNAL;
  }
  return outcome;
}

}  // namespace grparse
