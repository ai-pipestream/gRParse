// Reading server settings from the environment, shared by the preset
// policies: trimmed values, list settings (a JSON array of strings read as
// the typed ServerSettingNameList, or comma-separated words, as
// docling-serve accepts), and "unset or empty" lookups.
#ifndef GRPARSE_SETTING_VALUES_H
#define GRPARSE_SETTING_VALUES_H

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::settings {

namespace parsev1 = ai::pipestream::parse::v1;

inline std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::string();
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

// A list setting: a JSON array of strings (read as the typed
// ServerSettingNameList), or comma-separated words.
inline std::vector<std::string> parse_list(const char* variable, std::string_view value) {
  std::vector<std::string> items;
  const std::string text = trimmed(value);
  if (text.starts_with('[')) {
    parsev1::ServerSettingNameList list;
    if (!google::protobuf::util::JsonStringToMessage("{\"names\":" + text + "}", &list).ok()) {
      throw std::invalid_argument(std::string(variable) +
                                  " is not a JSON array of non-empty strings");
    }
    for (const std::string& entry : list.names()) {
      if (trimmed(entry).empty()) {
        throw std::invalid_argument(std::string(variable) +
                                    " is not a JSON array of non-empty strings");
      }
      items.push_back(trimmed(entry));
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
