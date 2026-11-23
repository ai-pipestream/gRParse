// Reading server settings from the environment, shared by the preset
// policies: trimmed values, list settings (comma-separated words, or the
// typed ServerSettingNameList in protobuf text format), and "unset or empty"
// lookups.
#ifndef GRPARSE_SETTING_VALUES_H
#define GRPARSE_SETTING_VALUES_H

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/string_view.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::settings {

namespace parsev1 = ai::pipestream::parse::v1;

inline std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::string();
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

// Keeps the first protobuf text format error, with its 1-based position.
class FirstTextFormatError final : public google::protobuf::io::ErrorCollector {
 public:
  void RecordError(int line, google::protobuf::io::ColumnNumber column,
                   absl::string_view message) override {
    if (!text_.empty()) return;
    text_ = "line " + std::to_string(line + 1) + " column " + std::to_string(column + 1) +
            ": " + std::string(message);
  }
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

// A list setting: comma-separated words (a,b,c), or a ServerSettingNameList
// in protobuf text format (names: "a" names: "b"), which a colon marks. A
// leading '[' is refused with the comma form shown: list settings are not
// bracketed arrays.
inline std::vector<std::string> parse_list(const char* variable, std::string_view value) {
  std::vector<std::string> items;
  const std::string text = trimmed(value);
  if (text.starts_with('[')) {
    throw std::invalid_argument(std::string(variable) +
                                " starts with '[', but list settings are not bracketed arrays: "
                                "write it comma-separated, such as a,b,c, or as a "
                                "ServerSettingNameList in protobuf text format, such as "
                                "names: \"a\" names: \"b\"");
  }
  if (text.contains(':')) {
    parsev1::ServerSettingNameList list;
    FirstTextFormatError errors;
    google::protobuf::TextFormat::Parser parser;
    parser.RecordErrorsTo(&errors);
    if (!parser.ParseFromString(text, &list)) {
      throw std::invalid_argument(std::string(variable) +
                                  " is not a ServerSettingNameList in protobuf text format: " +
                                  errors.text());
    }
    for (const std::string& entry : list.names()) {
      const std::string item = trimmed(entry);
      if (item.empty()) {
        throw std::invalid_argument(std::string(variable) + " has an empty entry");
      }
      items.push_back(item);
    }
    return items;
  }
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const std::string item =
        trimmed(std::string_view(text).substr(start, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - start));
    if (item.empty()) {
      throw std::invalid_argument(std::string(variable) + " has an empty entry");
    }
    items.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return items;
}

inline std::optional<std::string> configured(const std::function<const char*(const char*)>& lookup,
                                      const char* variable) {
  const char* value = lookup(variable);
  if (value == nullptr || trimmed(value).empty()) return std::nullopt;
  return std::string(value);
}

}  // namespace grparse::settings

#endif
