// The Markdown renderer behind render_markdown; semantics documented on the
// declaration in include/grparse/document_render.h.
//
// The walk is a port of the reference Markdown serializer: a pre-order pass
// over the body tree where each node is serialized once (a group consumes its
// subtree, everything else leaves its children to the pass), block results
// join with a blank line, and list groups join with a single newline plus a
// four-space indent per nesting level. The reference is driven with its own
// default parameters throughout:
//
//   body content layer only, the export label vocabulary, no page slicing,
//   formatting and hyperlinks on, caption delimiter " ", placeholder image
//   mode with "<!-- image -->", chart tables on, indent 4, no wrap width, no
//   page-break placeholder, HTML escaping on, underscore escaping on, meta
//   and annotation sections unmarked, automatic original-list-marker mode
//   with marker validation, fenced code blocks, padded (non-compact) tables,
//   picture classification included, pictures not traversed.
//
// Two upstream facts shape the port. First, the reference reaches a document
// through its model layer, which normalizes on load; render_markdown applies
// the shared load normalization (src/render/load_normalization.h) so it
// starts from the same tree. Only the list-item migration can change
// Markdown, so the box clamping is skipped. Second, the model layer treats
// a table's cell grid as derived from the flat cell list and rebuilds picture
// and table annotations from `meta`; the wire `grid` and `annotations`
// projections are therefore not read here.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "grparse/document_render.h"
#include "canonical_json_writer.h"
#include "display_width.h"
#include "load_normalization.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

constexpr std::string_view kImagePlaceholder = "<!-- image -->";
constexpr std::string_view kFormulaPlaceholder = "<!-- formula-not-decoded -->";
constexpr std::string_view kMissingKeyValue = "<!-- missing-key-value-item -->";
constexpr std::string_view kMissingForm = "<!-- missing-form-item -->";
constexpr std::size_t kListIndent = 4;
constexpr std::string_view kCaptionDelim = " ";
// The reference table formatter's minimum header padding.
constexpr int kMinTablePadding = 2;

// ---------------------------------------------------------------------------
// Text helpers mirroring the reference's post-processing primitives.
// ---------------------------------------------------------------------------

// Whitespace-stripped copy, matching the strip the table formatter applies to
// every data cell.
std::string stripped(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\n\r\f\v");
  if (begin == std::string_view::npos) return std::string();
  const auto end = text.find_last_not_of(" \t\n\r\f\v");
  return std::string(text.substr(begin, end - begin + 1));
}

// Underscore escaping: every "_" not already escaped becomes "\_", except
// inside an inline image target, which is left verbatim so URLs survive. The
// image pattern is "![" alt "](" target ")" with neither part crossing a
// newline, matched left to right without overlap.
std::string escape_underscores(const std::string& text) {
  const auto escape_span = [&text](std::size_t from, std::size_t to,
                                   std::string* out) {
    for (std::size_t i = from; i < to; ++i) {
      if (text[i] == '_' && (i == 0 || text[i - 1] != '\\')) {
        out->append("\\_");
      } else {
        out->push_back(text[i]);
      }
    }
  };
  // The end of an image match starting at `start`, or npos when the pattern
  // does not complete. Mirrors the lazy quantifiers: the first "](" that
  // closes the alt text, then the first ")" that closes the target.
  const auto image_match_end = [&text](std::size_t start) -> std::size_t {
    for (std::size_t close = start + 2; close < text.size(); ++close) {
      if (text[close] == '\n') return std::string::npos;
      if (text[close] != ']') continue;
      if (close + 1 >= text.size() || text[close + 1] != '(') continue;
      for (std::size_t end = close + 2; end < text.size(); ++end) {
        if (text[end] == '\n') return std::string::npos;
        if (text[end] == ')') return end + 1;
      }
      return std::string::npos;
    }
    return std::string::npos;
  };

  std::string out;
  out.reserve(text.size());
  std::size_t last_end = 0;
  std::size_t at = 0;
  while (at + 1 < text.size()) {
    if (text[at] != '!' || text[at + 1] != '[') {
      ++at;
      continue;
    }
    const std::size_t match_end = image_match_end(at);
    if (match_end == std::string::npos) {
      ++at;
      continue;
    }
    escape_span(last_end, at, &out);
    out.append(text, at, match_end - at);
    last_end = match_end;
    at = match_end;
  }
  escape_span(last_end, text.size(), &out);
  return out;
}

// GFM hard line breaks: a lone newline gains two trailing spaces, a blank
// line stays a paragraph break.
std::string md_line_breaks(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t split = text.find("\n\n", at);
    const std::size_t end = split == std::string::npos ? text.size() : split;
    for (std::size_t i = at; i < end; ++i) {
      if (text[i] == '\n') out.append("  ");
      out.push_back(text[i]);
    }
    if (split == std::string::npos) break;
    out.append("\n\n");
    at = split + 2;
  }
  return out;
}

// Headings cannot span lines, so their newlines collapse to spaces.
std::string heading_line_breaks(std::string text) {
  std::ranges::replace(text, '\n', ' ');
  return text;
}

// The reference's humanizer: underscores become spaces and the first
// character is upper-cased (ASCII; the vocabularies it runs on are ASCII).
std::string humanized(std::string text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '_' && i + 1 < text.size() && text[i + 1] == '_') ++i;
    out.push_back(text[i] == '_' ? ' ' : text[i]);
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto c = static_cast<unsigned char>(out[i]);
    out[i] = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    if (!out.empty()) out.append(sep);
    out.append(part);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Numeric classification, as the reference's table formatter performs it to
// pick a column's alignment. Mirrors Python's int()/float() acceptance:
// surrounding whitespace, an optional sign, digit-group underscores, and the
// literal infinity and not-a-number spellings.
// ---------------------------------------------------------------------------

// Digits with at most single underscores between them, never at either end.
bool scan_digits(std::string_view text, std::size_t* at) {
  bool saw_digit = false;
  bool prev_underscore = false;
  const std::size_t start = *at;
  while (*at < text.size()) {
    const char c = text[*at];
    if (c >= '0' && c <= '9') {
      saw_digit = true;
      prev_underscore = false;
    } else if (c == '_') {
      if (!saw_digit || prev_underscore) return false;
      prev_underscore = true;
    } else {
      break;
    }
    ++*at;
  }
  if (prev_underscore) return false;
  if (!saw_digit) *at = start;
  return saw_digit;
}

bool matches_keyword(std::string_view text, std::size_t* at,
                     std::string_view word) {
  if (text.size() - *at < word.size()) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[*at + i])) != word[i]) {
      return false;
    }
  }
  *at += word.size();
  return true;
}

std::string_view python_trimmed(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\n\r\f\v");
  if (begin == std::string_view::npos) return std::string_view();
  return text.substr(begin, text.find_last_not_of(" \t\n\r\f\v") - begin + 1);
}

