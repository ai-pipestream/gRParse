#include "renderer_base.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {

ArenaRef parse_ref(const std::string& ref) {
  static const std::vector<std::pair<std::string, ArenaRef::Kind>> kArenas{
      {"#/texts/", ArenaRef::kText},
      {"#/tables/", ArenaRef::kTable},
      {"#/pictures/", ArenaRef::kPicture},
      {"#/groups/", ArenaRef::kGroup},
      {"#/key_value_items/", ArenaRef::kKeyValue},
      {"#/form_items/", ArenaRef::kForm},
      {"#/field_regions/", ArenaRef::kFieldRegion},
      {"#/field_items/", ArenaRef::kFieldItem},
  };
  for (const auto& [prefix, kind] : kArenas) {
    if (!ref.starts_with(prefix)) continue;
    const std::string digits = ref.substr(prefix.size());
    // Nine digits keeps the index inside int range; anything longer cannot
    // name a real arena entry and resolves to kUnknown like other malformed
    // references.
    if (digits.empty() || digits.size() > 9 ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
      return {};
    }
    return {kind, std::stoi(digits)};
  }
  return {};
}

const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading: return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

int heading_rank(int level) {
  return std::min(std::max(level, 1) + 1, 6);
}

std::string trimmed(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n\f\v");
  if (begin == std::string::npos) return std::string();
  const auto end = text.find_last_not_of(" \t\r\n\f\v");
  return text.substr(begin, end - begin + 1);
}

std::string code_fence_language(const docv1::CodeItem& code) {
  if (code.has_code_language_raw()) return code.code_language_raw();
  switch (code.code_language()) {
    case docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED:
    case docv1::CODE_LANGUAGE_LABEL_UNKNOWN:
      return std::string();
    case docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS: return "cpp";
    case docv1::CODE_LANGUAGE_LABEL_C_SHARP: return "csharp";
    default: break;
  }
  std::string name = docv1::CodeLanguageLabel_Name(code.code_language());
  static const std::string kPrefix = "CODE_LANGUAGE_LABEL_";
  if (name.starts_with(kPrefix)) name = name.substr(kPrefix.size());
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name;
}

std::vector<std::vector<const docv1::TableCell*>> table_grid(
    const docv1::TableData& data) {
  std::vector<std::vector<const docv1::TableCell*>> grid;
  if (!data.grid().empty()) {
    grid.reserve(data.grid_size());
    for (const auto& row : data.grid()) {
      std::vector<const docv1::TableCell*> cells;
      cells.reserve(row.cells_size());
      for (const auto& cell : row.cells()) cells.push_back(&cell);
      grid.push_back(std::move(cells));
    }
    return grid;
  }
  const int rows = data.num_rows();
  const int cols = data.num_cols();
  if (rows <= 0 || cols <= 0) return grid;
  grid.assign(static_cast<size_t>(rows),
              std::vector<const docv1::TableCell*>(static_cast<size_t>(cols), nullptr));
  for (const auto& cell : data.table_cells()) {
    const int row_end = std::min(
        rows, std::max(cell.end_row_offset_idx(), cell.start_row_offset_idx() + 1));
    const int col_end = std::min(
        cols, std::max(cell.end_col_offset_idx(), cell.start_col_offset_idx() + 1));
    for (int row = std::max(0, cell.start_row_offset_idx()); row < row_end; ++row) {
      for (int col = std::max(0, cell.start_col_offset_idx()); col < col_end; ++col) {
        grid[static_cast<size_t>(row)][static_cast<size_t>(col)] = &cell;
      }
    }
  }
  return grid;
}

std::vector<std::vector<const docv1::TableCell*>> derived_table_grid(
    const docv1::TableData& data) {
  const int rows = std::max(data.num_rows(), 0);
  const int cols = std::max(data.num_cols(), 0);
  std::vector<std::vector<const docv1::TableCell*>> grid(
      static_cast<std::size_t>(rows),
      std::vector<const docv1::TableCell*>(static_cast<std::size_t>(cols), nullptr));
  const auto wrapped = [](int index, int size) {
    return index < 0 ? index + size : index;
  };
  for (const auto& cell : data.table_cells()) {
    const int row_end = std::min(cell.end_row_offset_idx(), rows);
    const int col_end = std::min(cell.end_col_offset_idx(), cols);
    for (int row = std::min(cell.start_row_offset_idx(), rows); row < row_end; ++row) {
      const int row_at = wrapped(row, rows);
      if (row_at < 0) continue;
      for (int col = std::min(cell.start_col_offset_idx(), cols); col < col_end; ++col) {
        const int col_at = wrapped(col, cols);
        if (col_at < 0) continue;
        grid[static_cast<std::size_t>(row_at)][static_cast<std::size_t>(col_at)] = &cell;
      }
    }
  }
  return grid;
}

