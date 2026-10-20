#include "grparse/ocr_preset_policy.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <utility>

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>

#include "setting_values.h"
#include "source_parse.h"

namespace grparse {

namespace parsev1 = ai::pipestream::parse::v1;
using settings::configured;
using settings::parse_list;
using settings::trimmed;

namespace {

constexpr std::array<parsev1::OcrEngineKind, 6> kKinds = {
    parsev1::OCR_ENGINE_KIND_AUTO,      parsev1::OCR_ENGINE_KIND_RAPIDOCR,
    parsev1::OCR_ENGINE_KIND_EASYOCR,   parsev1::OCR_ENGINE_KIND_TESSEROCR,
    parsev1::OCR_ENGINE_KIND_TESSERACT, parsev1::OCR_ENGINE_KIND_OCRMAC};

// The kinds this server runs: RapidOCR, which "auto" also resolves to.
constexpr std::array<parsev1::OcrEngineKind, 2> kServedKinds = {parsev1::OCR_ENGINE_KIND_AUTO,
                                                                parsev1::OCR_ENGINE_KIND_RAPIDOCR};

bool served(parsev1::OcrEngineKind kind) {
  return std::ranges::find(kServedKinds, kind) != kServedKinds.end();
}

class FirstError final : public google::protobuf::io::ErrorCollector {
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

std::string joined(const std::vector<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) out += ", ";
    out += "'" + value + "'";
  }
  return "[" + out + "]";
}

parsev1::OcrEngineKind kind_setting(const char* variable, const std::string& value) {
  const auto kind = ocr_kind_from_name(trimmed(value));
  if (!kind.has_value()) {
    throw std::invalid_argument(std::string(variable) + " names '" + trimmed(value) +
                                "', which is not an OCR kind");
  }
  return *kind;
}

}  // namespace

std::string ocr_kind_name(parsev1::OcrEngineKind kind) {
  switch (kind) {
    case parsev1::OCR_ENGINE_KIND_AUTO: return "auto";
    case parsev1::OCR_ENGINE_KIND_RAPIDOCR: return "rapidocr";
    case parsev1::OCR_ENGINE_KIND_EASYOCR: return "easyocr";
    case parsev1::OCR_ENGINE_KIND_TESSEROCR: return "tesserocr";
    case parsev1::OCR_ENGINE_KIND_TESSERACT: return "tesseract";
    case parsev1::OCR_ENGINE_KIND_OCRMAC: return "ocrmac";
    default: return std::string();
  }
}

std::optional<parsev1::OcrEngineKind> ocr_kind_from_name(std::string_view name) {
  for (const parsev1::OcrEngineKind kind : kKinds) {
    if (ocr_kind_name(kind) == name) return kind;
  }
  return std::nullopt;
}

bool ocr_language_readable(std::string_view language) {
  static constexpr std::string_view kRead[] = {"ch", "chinese", "zh", "en", "english"};
  return std::ranges::find(kRead, language) != std::end(kRead);
}

OcrPresetPolicy::OcrPresetPolicy(OcrPresetPolicySettings settings) : settings_(std::move(settings)) {
  if (!served(settings_.default_preset)) {
    throw std::invalid_argument("GRPARSE_DEFAULT_OCR_PRESET '" +
                                ocr_kind_name(settings_.default_preset) +
                                "' is not an OCR kind this server runs (auto, rapidocr)");
  }
  if (!served(settings_.default_kind)) {
    throw std::invalid_argument("GRPARSE_DEFAULT_OCR_KIND '" +
                                ocr_kind_name(settings_.default_kind) +
                                "' is not an OCR kind this server runs (auto, rapidocr)");
  }
  registry_["default"] = OcrPreset{.id = "default", .kind = settings_.default_preset};
  for (const parsev1::OcrEngineKind kind : kServedKinds) {
    const std::string id = ocr_kind_name(kind);
    registry_[id] = OcrPreset{.id = id, .kind = kind};
  }
  if (settings_.allowed_presets.has_value()) {
    std::set<std::string> allowed(settings_.allowed_presets->begin(),
                                  settings_.allowed_presets->end());
    allowed.insert("default");
    std::erase_if(registry_, [&allowed](const auto& entry) { return !allowed.contains(entry.first); });
  }
  for (const auto& [id, spec] : settings_.custom_presets) {
    const std::string where = "GRPARSE_CUSTOM_OCR_PRESETS preset '" + id + "'";
    if (trimmed(id).empty() || trimmed(id) != id) {
      throw std::invalid_argument("GRPARSE_CUSTOM_OCR_PRESETS has an empty or padded preset id");
    }
    if (ocr_kind_name(spec.kind()).empty()) {
      throw std::invalid_argument(where + " must name a kind");
    }
    for (const std::string& language : spec.lang()) {
      if (!ocr_language_readable(language)) {
        throw std::invalid_argument(where + ": lang '" + language +
                                    "' is not a language the installed OCR models read "
                                    "(en, english, ch, chinese, zh)");
      }
    }
    registry_[id] = OcrPreset{.id = id, .kind = spec.kind()};
  }
}