bool is_python_int(std::string_view raw) {
  const std::string_view text = python_trimmed(raw);
  std::size_t at = 0;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
  if (!scan_digits(text, &at)) return false;
  return at == text.size();
}

// Whether Python's float() would accept the text, and what it would produce.
bool is_python_float(std::string_view raw, double* value) {
  const std::string_view text = python_trimmed(raw);
  std::size_t at = 0;
  bool negative = false;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    negative = text[at] == '-';
    ++at;
  }
  if (matches_keyword(text, &at, "infinity") || matches_keyword(text, &at, "inf")) {
    if (at != text.size()) return false;
    *value = negative ? -HUGE_VAL : HUGE_VAL;
    return true;
  }
  if (matches_keyword(text, &at, "nan")) {
    if (at != text.size()) return false;
    *value = std::nan("");
    return true;
  }
  const std::size_t mantissa_start = at;
  const bool leading_digits = scan_digits(text, &at);
  bool fraction_digits = false;
  if (at < text.size() && text[at] == '.') {
    ++at;
    fraction_digits = scan_digits(text, &at);
  }
  if (!leading_digits && !fraction_digits) return false;
  const std::size_t mantissa_end = at;
  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    ++at;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
    if (!scan_digits(text, &at)) return false;
  }
  if (at != text.size()) return false;
  // Underscores are grouping only; strtod does not accept them.
  std::string plain;
  plain.reserve(text.size());
  if (negative) plain.push_back('-');
  for (std::size_t i = mantissa_start; i < text.size(); ++i) {
    if (text[i] != '_') plain.push_back(text[i]);
  }
  static_cast<void>(mantissa_end);
  *value = std::strtod(plain.c_str(), nullptr);
  return true;
}

// The reference's numeric test: float-convertible, and not an overflow to an
// infinity or a not-a-number that the text did not spell out literally.
bool is_reference_number(const std::string& text) {
  double value = 0.0;
  if (!is_python_float(text, &value)) return false;
  if (!std::isinf(value) && !std::isnan(value)) return true;
  std::string lowered;
  lowered.reserve(text.size());
  for (const char c : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lowered == "inf" || lowered == "-inf" || lowered == "nan";
}

// "^(([+-]?[0-9]{1,3})(?:,([0-9]{3}))*)?(?(1)\.[0-9]*|\.[0-9]+)?$"
bool has_thousands_separators(const std::string& text) {
  std::size_t at = 0;
  bool integer_part = false;
  const std::size_t sign_at = at;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
  std::size_t digits = 0;
  while (at < text.size() && digits < 3 && std::isdigit(static_cast<unsigned char>(text[at]))) {
    ++at;
    ++digits;
  }
  if (digits == 0) {
    at = sign_at;
  } else {
    integer_part = true;
    while (at + 3 < text.size() && text[at] == ',') {
      if (!std::isdigit(static_cast<unsigned char>(text[at + 1])) ||
          !std::isdigit(static_cast<unsigned char>(text[at + 2])) ||
          !std::isdigit(static_cast<unsigned char>(text[at + 3]))) {
        break;
      }
      at += 4;
    }
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    std::size_t fraction = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
      ++at;
      ++fraction;
    }
    if (!integer_part && fraction == 0) return false;
  }
  return at == text.size();
}

// The reference's type ladder for a cell value; only the ordering matters.
enum class ValueType { kNone = 0, kBool = 1, kInt = 2, kFloat = 3, kStr = 5 };

ValueType value_type(const std::string& text) {
  if (text.empty()) return ValueType::kNone;
  if (text == "True" || text == "False") return ValueType::kBool;
  if (is_python_int(text) ||
      (has_thousands_separators(text) && !text.contains('.'))) {
    return ValueType::kInt;
  }
  if (is_reference_number(text) || has_thousands_separators(text)) {
    return ValueType::kFloat;
  }
  return ValueType::kStr;
}

// A column is right-aligned when every value folds to a number; the fold
// starts at the boolean rung, so an all-empty column never does.
bool column_is_numeric(const std::vector<std::string>& values) {
  ValueType folded = ValueType::kBool;
  for (const auto& value : values) {
    folded = std::max(folded, value_type(value));
  }
  return folded == ValueType::kInt || folded == ValueType::kFloat;
}

// ---------------------------------------------------------------------------
// Meta values. Custom meta fields carry arbitrary JSON payloads, and the
// reference renders them with the host language's str(); these helpers
// reproduce that, including the repr() nesting inside containers.
// ---------------------------------------------------------------------------

std::string python_string_repr(const std::string& text) {
  const char quote = text.contains('\'') && !text.contains('"') ? '"' : '\'';
  std::string out(1, quote);
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == quote || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out.append("\\n");
    } else if (c == '\r') {
      out.append("\\r");
    } else if (c == '\t') {
      out.append("\\t");
    } else if (byte < 0x20 || byte == 0x7f) {
      static constexpr char kHex[] = "0123456789abcdef";
      out.append("\\x");
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0f]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back(quote);
  return out;
}

std::string number_text(double number) {
  // The model narrows integral numbers to integers on import.
  if (std::isfinite(number) && std::floor(number) == number) {
    return canonical_integral_decimal(number);
  }
  return canonical_double(number);
}

bool value_is_truthy(const google::protobuf::Value& value);
std::string value_repr(const google::protobuf::Value& value);

std::string value_str(const google::protobuf::Value& value) {
  // str() differs from repr() only for a bare string.
  if (value.kind_case() == google::protobuf::Value::kStringValue) {
    return value.string_value();
  }
  return value_repr(value);
}

std::string value_repr(const google::protobuf::Value& value) {
  switch (value.kind_case()) {
    case google::protobuf::Value::kBoolValue:
      return value.bool_value() ? "True" : "False";
    case google::protobuf::Value::kNumberValue:
      return number_text(value.number_value());
    case google::protobuf::Value::kStringValue:
      return python_string_repr(value.string_value());
    case google::protobuf::Value::kListValue: {
      std::string out = "[";
      bool first = true;
      for (const auto& entry : value.list_value().values()) {
        if (!first) out.append(", ");
        first = false;
        out.append(value_repr(entry));
      }
      return out + "]";
    }
    case google::protobuf::Value::kStructValue: {
      // The wire map is unordered; sorted keys keep the rendering stable.
      std::vector<std::string> keys;
      keys.reserve(value.struct_value().fields().size());
      for (const auto& [key, unused_value] : value.struct_value().fields()) {
        keys.push_back(key);
      }
      std::ranges::sort(keys);
      std::string out = "{";
      bool first = true;
      for (const auto& key : keys) {
        if (!first) out.append(", ");
        first = false;
        out.append(python_string_repr(key));
        out.append(": ");
        out.append(value_repr(value.struct_value().fields().at(key)));
      }
      return out + "}";
    }
    case google::protobuf::Value::kNullValue:
    case google::protobuf::Value::KIND_NOT_SET: return "None";
  }
  return "None";
}

