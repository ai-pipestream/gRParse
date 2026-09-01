#include "meta_repr.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "canonical_json_writer.h"
#include "markdown_text.h"
#include "renderer_base.h"
#include "value_repr.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {
namespace {

std::string optional_string_repr(bool present, const std::string& text) {
  return present ? python_string_repr(text) : "None";
}

std::string optional_double_repr(bool present, double value) {
  return present ? canonical_double(value) : "None";
}

// The pairs a prediction-shaped model prints before its own fields.
template <typename Message>
void append_prediction_repr(const Message& message,
                            std::vector<std::string>* pairs) {
  pairs->push_back("confidence=" + optional_double_repr(message.has_confidence(),
                                                        message.confidence()));
  pairs->push_back(
      "created_by=" + optional_string_repr(message.has_created_by(),
                                           message.created_by()));
}

template <typename Message>
void append_extras_repr(const Message& message, std::vector<std::string>* pairs) {
  for (const auto& [key, value] : ordered_custom_fields(message.custom_fields())) {
    pairs->push_back(key + "=" + value_repr(*value));
  }
}

// An enum member reprs as "<Type.MEMBER: 'value'>". The member name is the
// proto enum name without its vocabulary prefix.
std::string enum_repr(std::string_view type, std::string_view member,
                      std::string_view value) {
  return std::string("<").append(type).append(".").append(member).append(": '")
      .append(value)
      .append("'>");
}

std::string entity_mention_repr(const docv1::EntityMention& mention) {
  std::vector<std::string> pairs;
  append_prediction_repr(mention, &pairs);
  pairs.push_back("text=" + python_string_repr(mention.text()));
  pairs.push_back("orig=" + optional_string_repr(mention.has_orig(), mention.orig()));
  pairs.push_back("label=" +
                  optional_string_repr(mention.has_label(), mention.label()));
  pairs.push_back(mention.has_charspan()
                      ? "charspan=(" + std::to_string(mention.charspan().start()) +
                            ", " + std::to_string(mention.charspan().end()) + ")"
                      : "charspan=None");
  append_extras_repr(mention, &pairs);
  return "EntityMention(" + join(pairs, ", ") + ")";
}

}  // namespace

std::string language_meta_repr(const docv1::LanguageMetaField& meta) {
  // An unrepresentable code drops the whole field from the export, so there
  // is nothing for the reference to stringify.
  const auto code = human_language_string(meta.code());
  if (!code) return std::string();
  std::string member = *code;
  std::ranges::transform(member, member.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  std::vector<std::string> pairs;
  append_prediction_repr(meta, &pairs);
  pairs.push_back("code=" + enum_repr("HumanLanguageLabel", member, *code));
  append_extras_repr(meta, &pairs);
  return join(pairs, " ");
}

std::string entities_meta_repr(const docv1::EntitiesMetaField& meta) {
  // An empty mention list drops the whole field from the export.
  if (meta.mentions().empty()) return std::string();
  std::vector<std::string> mentions;
  mentions.reserve(static_cast<std::size_t>(meta.mentions_size()));
  for (const auto& mention : meta.mentions()) {
    mentions.push_back(entity_mention_repr(mention));
  }
  std::vector<std::string> pairs{"mentions=[" + join(mentions, ", ") + "]"};
  append_extras_repr(meta, &pairs);
  return join(pairs, " ");
}

std::string code_meta_repr(const docv1::CodeMetaField& meta) {
  std::vector<std::string> pairs;
  append_prediction_repr(meta, &pairs);
  pairs.push_back("text=" + python_string_repr(meta.text()));
  // The export writes the language only for a recognized tag or a raw
  // fallback, which collapses to the vocabulary's catch-all.
  const auto language = code_language_string(meta.language());
  if (language || !meta.language_raw().empty()) {
    std::string member = "UNKNOWN";
    if (language) {
      constexpr std::string_view prefix = "CODE_LANGUAGE_LABEL_";
      std::string name = docv1::CodeLanguageLabel_Name(meta.language());
      member = name.starts_with(prefix) ? name.substr(prefix.size()) : name;
    }
    pairs.push_back("language=" + enum_repr("CodeLanguageLabel", member,
                                            language.value_or("unknown")));
  } else {
    pairs.emplace_back("language=None");
  }
  append_extras_repr(meta, &pairs);
  return join(pairs, " ");
}

}  // namespace grparse::render