namespace {

bool is_special_scheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" ||
         scheme == "wss" || scheme == "ftp" || scheme == "file";
}

}  // namespace

std::string normalized_uri(const std::string& uri) {
  // Scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
  std::size_t colon = std::string::npos;
  if (!uri.empty() && std::isalpha(static_cast<unsigned char>(uri[0]))) {
    for (std::size_t i = 1; i < uri.size(); ++i) {
      const char c = uri[i];
      if (c == ':') {
        colon = i;
        break;
      }
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' &&
          c != '.') {
        break;
      }
    }
  }
  if (colon == std::string::npos) return uri;

  std::string scheme = uri.substr(0, colon);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string rest = uri.substr(colon + 1);
  if (!is_special_scheme(scheme) || !rest.starts_with("//")) {
    return scheme + ":" + rest;
  }

  const std::size_t authority_start = 2;
  std::size_t authority_end = rest.find_first_of("/?#", authority_start);
  if (authority_end == std::string::npos) authority_end = rest.size();
  std::string authority =
      rest.substr(authority_start, authority_end - authority_start);
  // Lowercase the host: the part after any userinfo and before any port.
  const std::size_t host_start =
      authority.rfind('@') == std::string::npos ? 0 : authority.rfind('@') + 1;
  std::size_t host_end = authority.find(':', host_start);
  if (host_end == std::string::npos) host_end = authority.size();
  std::transform(authority.begin() + static_cast<std::ptrdiff_t>(host_start),
                 authority.begin() + static_cast<std::ptrdiff_t>(host_end),
                 authority.begin() + static_cast<std::ptrdiff_t>(host_start),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string tail = rest.substr(authority_end);
  if (tail.empty() || tail[0] == '?' || tail[0] == '#') tail.insert(0, "/");
  return scheme + "://" + authority + tail;
}