// ---------------------------------------------------------------------------
// Generic model stringification. A few meta fields have no rendering of their
// own and fall through to the reference's stringification of the model
// object, which prints "name=value" pairs in field-declaration order with
// every value repr'd, the extra members last.
// ---------------------------------------------------------------------------

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

// A meta field renders only when its value is truthy, matching the
// reference's `str(value or "")` guard.
bool value_is_truthy(const google::protobuf::Value& value) {
  switch (value.kind_case()) {
    case google::protobuf::Value::kBoolValue: return value.bool_value();
    case google::protobuf::Value::kNumberValue: return value.number_value() != 0.0;
    case google::protobuf::Value::kStringValue: return !value.string_value().empty();
    case google::protobuf::Value::kListValue:
      return !value.list_value().values().empty();
    case google::protobuf::Value::kStructValue:
      return !value.struct_value().fields().empty();
    case google::protobuf::Value::kNullValue:
    case google::protobuf::Value::KIND_NOT_SET: return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Text item classification. The model reconstructs a text entry's class from
// its dedicated arm, or, for the generic arm foreign producers use, from the
// label alone.
// ---------------------------------------------------------------------------

enum class TextClass {
  kTitle,
  kSectionHeader,
  kListItem,
  kCode,
  kFormula,
  kFieldHeading,
  kFieldValue,
  kText,
};

TextClass classify_text(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return TextClass::kTitle;
    case docv1::BaseTextItem::kSectionHeader: return TextClass::kSectionHeader;
    case docv1::BaseTextItem::kListItem: return TextClass::kListItem;
    case docv1::BaseTextItem::kCode: return TextClass::kCode;
    case docv1::BaseTextItem::kFormula: return TextClass::kFormula;
    case docv1::BaseTextItem::kFieldHeading: return TextClass::kFieldHeading;
    case docv1::BaseTextItem::kFieldValue: return TextClass::kFieldValue;
    case docv1::BaseTextItem::kText: break;
    case docv1::BaseTextItem::ITEM_NOT_SET: return TextClass::kText;
  }
  switch (item.text().base().label()) {
    case docv1::DOC_ITEM_LABEL_TITLE: return TextClass::kTitle;
    case docv1::DOC_ITEM_LABEL_SECTION_HEADER: return TextClass::kSectionHeader;
    case docv1::DOC_ITEM_LABEL_LIST_ITEM: return TextClass::kListItem;
    case docv1::DOC_ITEM_LABEL_FORMULA: return TextClass::kFormula;
    case docv1::DOC_ITEM_LABEL_FIELD_HEADING: return TextClass::kFieldHeading;
    case docv1::DOC_ITEM_LABEL_FIELD_VALUE: return TextClass::kFieldValue;
    case docv1::DOC_ITEM_LABEL_CODE: return TextClass::kCode;
    default: return TextClass::kText;
  }
}

// The label vocabulary the export includes. Everything outside it is dropped
// (its captions still render, exactly as in the reference).
bool exported_label(docv1::DocItemLabel label) {
  switch (label) {
    case docv1::DOC_ITEM_LABEL_CHART:
    case docv1::DOC_ITEM_LABEL_GRADING_SCALE:
    case docv1::DOC_ITEM_LABEL_FIELD_REGION:
    case docv1::DOC_ITEM_LABEL_FIELD_ITEM: return false;
    default: return true;
  }
}

// The label the model reconstructs: an unset tag falls back to the class
// default, which the export vocabulary always contains.
docv1::DocItemLabel effective_label(docv1::DocItemLabel label,
                                    docv1::DocItemLabel fallback) {
  return label == docv1::DOC_ITEM_LABEL_UNSPECIFIED ? fallback : label;
}

// ---------------------------------------------------------------------------
// The renderer.
// ---------------------------------------------------------------------------

class MarkdownRenderer : RendererBase {
 public:
  explicit MarkdownRenderer(const docv1::Document& document)
      : RendererBase(document) {
    collect_reference_sets();
  }

  std::string render() {
    std::vector<std::string> parts;
    consumed_.insert("#/body");
    parts.push_back(join(part_texts(get_parts("#/body", 0, false)), "\n\n"));
    if (document_.body().has_meta()) {
      parts.push_back(serialize_meta(document_.body().meta(), MetaShape::kBase));
    }
    return join(parts, "\n\n");
  }

 private:
  // One serialized sibling. `first_span` is the reference of the first
  // document item the part's text came from, which is the one structural
  // fact a list group needs about a part: a part whose first item sits in an
  // inline group is appended to the preceding line instead of starting a new
  // one. Empty when the part covers no document item.
  struct Part {
    std::string text;
    std::string first_span;
  };

  enum class MetaShape { kBase, kFloating, kPicture };

  std::set<std::string> caption_refs_;
  std::set<std::string> footnote_refs_;
  std::set<std::string> excluded_refs_;

  // -- reference resolution -------------------------------------------------

  const docv1::GroupItem* group_at(const std::string& ref) const {
    if (ref == "#/body") return &document_.body();
    if (ref == "#/furniture") return &document_.furniture();
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kGroup && parsed.index < document_.groups_size()) {
      return &document_.groups(parsed.index);
    }
    return nullptr;
  }

  const docv1::BaseTextItem* text_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kText && parsed.index < document_.texts_size()) {
      return &document_.texts(parsed.index);
    }
    return nullptr;
  }

  const docv1::TableItem* table_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kTable && parsed.index < document_.tables_size()) {
      return &document_.tables(parsed.index);
    }
    return nullptr;
  }

  const docv1::PictureItem* picture_at(const std::string& ref) const {
    const ArenaRef parsed = parse_ref(ref);
    if (parsed.kind == ArenaRef::kPicture &&
        parsed.index < document_.pictures_size()) {
      return &document_.pictures(parsed.index);
    }
    return nullptr;
  }

  // The child references of any node, in document order.
  std::vector<std::string> children_of(const std::string& ref) const {
    std::vector<std::string> refs;
    const auto collect = [&refs](const auto& children) {
      for (const auto& child : children) refs.push_back(child.ref());
    };
    if (const auto* group = group_at(ref)) {
      collect(group->children());
      return refs;
    }
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        collect(text->code().children());
      } else if (const auto* base = text_base(*text)) {
        collect(base->children());
      }
      return refs;
    }
    if (const auto* table = table_at(ref)) {
      collect(table->children());
      return refs;
    }
    if (const auto* picture = picture_at(ref)) {
      collect(picture->children());
      return refs;
    }
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        if (parsed.index < document_.key_value_items_size()) {
          collect(document_.key_value_items(parsed.index).children());
        }
        break;
      case ArenaRef::kForm:
        if (parsed.index < document_.form_items_size()) {
          collect(document_.form_items(parsed.index).children());
        }
        break;
      case ArenaRef::kFieldRegion:
        if (parsed.index < document_.field_regions_size()) {
          collect(document_.field_regions(parsed.index).children());
        }
        break;
      case ArenaRef::kFieldItem:
        if (parsed.index < document_.field_items_size()) {
          collect(document_.field_items(parsed.index).children());
        }
        break;
      default: break;
    }
    return refs;
  }

  docv1::ContentLayer layer_of(const std::string& ref) const {
    if (const auto* group = group_at(ref)) return group->content_layer();
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        return text->code().content_layer();
      }
      if (const auto* base = text_base(*text)) return base->content_layer();
      return docv1::CONTENT_LAYER_UNSPECIFIED;
    }
    if (const auto* table = table_at(ref)) return table->content_layer();
    if (const auto* picture = picture_at(ref)) return picture->content_layer();
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        if (parsed.index < document_.key_value_items_size()) {
          return document_.key_value_items(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kForm:
        if (parsed.index < document_.form_items_size()) {
          return document_.form_items(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kFieldRegion:
        if (parsed.index < document_.field_regions_size()) {
          return document_.field_regions(parsed.index).content_layer();
        }
        break;
      case ArenaRef::kFieldItem:
        if (parsed.index < document_.field_items_size()) {
          return document_.field_items(parsed.index).content_layer();
        }
        break;
      default: break;
    }
    return docv1::CONTENT_LAYER_UNSPECIFIED;
  }

  // The label the exclusion test reads, or unspecified for a node that is
  // not a document item (groups are never excluded).
  docv1::DocItemLabel label_of(const std::string& ref) const {
    if (const auto* text = text_at(ref)) {
      switch (classify_text(*text)) {
        case TextClass::kTitle: return docv1::DOC_ITEM_LABEL_TITLE;
        case TextClass::kSectionHeader: return docv1::DOC_ITEM_LABEL_SECTION_HEADER;
        case TextClass::kListItem: return docv1::DOC_ITEM_LABEL_LIST_ITEM;
        case TextClass::kCode: return docv1::DOC_ITEM_LABEL_CODE;
        case TextClass::kFormula: return docv1::DOC_ITEM_LABEL_FORMULA;
        case TextClass::kFieldHeading: return docv1::DOC_ITEM_LABEL_FIELD_HEADING;
        case TextClass::kFieldValue: return docv1::DOC_ITEM_LABEL_FIELD_VALUE;
        case TextClass::kText: break;
      }
      const auto* base = text_base(*text);
      return effective_label(base != nullptr ? base->label()
                                             : docv1::DOC_ITEM_LABEL_TEXT,
                             docv1::DOC_ITEM_LABEL_TEXT);
    }
    if (const auto* table = table_at(ref)) {
      return effective_label(table->label(), docv1::DOC_ITEM_LABEL_TABLE);
    }
    if (const auto* picture = picture_at(ref)) {
      return effective_label(picture->label(), docv1::DOC_ITEM_LABEL_PICTURE);
    }
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        return parsed.index < document_.key_value_items_size()
                   ? effective_label(document_.key_value_items(parsed.index).label(),
                                     docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION)
                   : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
      case ArenaRef::kForm:
        return parsed.index < document_.form_items_size()
                   ? effective_label(document_.form_items(parsed.index).label(),
                                     docv1::DOC_ITEM_LABEL_FORM)
                   : docv1::DOC_ITEM_LABEL_UNSPECIFIED;
      case ArenaRef::kFieldRegion: return docv1::DOC_ITEM_LABEL_FIELD_REGION;
      case ArenaRef::kFieldItem: return docv1::DOC_ITEM_LABEL_FIELD_ITEM;
      default: return docv1::DOC_ITEM_LABEL_UNSPECIFIED;
    }
  }

  bool is_document_item(const std::string& ref) const {
    return group_at(ref) == nullptr && parse_ref(ref).kind != ArenaRef::kUnknown;
  }

  bool excluded(const std::string& ref) const {
    return excluded_refs_.contains(ref);
  }

  // -- reference sets -------------------------------------------------------

  // The caption and footnote references some floating item claims, and the
  // body-tree items the label vocabulary drops. Both scans walk the tree the
  // way the reference does for these sets: all layers for the claim scan,
  // body layer only for the exclusion scan, pictures traversed in both.
  void collect_reference_sets() {
    const auto claim = [this](const auto& item) {
      for (const auto& ref : item.captions()) caption_refs_.insert(ref.ref());
      for (const auto& ref : item.footnotes()) footnote_refs_.insert(ref.ref());
    };
    for (const auto& item : document_.tables()) claim(item);
    for (const auto& item : document_.pictures()) claim(item);
    for (const auto& item : document_.key_value_items()) claim(item);
    for (const auto& item : document_.form_items()) claim(item);
    for (const auto& item : document_.texts()) {
      if (item.item_case() == docv1::BaseTextItem::kCode) claim(item.code());
    }

    std::set<std::string> seen;
    walk_for_exclusions("#/body", &seen);
  }

  void walk_for_exclusions(const std::string& ref, std::set<std::string>* seen) {
    if (!seen->insert(ref).second) return;
    if (layer_of(ref) == docv1::CONTENT_LAYER_BODY ||
        layer_of(ref) == docv1::CONTENT_LAYER_UNSPECIFIED) {
      if (is_document_item(ref) && !exported_label(label_of(ref))) {
        excluded_refs_.insert(ref);
      }
    }
    for (const auto& child : children_of(ref)) walk_for_exclusions(child, seen);
  }

  // -- the walk -------------------------------------------------------------

  // Pre-order references under `root`, yielding only body-layer nodes but
  // descending regardless, and stopping at a picture's children unless they
  // are that picture's own captions.
  void collect_walk(const std::string& ref, std::set<std::string>* seen,
                    std::vector<std::string>* out) const {
    if (!seen->insert(ref).second) return;
    const docv1::ContentLayer layer = layer_of(ref);
    if (layer == docv1::CONTENT_LAYER_BODY ||
        layer == docv1::CONTENT_LAYER_UNSPECIFIED) {
      out->push_back(ref);
    }
    const auto* picture = picture_at(ref);
    std::set<std::string> allowed;
    if (picture != nullptr) {
      for (const auto& caption : picture->captions()) allowed.insert(caption.ref());
    }
    for (const auto& child : children_of(ref)) {
      if (picture != nullptr && !allowed.contains(child)) continue;
      collect_walk(child, seen, out);
    }
  }

  std::vector<Part> get_parts(const std::string& root, int list_level,
                              bool inline_scope) {
    std::vector<std::string> refs;
    std::set<std::string> seen;
    collect_walk(root, &seen, &refs);
    std::vector<Part> parts;
    for (const auto& ref : refs) {
      if (!consume(ref)) continue;
      Part part;
      part.text = serialize(ref, list_level, inline_scope, &part.first_span);
      if (!part.text.empty()) parts.push_back(std::move(part));
    }
    return parts;
  }

  // The reference a node declares as its parent, or empty when it declares
  // none.
  std::string parent_of(const std::string& ref) const {
    const auto parent = [](const auto& item) {
      return item.has_parent() ? item.parent().ref() : std::string();
    };
    if (const auto* group = group_at(ref)) return parent(*group);
    if (const auto* text = text_at(ref)) {
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        return parent(text->code());
      }
      const auto* base = text_base(*text);
      return base != nullptr ? parent(*base) : std::string();
    }
    if (const auto* table = table_at(ref)) return parent(*table);
    if (const auto* picture = picture_at(ref)) return parent(*picture);
    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue:
        if (parsed.index < document_.key_value_items_size()) {
          return parent(document_.key_value_items(parsed.index));
        }
        break;
      case ArenaRef::kForm:
        if (parsed.index < document_.form_items_size()) {
          return parent(document_.form_items(parsed.index));
        }
        break;
      case ArenaRef::kFieldRegion:
        if (parsed.index < document_.field_regions_size()) {
          return parent(document_.field_regions(parsed.index));
        }
        break;
      case ArenaRef::kFieldItem:
        if (parsed.index < document_.field_items_size()) {
          return parent(document_.field_items(parsed.index));
        }
        break;
      default: break;
    }
    return std::string();
  }

  static std::vector<std::string> part_texts(const std::vector<Part>& parts) {
    std::vector<std::string> texts;
    texts.reserve(parts.size());
    for (const auto& part : parts) texts.push_back(part.text);
    return texts;
  }

  bool is_inline_group(const std::string& ref) const {
    const auto* group = group_at(ref);
    return group != nullptr && group->label() == docv1::GROUP_LABEL_INLINE;
  }

  bool is_list_group(const std::string& ref) const {
    const auto* group = group_at(ref);
    return group != nullptr && (group->label() == docv1::GROUP_LABEL_LIST ||
                                group->label() == docv1::GROUP_LABEL_ORDERED_LIST);
  }

  std::string serialize(const std::string& ref, int list_level,
                        bool inline_scope, std::string* first_span = nullptr) {
    consumed_.insert(ref);
    const auto claim = [&](const std::string& span) {
      if (first_span != nullptr && first_span->empty()) *first_span = span;
    };
    std::vector<std::string> parts;

    if (const auto* group = group_at(ref)) {
      // A group is not a document item, so it never covers a span itself;
      // its span is the first one its content covers.
      parts.push_back(
          serialize_group_content(ref, *group, list_level, inline_scope, first_span));
      if (group->has_meta() && !excluded(ref)) {
        parts.push_back(serialize_meta(group->meta(), MetaShape::kBase));
      }
      return join(parts, "\n\n");
    }

    if (const auto* text = text_at(ref)) {
      // A caption or footnote renders through the item that claims it, meta
      // included.
      if (caption_refs_.contains(ref) || footnote_refs_.contains(ref)) {
        return std::string();
      }
      std::string body;
      if (!excluded(ref)) {
        std::string body_span;
        body = serialize_text(ref, *text, inline_scope, &body_span);
        claim(body_span);
      }
      parts.push_back(std::move(body));
      const std::size_t before = parts.size();
      append_text_meta(*text, ref, &parts);
      if (parts.size() > before && !parts.back().empty()) claim(ref);
      return join(parts, "\n\n");
    }

    if (const auto* table = table_at(ref)) {
      std::string body_span;
      parts.push_back(serialize_table(ref, *table, false, &body_span));
      claim(body_span);
      if (table->has_meta() && !excluded(ref)) {
        parts.push_back(serialize_meta_floating(table->meta()));
        if (!parts.back().empty()) claim(ref);
      }
      return join(parts, "\n\n");
    }

    if (const auto* picture = picture_at(ref)) {
      std::string body_span;
      parts.push_back(serialize_picture(ref, *picture, &body_span));
      claim(body_span);
      if (picture->has_meta() && !excluded(ref)) {
        parts.push_back(serialize_picture_meta(picture->meta()));
        if (!parts.back().empty()) claim(ref);
      }
      return join(parts, "\n\n");
    }

    const ArenaRef parsed = parse_ref(ref);
    switch (parsed.kind) {
      case ArenaRef::kKeyValue: {
        if (parsed.index >= document_.key_value_items_size()) break;
        const auto& item = document_.key_value_items(parsed.index);
        if (!excluded(ref)) {
          parts.emplace_back(kMissingKeyValue);
          claim(ref);
          if (item.has_meta()) {
            parts.push_back(serialize_meta_floating(item.meta()));
          }
        }
        break;
      }
      case ArenaRef::kForm: {
        if (parsed.index >= document_.form_items_size()) break;
        const auto& item = document_.form_items(parsed.index);
        if (!excluded(ref)) {
          parts.emplace_back(kMissingForm);
          claim(ref);
          if (item.has_meta()) {
            parts.push_back(serialize_meta_floating(item.meta()));
          }
        }
        break;
      }
      // Field regions and field items serialize to nothing; their children
      // are reached by the walk, not through them.
      case ArenaRef::kFieldRegion:
      case ArenaRef::kFieldItem: break;
      default: break;
    }
    return join(parts, "\n\n");
  }

  void append_text_meta(const docv1::BaseTextItem& text, const std::string& ref,
                        std::vector<std::string>* parts) {
    if (excluded(ref)) return;
    if (text.item_case() == docv1::BaseTextItem::kCode) {
      if (text.code().has_meta()) {
        parts->push_back(serialize_meta_floating(text.code().meta()));
      }
      return;
    }
    const auto* base = text_base(text);
    if (base != nullptr && base->has_meta()) {
      parts->push_back(serialize_meta(base->meta(), MetaShape::kBase));
    }
  }

  static std::string first_part_span(const std::vector<Part>& parts) {
    for (const auto& part : parts) {
      if (!part.first_span.empty()) return part.first_span;
    }
    return std::string();
  }

  std::string serialize_group_content(const std::string& ref,
                                      const docv1::GroupItem& group,
                                      int list_level, bool inline_scope,
                                      std::string* first_span) {
    if (group.label() == docv1::GROUP_LABEL_LIST ||
        group.label() == docv1::GROUP_LABEL_ORDERED_LIST) {
      return serialize_list(ref, list_level, inline_scope, first_span);
    }
    if (group.label() == docv1::GROUP_LABEL_INLINE) {
      const auto parts = get_parts(ref, list_level, true);
      if (first_span != nullptr) *first_span = first_part_span(parts);
      return join(part_texts(parts), " ");
    }
    // Every other group label is a transparent block container; its parts
    // start a fresh list scope.
    const auto parts = get_parts(ref, 0, false);
    if (first_span != nullptr) *first_span = first_part_span(parts);
    return join(part_texts(parts), "\n\n");
  }

  // -- lists ----------------------------------------------------------------

  // Whether a part's first document item declares an inline group as its
  // parent, which is what makes the list append the part to the line before
  // it instead of opening a new one.
  bool span_sits_in_inline_group(const std::string& span) const {
    return !span.empty() && is_inline_group(parent_of(span));
  }

  std::string serialize_list(const std::string& ref, int list_level,
                             bool inline_scope, std::string* first_span) {
    std::vector<Part> parts = get_parts(ref, list_level + 1, inline_scope);
    if (first_span != nullptr) *first_span = first_part_span(parts);
    std::vector<Part> merged;
    for (auto& part : parts) {
      if (!merged.empty() && span_sits_in_inline_group(part.first_span)) {
        merged.back().text.append(part.text);
      } else {
        merged.push_back(std::move(part));
      }
    }
    const std::string indent(static_cast<std::size_t>(list_level) * kListIndent, ' ');
    std::string out;
    for (std::size_t i = 0; i < merged.size(); ++i) {
      if (i != 0) out.push_back('\n');
      // A part that already starts with a space is an evaluated sublist and
      // carries its own indent.
      if (!merged[i].text.empty() && merged[i].text.front() == ' ') {
        out.append(merged[i].text);
      } else {
        out.append(indent);
        out.append(merged[i].text);
      }
    }
    return out;
  }

  // The list marker pieces for one item, following the reference's marker
  // rules: keep an already-valid original marker, keep any alphanumeric one
  // behind a generated "-", and number an enumerated item that carries no
  // marker of its own.
  std::string list_item_prefix(const std::string& ref,
                               const docv1::BaseTextItem& text) {
    std::string marker = "-";
    bool enumerated = false;
    if (text.item_case() == docv1::BaseTextItem::kListItem) {
      enumerated = text.list_item().enumerated();
      if (text.list_item().has_marker()) marker = text.list_item().marker();
    }
    static_cast<void>(enumerated);

    const bool has_alnum = std::ranges::any_of(marker, [](unsigned char c) {
      return std::isalnum(c) != 0;
    });
    const bool numeric_marker =
        marker.size() > 1 && marker.back() == '.' &&
        std::ranges::all_of(marker.substr(0, marker.size() - 1),
                            [](unsigned char c) { return std::isdigit(c) != 0; });
    const bool already_valid =
        marker == "-" || marker == "*" || marker == "+" || numeric_marker;

    std::vector<std::string> pieces;
    if (!already_valid) {
      std::string generated = "-";
      const auto* base = text_base(text);
      const std::string parent_ref =
          base != nullptr && base->has_parent() ? base->parent().ref() : std::string();
      if (marker.empty() && is_list_group(parent_ref) &&
          first_item_is_enumerated(parent_ref)) {
        int position = -1;
        const auto siblings = children_of(parent_ref);
        for (std::size_t i = 0; i < siblings.size(); ++i) {
          if (siblings[i] == ref) {
            position = static_cast<int>(i);
            break;
          }
        }
        generated = std::to_string(position + 1) + ".";
      }
      pieces.push_back(generated);
    }
    if (!marker.empty() && (has_alnum || already_valid)) pieces.push_back(marker);
    return join(pieces, " ");
  }

  bool first_item_is_enumerated(const std::string& group_ref) const {
    for (const auto& child : children_of(group_ref)) {
      const auto* text = text_at(child);
      if (text == nullptr) return false;
      return text->item_case() == docv1::BaseTextItem::kListItem &&
             text->list_item().enumerated();
    }
    return false;
  }

  // -- text items -----------------------------------------------------------

  std::string post_process(const std::string& text, bool escape_html_chars,
                           bool escape_underscore_chars,
                           const docv1::Formatting* formatting,
                           const std::string* hyperlink) {
    std::string out = text;
    if (escape_underscore_chars) out = escape_underscores(out);
    if (escape_html_chars) out = escape_html_text(out);
    if (formatting != nullptr) {
      if (formatting->bold()) out = "**" + out + "**";
      if (formatting->italic()) out = "*" + out + "*";
      // Underline, subscript, and superscript have no Markdown rendering.
      if (formatting->strikethrough()) out = "~~" + out + "~~";
    }
    if (hyperlink != nullptr) out = "[" + out + "](" + normalized_uri(*hyperlink) + ")";
    return out;
  }

  std::string serialize_text(const std::string& ref,
                             const docv1::BaseTextItem& item, bool inline_scope,
                             std::string* first_span) {
    const TextClass kind = classify_text(item);
    if (kind == TextClass::kCode) {
      return serialize_code(ref, item, inline_scope, first_span);
    }

    const auto* base = text_base(item);
    if (base == nullptr) return std::string();
    const docv1::Formatting* formatting =
        base->has_formatting() ? &base->formatting() : nullptr;
    const std::string* hyperlink =
        base->has_hyperlink() ? &base->hyperlink() : nullptr;

    // A text item whose only content is one inline group renders that group
    // and skips its own processing: the children carry the formatting.
    std::string text = base->text();
    bool processing_pending = true;
    const auto children = children_of(ref);
    if (text.empty() && children.size() == 1 && is_inline_group(children.front())) {
      if (consume(children.front())) {
        text = serialize(children.front(), 0, false);
      } else {
        text.clear();
      }
      processing_pending = false;
    }

    if (base->label() == docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED) {
      text = "- [x] " + text;
    } else if (base->label() == docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED) {
      text = "- [ ] " + text;
    }

    std::string text_part;
    bool escape_html_chars = true;
    bool escape_underscore_chars = true;
    if (kind == TextClass::kListItem || kind == TextClass::kTitle ||
        kind == TextClass::kSectionHeader) {
      if (processing_pending) {
        text = kind == TextClass::kListItem ? md_line_breaks(text)
                                            : heading_line_breaks(text);
        text = post_process(text, true, true, formatting, hyperlink);
        processing_pending = false;
      }
      if (kind == TextClass::kListItem) {
        const std::string prefix = list_item_prefix(ref, item);
        text_part = prefix.empty() ? text : prefix + " " + text;
      } else if (kind == TextClass::kTitle) {
        text_part = "# " + text;
      } else {
        const int level = std::max(item.section_header().level(), 1);
        text_part = std::string(static_cast<std::size_t>(level) + 1, '#') + " " + text;
      }
    } else if (kind == TextClass::kFormula) {
      escape_html_chars = false;
      escape_underscore_chars = false;
      if (!text.empty()) {
        text_part = inline_scope ? "$" + text + "$" : "$$" + text + "$$";
      } else if (!base->orig().empty()) {
        text_part = std::string(kFormulaPlaceholder);
      }
    } else {
      text_part = md_line_breaks(text);
    }

    std::vector<std::string> res_parts;
    if (!text_part.empty()) {
      res_parts.push_back(text_part);
      *first_span = ref;
    }
    std::string out = join(res_parts, inline_scope ? " " : "\n\n");
    if (processing_pending) {
      out = post_process(out, escape_html_chars, escape_underscore_chars, formatting,
                         hyperlink);
    }
    return out;
  }

  // A code item reaches the renderer two ways: through its own variant, or
  // through the generic text arm carrying a code label, which the model
  // rebuilds into a code item with the base fields and no captions.
  std::string serialize_code(const std::string& ref, const docv1::BaseTextItem& item,
                             bool inline_scope, std::string* first_span) {
    const bool own_variant = item.item_case() == docv1::BaseTextItem::kCode;
    const auto* base = own_variant ? nullptr : text_base(item);
    if (!own_variant && base == nullptr) return std::string();
    const std::string& text = own_variant ? item.code().text() : base->text();
    const bool has_formatting =
        own_variant ? item.code().has_formatting() : base->has_formatting();
    const docv1::Formatting* formatting =
        !has_formatting            ? nullptr
        : own_variant              ? &item.code().formatting()
                                   : &base->formatting();
    const bool has_hyperlink =
        own_variant ? item.code().has_hyperlink() : base->has_hyperlink();
    const std::string* hyperlink =
        !has_hyperlink ? nullptr
        : own_variant  ? &item.code().hyperlink()
                       : &base->hyperlink();
    // Inline code, and any code carrying a hyperlink, uses single backticks;
    // everything else uses a plain fence with no info string.
    const bool backticks = inline_scope || hyperlink != nullptr;
    std::vector<std::string> res_parts;
    res_parts.push_back(backticks ? "`" + text + "`" : "```\n" + text + "\n```");
    *first_span = ref;
    if (own_variant) {
      const std::string captions = serialize_captions(item.code().captions());
      if (!captions.empty()) res_parts.push_back(captions);
    }
    return post_process(join(res_parts, inline_scope ? " " : "\n\n"), false, false,
                        formatting, hyperlink);
  }

  // -- captions -------------------------------------------------------------

  std::string serialize_captions(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions,
      std::string* first_span = nullptr) {
    std::vector<std::string> texts;
    for (const auto& ref : captions) {
      const auto* text = text_at(ref.ref());
      if (text == nullptr || excluded(ref.ref())) continue;
      if (first_span != nullptr && first_span->empty()) *first_span = ref.ref();
      if (text->item_case() == docv1::BaseTextItem::kCode) {
        texts.push_back(text->code().text());
        continue;
      }
      const auto* base = text_base(*text);
      if (base != nullptr) texts.push_back(base->text());
    }
    std::string joined;
    for (std::size_t i = 0; i < texts.size(); ++i) {
      if (i != 0) joined.append(kCaptionDelim);
      joined.append(texts[i]);
    }
    return post_process(joined, true, true, nullptr, nullptr);
  }

  // -- tables ---------------------------------------------------------------

  std::string serialize_table(const std::string& ref, const docv1::TableItem& table,
                              bool nested_in_table,
                              std::string* first_span = nullptr) {
    if (nested_in_table) {
      mark_subtree_visited(ref);
      return collect_subtree_text(ref);
    }
    std::vector<std::string> parts;
    std::string caption_span;
    parts.push_back(serialize_captions(table.captions(), &caption_span));
    if (!parts.back().empty() && first_span != nullptr) *first_span = caption_span;
    if (!excluded(ref)) {
      parts.push_back(table_markdown(table.data()));
      if (!parts.back().empty() && first_span != nullptr && first_span->empty()) {
        *first_span = ref;
      }
    }
    return join(parts, "\n\n");
  }

  // The cell grid the model derives from the flat cell list: a rectangle of
  // empty cells that every declared cell overwrites at each position it
  // covers.
  std::vector<std::vector<std::string>> table_rows(const docv1::TableData& data) {
    const int rows = std::max(data.num_rows(), 0);
    const int cols = std::max(data.num_cols(), 0);
    std::vector<std::vector<const docv1::TableCell*>> grid(
        static_cast<std::size_t>(rows),
        std::vector<const docv1::TableCell*>(static_cast<std::size_t>(cols), nullptr));
    for (const auto& cell : data.table_cells()) {
      const int row_begin = std::clamp(cell.start_row_offset_idx(), 0, rows);
      const int row_end = std::clamp(cell.end_row_offset_idx(), 0, rows);
      const int col_begin = std::clamp(cell.start_col_offset_idx(), 0, cols);
      const int col_end = std::clamp(cell.end_col_offset_idx(), 0, cols);
      for (int row = row_begin; row < row_end; ++row) {
        for (int col = col_begin; col < col_end; ++col) {
          grid[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = &cell;
        }
      }
    }
    std::vector<std::vector<std::string>> out;
    out.reserve(grid.size());
    for (const auto& row : grid) {
      std::vector<std::string> texts;
      texts.reserve(row.size());
      for (const auto* cell : row) {
        std::string text;
        if (cell != nullptr) {
          text = cell->has_ref() ? rich_cell_text(cell->ref().ref()) : cell->text();
        }
        // A cell must not break the row or the column separator.
        std::string safe;
        safe.reserve(text.size());
        for (const char c : text) {
          if (c == '\n') {
            safe.push_back(' ');
          } else if (c == '|') {
            safe.append("&#124;");
          } else {
            safe.push_back(c);
          }
        }
        texts.push_back(std::move(safe));
      }
      out.push_back(std::move(texts));
    }
    return out;
  }

  // A cell that points at another item renders that item, with a nested
  // table flattened to its text so the row stays intact.
  std::string rich_cell_text(const std::string& ref) {
    if (const auto* table = table_at(ref)) return serialize_table(ref, *table, true);
    consume(ref);
    return serialize(ref, 0, false);
  }

  void mark_subtree_visited(const std::string& ref) {
    if (!consume(ref)) return;
    for (const auto& child : children_of(ref)) mark_subtree_visited(child);
  }

  std::string collect_subtree_text(const std::string& ref) const {
    std::vector<std::string> parts;
    if (const auto* table = table_at(ref)) {
      for (const auto& cell : table->data().table_cells()) {
        if (!cell.text().empty()) parts.push_back(cell.text());
      }
      return join(parts, " ");
    }
    if (const auto* text = text_at(ref)) {
      const auto* base = text_base(*text);
      const std::string& own =
          text->item_case() == docv1::BaseTextItem::kCode ? text->code().text()
          : base != nullptr                               ? base->text()
                                                          : std::string();
      if (!own.empty()) parts.push_back(own);
    }
    for (const auto& child : children_of(ref)) {
      const std::string child_text = collect_subtree_text(child);
      if (!child_text.empty()) parts.push_back(child_text);
    }
    return join(parts, " ");
  }

  // The reference's padded table layout: one leading and one trailing space
  // per cell, a column width of at least the header width plus two, data
  // cells stripped and padded to the column width, and a dashed rule under
  // the header row.
  std::string table_markdown(const docv1::TableData& data) {
    const auto rows = table_rows(data);
    if (rows.empty()) return std::string();
    const std::vector<std::string>& headers = rows.front();
    const std::size_t columns = headers.size();

    std::vector<bool> right_aligned(columns, false);
    if (rows.size() > 1) {
      for (std::size_t col = 0; col < columns; ++col) {
        std::vector<std::string> values;
        values.reserve(rows.size() - 1);
        for (std::size_t row = 1; row < rows.size(); ++row) {
          values.push_back(col < rows[row].size() ? rows[row][col] : std::string());
        }
        right_aligned[col] = column_is_numeric(values);
      }
    }

    std::vector<int> widths(columns, 0);
    for (std::size_t col = 0; col < columns; ++col) {
      widths[col] = display_width(headers[col]) + kMinTablePadding;
      for (std::size_t row = 1; row < rows.size(); ++row) {
        if (col >= rows[row].size()) continue;
        widths[col] = std::max(widths[col], display_width(stripped(rows[row][col])));
      }
    }

    const auto pad = [](const std::string& cell, int width, bool right) {
      const int fill = std::max(width - display_width(cell), 0);
      return right ? std::string(static_cast<std::size_t>(fill), ' ') + cell
                   : cell + std::string(static_cast<std::size_t>(fill), ' ');
    };
    const auto build_row = [&](const std::vector<std::string>& cells, bool strip) {
      std::string line = "|";
      for (std::size_t col = 0; col < columns; ++col) {
        const std::string cell =
            col < cells.size() ? (strip ? stripped(cells[col]) : cells[col])
                               : std::string();
        line.append(" ");
        line.append(pad(cell, widths[col], right_aligned[col]));
        line.append(" |");
      }
      return line;
    };

    std::vector<std::string> lines;
    lines.push_back(build_row(headers, false));
    std::string rule = "|";
    for (std::size_t col = 0; col < columns; ++col) {
      rule.append(static_cast<std::size_t>(widths[col]) + 2, '-');
      rule.push_back('|');
    }
    lines.push_back(rule);
    for (std::size_t row = 1; row < rows.size(); ++row) {
      lines.push_back(build_row(rows[row], true));
    }
    return join(lines, "\n");
  }

  // -- pictures -------------------------------------------------------------

  std::string serialize_picture(const std::string& ref,
                                const docv1::PictureItem& picture,
                                std::string* first_span) {
    std::vector<std::string> parts;
    std::string caption_span;
    parts.push_back(serialize_captions(picture.captions(), &caption_span));
    if (!parts.back().empty()) *first_span = caption_span;
    // The default image mode positions every picture with a placeholder,
    // whatever image the item carries.
    if (!excluded(ref)) {
      parts.emplace_back(kImagePlaceholder);
      if (first_span->empty()) *first_span = ref;
    }
    return join(parts, "\n\n");
  }

  // -- meta -----------------------------------------------------------------

  std::string serialize_meta(const docv1::BaseMeta& meta, MetaShape shape) {
    std::vector<std::string> parts;
    append_base_meta(meta, &parts);
    static_cast<void>(shape);
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  std::string serialize_meta_floating(const docv1::FloatingMeta& meta) {
    std::vector<std::string> parts;
    append_inherited_meta(meta, &parts);
    if (meta.has_description() && !meta.description().text().empty()) {
      parts.push_back(meta.description().text());
    }
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  std::string serialize_picture_meta(const docv1::PictureMeta& meta) {
    std::vector<std::string> parts;
    append_inherited_meta(meta, &parts);
    if (meta.has_description() && !meta.description().text().empty()) {
      parts.push_back(meta.description().text());
    }
    if (meta.has_classification()) {
      const std::string main = main_classification(meta.classification());
      if (!main.empty()) parts.push_back(humanized(main));
    }
    if (meta.has_molecule() && !meta.molecule().smi().empty()) {
      parts.push_back(meta.molecule().smi());
    }
    if (meta.has_tabular_chart()) {
      const std::string table = stripped(table_markdown(meta.tabular_chart().chart_data()));
      if (!table.empty()) parts.push_back(table);
    }
    if (meta.has_code()) parts.push_back(code_meta_repr(meta.code()));
    append_custom_fields(meta.custom_fields(), &parts);
    return join(parts, "\n\n");
  }

  // The declaration order the reference iterates: the inherited fields
  // first, then the shape's own, then the custom part.
  template <typename Meta>
  void append_inherited_meta(const Meta& meta, std::vector<std::string>* parts) {
    if (meta.has_summary() && !meta.summary().text().empty()) {
      parts->push_back(meta.summary().text());
    }
    if (meta.has_language()) parts->push_back(language_meta_repr(meta.language()));
    if (meta.has_entities()) parts->push_back(entities_meta_repr(meta.entities()));
    if (meta.has_keywords()) {
      parts->push_back(join_values(meta.keywords().values()));
    }
    if (meta.has_topics()) {
      parts->push_back(join_values(meta.topics().values()));
    }
  }

  void append_base_meta(const docv1::BaseMeta& meta, std::vector<std::string>* parts) {
    append_inherited_meta(meta, parts);
  }

  // The model's keyword and topic lists are unique lists: a repeat drops on
  // load, keeping the first occurrence.
  static std::string join_values(
      const google::protobuf::RepeatedPtrField<std::string>& values) {
    std::string out;
    std::set<std::string_view> seen;
    for (const auto& value : values) {
      if (!seen.insert(value).second) continue;
      if (!out.empty()) out.append(", ");
      out.append(value);
    }
    return out;
  }

  static std::string main_classification(
      const docv1::PictureClassificationMetaField& classification) {
    const docv1::PictureClassificationPrediction* best = nullptr;
    double best_confidence = 0.0;
    for (const auto& prediction : classification.predictions()) {
      if (!prediction.has_confidence()) continue;
      if (best == nullptr || prediction.confidence() > best_confidence) {
        best = &prediction;
        best_confidence = prediction.confidence();
      }
    }
    if (best == nullptr && !classification.predictions().empty()) {
      best = &classification.predictions(0);
    }
    return best != nullptr ? best->class_name() : std::string();
  }

  // The custom part of a meta block, in the shared export order. Only the
  // value renders; the name is what fixes the order.
  void append_custom_fields(
      const google::protobuf::Map<std::string, google::protobuf::Value>& fields,
      std::vector<std::string>* parts) {
    for (const auto& [key, value] : ordered_custom_fields(fields)) {
      if (!value_is_truthy(*value)) continue;
      parts->push_back(value_str(*value));
    }
  }
};

}  // namespace

std::string render_markdown(const docv1::Document& document) {
  // Only the list-item migration can change the Markdown; the box clamping
  // the model also applies on load is invisible here, so the defensive copy
  // is taken only when a list item actually needs re-homing.
  if (!render::has_misplaced_list_items(document)) {
    return MarkdownRenderer(document).render();
  }
  docv1::Document normalized = document;
  render::migrate_misplaced_list_items(&normalized);
  return MarkdownRenderer(normalized).render();
}

}  // namespace grparse