std::vector<std::string> OcrPresetPolicy::preset_ids() const {
  std::vector<std::string> ids;
  for (const auto& [id, preset] : registry_) ids.push_back(id);
  return ids;
}

std::expected<OcrPreset, grpc::Status> OcrPresetPolicy::resolve(
    const parsev1::ConvertDocumentOptions& options, const std::string& surface) const {
  const std::string requested =
      options.has_ocr_preset() && !options.ocr_preset().empty() ? options.ocr_preset() : "default";
  const auto found = registry_.find(requested);
  if (found == registry_.end()) {
    return std::unexpected(option_rejected(
        "ocr_preset", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED, requested,
        surface + ": ocr_preset '" + requested + "' is not allowed. Allowed values: " +
            joined(preset_ids()) + "."));
  }
  OcrPreset preset = found->second;
  const std::string kind = ocr_kind_name(preset.kind);
  if (preset.kind != settings_.default_kind && settings_.allowed_kinds.has_value() &&
      std::ranges::find(*settings_.allowed_kinds, preset.kind) == settings_.allowed_kinds->end()) {
    std::vector<std::string> names = {ocr_kind_name(settings_.default_kind)};
    for (const auto allowed : *settings_.allowed_kinds) names.push_back(ocr_kind_name(allowed));
    return std::unexpected(option_rejected(
        "ocr_preset", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED, requested,
        surface + ": OCR kind '" + kind + "' (preset " + requested +
            ") is not allowed by administrator. Allowed kinds: " + joined(names) + "."));
  }
  if (!served(preset.kind)) {
    return std::unexpected(option_rejected(
        "ocr_preset", parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED, requested,
        surface + ": OCR kind '" + kind + "' (preset " + requested +
            ") is not available. Available kinds: ['auto', 'rapidocr']."));
  }
  return preset;
}

const OcrPresetPolicy& default_ocr_preset_policy() {
  static const OcrPresetPolicy policy{OcrPresetPolicySettings{}};
  return policy;
}

OcrPresetPolicy ocr_preset_policy_from_env(const std::function<const char*(const char*)>& lookup) {
  OcrPresetPolicySettings settings;
  if (const auto value = configured(lookup, "GRPARSE_DEFAULT_OCR_PRESET")) {
    settings.default_preset = kind_setting("GRPARSE_DEFAULT_OCR_PRESET", *value);
  }
  if (const auto value = configured(lookup, "GRPARSE_DEFAULT_OCR_KIND")) {
    settings.default_kind = kind_setting("GRPARSE_DEFAULT_OCR_KIND", *value);
  }
  if (const auto value = configured(lookup, "GRPARSE_ALLOWED_OCR_PRESETS")) {
    settings.allowed_presets = parse_list("GRPARSE_ALLOWED_OCR_PRESETS", *value);
  }
  if (const auto value = configured(lookup, "GRPARSE_ALLOWED_OCR_KINDS")) {
    settings.allowed_kinds.emplace();
    for (const std::string& name : parse_list("GRPARSE_ALLOWED_OCR_KINDS", *value)) {
      settings.allowed_kinds->push_back(kind_setting("GRPARSE_ALLOWED_OCR_KINDS", name));
    }
  }
  if (const auto value = configured(lookup, "GRPARSE_CUSTOM_OCR_PRESETS")) {
    parsev1::OcrPresetRegistry registry;
    FirstError errors;
    google::protobuf::TextFormat::Parser parser;
    parser.RecordErrorsTo(&errors);
    if (!parser.ParseFromString(*value, &registry)) {
      throw std::invalid_argument(
          "GRPARSE_CUSTOM_OCR_PRESETS is not an OcrPresetRegistry in protobuf text format: " +
          errors.text());
    }
    for (const auto& [id, spec] : registry.presets()) settings.custom_presets.emplace(id, spec);
  }
  try {
    return OcrPresetPolicy(std::move(settings));
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("OCR preset policy: ") + error.what());
  }
}

}  // namespace grparse