namespace {

// The canonical spelling of every tag the vocabulary names. A tag the table
// does not list (unset, unspecified, or one this build does not know) has no
// canonical spelling and reports none.
struct CodeLanguageName {
  docv1::CodeLanguageLabel tag;
  std::string_view name;
};

constexpr CodeLanguageName kCodeLanguageNames[] = {
    {docv1::CODE_LANGUAGE_LABEL_ADA, "Ada"},
    {docv1::CODE_LANGUAGE_LABEL_AWK, "Awk"},
    {docv1::CODE_LANGUAGE_LABEL_BASH, "Bash"},
    {docv1::CODE_LANGUAGE_LABEL_BC, "bc"},
    {docv1::CODE_LANGUAGE_LABEL_C, "C"},
    {docv1::CODE_LANGUAGE_LABEL_C_SHARP, "C#"},
    {docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS, "C++"},
    {docv1::CODE_LANGUAGE_LABEL_CMAKE, "CMake"},
    {docv1::CODE_LANGUAGE_LABEL_COBOL, "COBOL"},
    {docv1::CODE_LANGUAGE_LABEL_CSS, "CSS"},
    {docv1::CODE_LANGUAGE_LABEL_CEYLON, "Ceylon"},
    {docv1::CODE_LANGUAGE_LABEL_CLOJURE, "Clojure"},
    {docv1::CODE_LANGUAGE_LABEL_CRYSTAL, "Crystal"},
    {docv1::CODE_LANGUAGE_LABEL_CUDA, "Cuda"},
    {docv1::CODE_LANGUAGE_LABEL_CYTHON, "Cython"},
    {docv1::CODE_LANGUAGE_LABEL_D, "D"},
    {docv1::CODE_LANGUAGE_LABEL_DART, "Dart"},
    {docv1::CODE_LANGUAGE_LABEL_DC, "dc"},
    {docv1::CODE_LANGUAGE_LABEL_DOCKERFILE, "Dockerfile"},
    {docv1::CODE_LANGUAGE_LABEL_ELIXIR, "Elixir"},
    {docv1::CODE_LANGUAGE_LABEL_ERLANG, "Erlang"},
    {docv1::CODE_LANGUAGE_LABEL_FORTRAN, "FORTRAN"},
    {docv1::CODE_LANGUAGE_LABEL_FORTH, "Forth"},
    {docv1::CODE_LANGUAGE_LABEL_GO, "Go"},
    {docv1::CODE_LANGUAGE_LABEL_HTML, "HTML"},
    {docv1::CODE_LANGUAGE_LABEL_HASKELL, "Haskell"},
    {docv1::CODE_LANGUAGE_LABEL_HAXE, "Haxe"},
    {docv1::CODE_LANGUAGE_LABEL_JAVA, "Java"},
    {docv1::CODE_LANGUAGE_LABEL_JAVASCRIPT, "JavaScript"},
    {docv1::CODE_LANGUAGE_LABEL_JSON, "JSON"},
    {docv1::CODE_LANGUAGE_LABEL_JULIA, "Julia"},
    {docv1::CODE_LANGUAGE_LABEL_KOTLIN, "Kotlin"},
    {docv1::CODE_LANGUAGE_LABEL_LISP, "Lisp"},
    {docv1::CODE_LANGUAGE_LABEL_LUA, "Lua"},
    {docv1::CODE_LANGUAGE_LABEL_MATLAB, "Matlab"},
    {docv1::CODE_LANGUAGE_LABEL_MOONSCRIPT, "MoonScript"},
    {docv1::CODE_LANGUAGE_LABEL_NIM, "Nim"},
    {docv1::CODE_LANGUAGE_LABEL_OCAML, "OCaml"},
    {docv1::CODE_LANGUAGE_LABEL_OBJECTIVEC, "ObjectiveC"},
    {docv1::CODE_LANGUAGE_LABEL_OCTAVE, "Octave"},
    {docv1::CODE_LANGUAGE_LABEL_PHP, "PHP"},
    {docv1::CODE_LANGUAGE_LABEL_PASCAL, "Pascal"},
    {docv1::CODE_LANGUAGE_LABEL_PERL, "Perl"},
    {docv1::CODE_LANGUAGE_LABEL_PROLOG, "Prolog"},
    {docv1::CODE_LANGUAGE_LABEL_PYTHON, "Python"},
    {docv1::CODE_LANGUAGE_LABEL_RACKET, "Racket"},
    {docv1::CODE_LANGUAGE_LABEL_RUBY, "Ruby"},
    {docv1::CODE_LANGUAGE_LABEL_RUST, "Rust"},
    {docv1::CODE_LANGUAGE_LABEL_SML, "SML"},
    {docv1::CODE_LANGUAGE_LABEL_SQL, "SQL"},
    {docv1::CODE_LANGUAGE_LABEL_SCALA, "Scala"},
    {docv1::CODE_LANGUAGE_LABEL_SCHEME, "Scheme"},
    {docv1::CODE_LANGUAGE_LABEL_SWIFT, "Swift"},
    {docv1::CODE_LANGUAGE_LABEL_TYPESCRIPT, "TypeScript"},
    {docv1::CODE_LANGUAGE_LABEL_UNKNOWN, "unknown"},
    {docv1::CODE_LANGUAGE_LABEL_VISUALBASIC, "VisualBasic"},
    {docv1::CODE_LANGUAGE_LABEL_XML, "XML"},
    {docv1::CODE_LANGUAGE_LABEL_YAML, "YAML"},
    {docv1::CODE_LANGUAGE_LABEL_LATEX, "Latex"},
    {docv1::CODE_LANGUAGE_LABEL_TIKZ, "Tikz"},
    {docv1::CODE_LANGUAGE_LABEL_DOCLANG, "DocLang"},
};

}  // namespace

std::optional<std::string_view> code_language_string(docv1::CodeLanguageLabel tag) {
  for (const auto& [known, name] : kCodeLanguageNames) {
    if (known == tag) return name;
  }
  return std::nullopt;
}

