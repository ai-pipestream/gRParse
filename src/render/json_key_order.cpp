#include "json_key_order.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <utility>
#include <vector>

namespace grparse::render {

std::set<std::string> map_field_names(const google::protobuf::Descriptor* root) {
  std::set<std::string> names;
  std::set<const google::protobuf::Descriptor*> seen;
  std::vector<const google::protobuf::Descriptor*> pending{root};
  while (!pending.empty()) {
    const google::protobuf::Descriptor* descriptor = pending.back();
    pending.pop_back();
    if (descriptor == nullptr || !seen.insert(descriptor).second) continue;
    for (int i = 0; i < descriptor->field_count(); i++) {
      const google::protobuf::FieldDescriptor* field = descriptor->field(i);
      if (field->is_map()) {
        names.insert(std::string(field->name()));
        pending.push_back(field->message_type()->map_value()->message_type());
      } else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        pending.push_back(field->message_type());
      }
    }
  }
  return names;
}

namespace {

// A single pass over the text that copies every byte through and re-emits
// only the objects that need their members ordered.
class KeyOrderPass {
 public:
  KeyOrderPass(std::string_view json, const std::set<std::string>& map_fields)
      : json_(json), map_fields_(map_fields) {}

  std::string run() {
    std::string out;
    out.reserve(json_.size());
    skip_space();
    value(out, false);
    skip_space();
    if (pos_ != json_.size()) fail("trailing characters after the document");
    return out;
  }

 private:
  struct Member {
    std::string key;   // the raw key text, quotes included
    std::string text;  // the member as written: key, colon, value
  };

  [[noreturn]] void fail(const char* what) const {
    throw std::invalid_argument(std::string("JSON key order: ") + what + " at byte " +
                                std::to_string(pos_));
  }

  void skip_space() {
    while (pos_ < json_.size() && std::isspace(static_cast<unsigned char>(json_[pos_])) != 0) pos_++;
  }

  char peek() const { return pos_ < json_.size() ? json_[pos_] : '\0'; }

  // Copies a string literal, escapes included, and returns it.
  std::string string_literal() {
    const size_t start = pos_;
    if (peek() != '"') fail("expected a string");
    pos_++;
    while (pos_ < json_.size()) {
      const char c = json_[pos_++];
      if (c == '\\') {
        if (pos_ >= json_.size()) fail("unterminated escape");
        pos_++;
      } else if (c == '"') {
        return std::string(json_.substr(start, pos_ - start));
      }
    }
    fail("unterminated string");
  }

  void scalar(std::string& out) {
    const size_t start = pos_;
    while (pos_ < json_.size()) {
      const char c = json_[pos_];
      if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c)) != 0) break;
      pos_++;
    }
    if (pos_ == start) fail("expected a value");
    out.append(json_.substr(start, pos_ - start));
  }

  void array(std::string& out) {
    out.push_back('[');
    pos_++;
    skip_space();
    if (peek() == ']') {
      pos_++;
      out.push_back(']');
      return;
    }
    while (true) {
      skip_space();
      value(out, false);
      skip_space();
      const char c = peek();
      pos_++;
      if (c == ',') {
        out.push_back(',');
        continue;
      }
      if (c == ']') {
        out.push_back(']');
        return;
      }
      fail("expected ',' or ']' in an array");
    }
  }

  void object(std::string& out, bool sort_members) {
    pos_++;
    std::vector<Member> members;
    skip_space();
    if (peek() == '}') {
      pos_++;
      out.append("{}");
      return;
    }
    while (true) {
      skip_space();
      Member member;
      member.key = string_literal();
      member.text = member.key;
      skip_space();
      if (peek() != ':') fail("expected ':' after a key");
      pos_++;
      member.text.push_back(':');
      skip_space();
      const std::string bare_key = member.key.substr(1, member.key.size() - 2);
      value(member.text, map_fields_.contains(bare_key));
      members.push_back(std::move(member));
      skip_space();
      const char c = peek();
      pos_++;
      if (c == ',') continue;
      if (c == '}') break;
      fail("expected ',' or '}' in an object");
    }
    if (sort_members) order(members);
    out.push_back('{');
    for (size_t i = 0; i < members.size(); i++) {
      if (i > 0) out.push_back(',');
      out.append(members[i].text);
    }
    out.push_back('}');
  }

  static bool integer_key(const std::string& quoted, long long* value) {
    const std::string_view digits(quoted.data() + 1, quoted.size() - 2);
    if (digits.empty()) return false;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), *value);
    return error == std::errc() && end == digits.data() + digits.size();
  }

  static void order(std::vector<Member>& members) {
    std::vector<long long> numbers(members.size());
    bool all_integers = true;
    for (size_t i = 0; i < members.size() && all_integers; i++) {
      all_integers = integer_key(members[i].key, &numbers[i]);
    }
    if (all_integers) {
      std::vector<size_t> index(members.size());
      for (size_t i = 0; i < index.size(); i++) index[i] = i;
      std::stable_sort(index.begin(), index.end(),
                       [&](size_t a, size_t b) { return numbers[a] < numbers[b]; });
      std::vector<Member> ordered;
      ordered.reserve(members.size());
      for (const size_t i : index) ordered.push_back(std::move(members[i]));
      members = std::move(ordered);
      return;
    }
    std::stable_sort(members.begin(), members.end(),
                     [](const Member& a, const Member& b) { return a.key < b.key; });
  }

  void value(std::string& out, bool sort_members) {
    switch (peek()) {
      case '{': object(out, sort_members); return;
      case '[': array(out); return;
      case '"': out.append(string_literal()); return;
      default: scalar(out); return;
    }
  }

  std::string_view json_;
  const std::set<std::string>& map_fields_;
  size_t pos_ = 0;
};

}  // namespace

std::string sort_map_objects(std::string_view json, const std::set<std::string>& map_fields) {
  return KeyOrderPass(json, map_fields).run();
}

}  // namespace grparse::render
