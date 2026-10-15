#pragma once
// The server-side OCR preset policy, with docling-serve's semantics
// (docling-jobkit's ConverterManager OCR registry and docling-serve #723):
//
//   "default"      always present; resolves to GRPARSE_DEFAULT_OCR_PRESET, a
//                  built-in kind ("auto" unless set).
//   built-in kinds the kinds this server runs ("auto", "rapidocr"),
//                  narrowed by GRPARSE_ALLOWED_OCR_PRESETS when set (names
//                  outside the built-in kinds drop out, as upstream).
//   custom presets GRPARSE_CUSTOM_OCR_PRESETS, an OcrPresetRegistry in
//                  protobuf text format. Always selectable, whatever
//                  GRPARSE_ALLOWED_OCR_PRESETS says (#723); a custom id
//                  replaces a built-in of the same name.
//
// A resolved preset's kind is then checked against GRPARSE_ALLOWED_OCR_KINDS
// (GRPARSE_DEFAULT_OCR_KIND is always allowed) and against the kinds this
// server can run. Everything is decided at startup except the per-request
// lookup.
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse {

struct OcrPreset {
  // The registry id the request resolved ("default", "rapidocr", or a
  // custom id).
  std::string id;
  ai::pipestream::parse::v1::OcrEngineKind kind =
      ai::pipestream::parse::v1::OCR_ENGINE_KIND_AUTO;
  // The preset's languages, or the request's ocr_lang when it sets any.
  std::vector<std::string> lang;
};

struct OcrPresetPolicySettings {
  ai::pipestream::parse::v1::OcrEngineKind default_preset =
      ai::pipestream::parse::v1::OCR_ENGINE_KIND_AUTO;
  ai::pipestream::parse::v1::OcrEngineKind default_kind =
      ai::pipestream::parse::v1::OCR_ENGINE_KIND_AUTO;
  std::optional<std::vector<std::string>> allowed_presets;
  std::map<std::string, ai::pipestream::parse::v1::OcrPresetSpec> custom_presets;
  std::optional<std::vector<ai::pipestream::parse::v1::OcrEngineKind>> allowed_kinds;
};

class OcrPresetPolicy {
 public:
  // Throws std::invalid_argument naming the setting when the settings are
  // unusable (a default that is not a kind this server runs, a custom preset
  // without a kind, with a padded id, or with a language the models cannot
  // read).
  explicit OcrPresetPolicy(OcrPresetPolicySettings settings);

  // The preset `options.ocr_preset` names (unset or empty means "default"),
  // with ocr_lang applied. INVALID_ARGUMENT carrying an OptionRejection on
  // "ocr_preset": NOT_ALLOWED for a name outside the registry (docling-serve
  // policy.py wording) or a kind GRPARSE_ALLOWED_OCR_KINDS excludes,
  // NOT_IMPLEMENTED for a kind this server cannot run.
  std::expected<OcrPreset, grpc::Status> resolve(
      const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
      const std::string& surface) const;

  // Registry ids in sorted order (the order docling-serve lists them in).
  std::vector<std::string> preset_ids() const;
  const OcrPresetPolicySettings& settings() const { return settings_; }

 private:
  OcrPresetPolicySettings settings_;
  std::map<std::string, OcrPreset> registry_;
};

// Docling's kind name ("auto", "rapidocr", ...); empty for UNSPECIFIED.
std::string ocr_kind_name(ai::pipestream::parse::v1::OcrEngineKind kind);
std::optional<ai::pipestream::parse::v1::OcrEngineKind> ocr_kind_from_name(std::string_view name);

// Whether the installed models read `language` (PP-OCRv3 "ch": Chinese and
// English).
bool ocr_language_readable(std::string_view language);

// The policy with every setting unset.
const OcrPresetPolicy& default_ocr_preset_policy();

// Reads GRPARSE_DEFAULT_OCR_PRESET, GRPARSE_DEFAULT_OCR_KIND,
// GRPARSE_ALLOWED_OCR_PRESETS, GRPARSE_CUSTOM_OCR_PRESETS and
// GRPARSE_ALLOWED_OCR_KINDS through `lookup`. Throws std::invalid_argument
// naming the variable on any unusable value.
OcrPresetPolicy ocr_preset_policy_from_env(const std::function<const char*(const char*)>& lookup);

}  // namespace grparse