// The BCP-47 code is the lowercase suffix of the proto enum name.
std::optional<std::string> human_language_string(docv1::HumanLanguageLabel tag) {
  if (tag == docv1::HUMAN_LANGUAGE_LABEL_UNSPECIFIED ||
      !docv1::HumanLanguageLabel_IsValid(tag)) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "HUMAN_LANGUAGE_LABEL_";
  std::string name = docv1::HumanLanguageLabel_Name(tag);
  if (!name.starts_with(prefix)) return std::nullopt;
  std::string code = name.substr(prefix.size());
  std::transform(code.begin(), code.end(), code.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return code;
}

namespace {

// True when the key already satisfies the namespace__field_name rule.
bool conforming_custom_name(std::string_view key) {
  const std::size_t pos = key.find("__");
  return pos != std::string_view::npos && pos > 0 && pos + 2 < key.size();
}

}  // namespace

std::vector<std::pair<std::string, const google::protobuf::Value*>>
ordered_custom_fields(
    const google::protobuf::Map<std::string, google::protobuf::Value>& fields) {
  std::vector<std::pair<std::string, const google::protobuf::Value*>> entries;
  entries.reserve(fields.size());
  for (const auto& [key, value] : fields) entries.emplace_back(key, &value);
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<std::string> taken;
  taken.reserve(entries.size());
  for (const auto& entry : entries) taken.push_back(entry.first);
  for (auto& [key, value] : entries) {
    if (conforming_custom_name(key)) continue;
    std::string base = "pipestream__";
    for (const char c : key) {
      const auto byte = static_cast<unsigned char>(c);
      // One replacement per character: UTF-8 continuation bytes fold into
      // their lead byte's underscore.
      if ((byte & 0xc0) == 0x80) continue;
      const bool word = (byte < 0x80 && std::isalnum(byte)) || c == '_';
      base.push_back(word ? c : '_');
    }
    std::string candidate = base;
    int suffix = 2;
    while (std::find(taken.begin(), taken.end(), candidate) != taken.end()) {
      candidate = base + "_" + std::to_string(suffix++);
    }
    *std::find(taken.begin(), taken.end(), key) = candidate;
    key = std::move(candidate);
  }

  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::erase_if(entries, [](const auto& entry) {
    // A null payload vanishes under the dump's exclude-none rule; nulls
    // nested inside values survive.
    return entry.second->kind_case() == google::protobuf::Value::kNullValue ||
           entry.second->kind_case() == google::protobuf::Value::KIND_NOT_SET;
  });
  return entries;
}

std::string escape_html_text(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': safe.append("&amp;"); break;
      case '<': safe.append("&lt;"); break;
      case '>': safe.append("&gt;"); break;
      default: safe.push_back(c);
    }
  }
  return safe;
}

std::string escape_html_attribute(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': safe.append("&amp;"); break;
      case '<': safe.append("&lt;"); break;
      case '>': safe.append("&gt;"); break;
      case '"': safe.append("&quot;"); break;
      default: safe.push_back(c);
    }
  }
  return safe;
}

std::string picture_description(const docv1::PictureItem& picture) {
  if (picture.has_meta() && picture.meta().has_description()) {
    return picture.meta().description().text();
  }
  for (const auto& annotation : picture.annotations()) {
    if (annotation.has_description()) return annotation.description().text();
  }
  return std::string();
}

std::string picture_classification_class(const docv1::PictureItem& picture) {
  if (picture.has_meta() && picture.meta().has_classification()) {
    // The reference rule: the highest confidence among predictions that
    // carry one; when none does, the first prediction by convention.
    const auto& predictions = picture.meta().classification().predictions();
    const docv1::PictureClassificationPrediction* best = nullptr;
    for (const auto& prediction : predictions) {
      if (!prediction.has_confidence()) continue;
      if (best == nullptr || prediction.confidence() > best->confidence()) {
        best = &prediction;
      }
    }
    if (best == nullptr && !predictions.empty()) best = &predictions[0];
    return best != nullptr ? best->class_name() : std::string();
  }
  for (const auto& annotation : picture.annotations()) {
    if (annotation.has_classification() &&
        !annotation.classification().predicted_classes().empty()) {
      return annotation.classification().predicted_classes(0).class_name();
    }
  }
  return std::string();
}

std::vector<const docv1::TextItemBase*> RendererBase::caption_bases(
    const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions) {
  std::vector<const docv1::TextItemBase*> bases;
  for (const auto& ref : captions) {
    const ArenaRef resolved = parse_ref(ref.ref());
    if (resolved.kind != ArenaRef::kText ||
        resolved.index >= document_.texts_size()) {
      continue;
    }
    consumed_.insert(ref.ref());
    const auto* base = text_base(document_.texts(resolved.index));
    if (base != nullptr) bases.push_back(base);
  }
  return bases;
}

std::vector<std::string> RendererBase::caption_texts(
    const google::protobuf::RepeatedPtrField<docv1::RefItem>& captions) {
  std::vector<std::string> texts;
  for (const auto* base : caption_bases(captions)) {
    if (!base->text().empty()) texts.push_back(base->text());
  }
  return texts;
}

}  // namespace grparse::render
