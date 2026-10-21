#include "grparse/chart_extraction_policy.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>

#include "setting_values.h"

namespace grparse {

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

constexpr std::string_view kStage = "Chart extraction";

constexpr std::array<parsev1::VlmEngineType, 8> kEngines = {
    parsev1::VLM_ENGINE_TYPE_TRANSFORMERS, parsev1::VLM_ENGINE_TYPE_MLX,
    parsev1::VLM_ENGINE_TYPE_VLLM,         parsev1::VLM_ENGINE_TYPE_API,
    parsev1::VLM_ENGINE_TYPE_API_OLLAMA,   parsev1::VLM_ENGINE_TYPE_API_LMSTUDIO,
    parsev1::VLM_ENGINE_TYPE_API_OPENAI,   parsev1::VLM_ENGINE_TYPE_AUTO_INLINE};

using settings::configured;
using settings::parse_list;
using settings::trimmed;

}  // namespace

std::map<std::string, ChartExtractionPreset> builtin_chart_extraction_presets() {
  ChartExtractionPreset granite;
  granite.id = "granite_vision_v4";
  granite.model = "granite-vision-4.1-4b";
  granite.engine = parsev1::VLM_ENGINE_TYPE_API_OPENAI;
  return {{granite.id, granite}};
}

ChartExtractionPolicy default_chart_extraction_policy() {
  PresetPolicySettings<ChartExtractionPreset> settings;
  settings.default_preset = "granite_vision_v4";
  return ChartExtractionPolicy(std::string(kStage), builtin_chart_extraction_presets(),
                               std::move(settings));
}

std::optional<parsev1::VlmEngineType> vlm_engine_from_name(std::string_view name) {
  for (const parsev1::VlmEngineType engine : kEngines) {
    if (vlm_engine_name(engine) == name) return engine;
  }
  return std::nullopt;
}

std::string vlm_engine_name(parsev1::VlmEngineType engine) {
  switch (engine) {
    case parsev1::VLM_ENGINE_TYPE_TRANSFORMERS: return "transformers";
    case parsev1::VLM_ENGINE_TYPE_MLX: return "mlx";
    case parsev1::VLM_ENGINE_TYPE_VLLM: return "vllm";
    case parsev1::VLM_ENGINE_TYPE_API: return "api";
    case parsev1::VLM_ENGINE_TYPE_API_OLLAMA: return "api_ollama";
    case parsev1::VLM_ENGINE_TYPE_API_LMSTUDIO: return "api_lmstudio";
    case parsev1::VLM_ENGINE_TYPE_API_OPENAI: return "api_openai";
    case parsev1::VLM_ENGINE_TYPE_AUTO_INLINE: return "auto_inline";
    default: return std::string();
  }
}

std::map<std::string, ChartExtractionPreset> chart_extraction_presets_from_registry(
    const parsev1::ChartExtractionPresetRegistry& registry) {
  constexpr const char* kVariable = "GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS";
  std::map<std::string, ChartExtractionPreset> presets;
  for (const auto& [id, spec] : registry.presets()) {
    const std::string where = std::string(kVariable) + " preset '" + id + "'";
    if (trimmed(id).empty() || trimmed(id) != id) {
      throw std::invalid_argument(std::string(kVariable) + " has an empty or padded preset id");
    }
    ChartExtractionPreset preset;
    preset.id = id;
    preset.model = spec.model();
    preset.vlm_endpoint = spec.url();
    if (spec.has_engine_type()) {
      if (vlm_engine_name(spec.engine_type()).empty()) {
        throw std::invalid_argument(where + ": engine_type " +
                                    parsev1::VlmEngineType_Name(spec.engine_type()) +
                                    " is not a usable engine");
      }
      preset.engine = spec.engine_type();
    }
    preset.chart2csv = !spec.has_chart2csv() || spec.chart2csv();
    preset.chart2summary = spec.has_chart2summary() && spec.chart2summary();
    preset.chart2code = spec.has_chart2code() && spec.chart2code();
    preset.use_natural_language_prompts =
        spec.has_use_natural_language_prompts() && spec.use_natural_language_prompts();
    if (!preset.chart2csv && !preset.chart2summary && !preset.chart2code) {
      throw std::invalid_argument(where +
                                  ": at least one of chart2csv, chart2summary, or chart2code "
                                  "must be true");
    }
    presets.emplace(id, std::move(preset));
  }
  return presets;
}

namespace {

// Collects the first text-format parse error so the startup failure can
// name it.
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

}  // namespace

std::map<std::string, ChartExtractionPreset> parse_chart_extraction_presets(
    std::string_view text) {
  constexpr const char* kVariable = "GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS";
  parsev1::ChartExtractionPresetRegistry registry;
  FirstError errors;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&errors);
  if (!parser.ParseFromString(std::string(text), &registry)) {
    throw std::invalid_argument(std::string(kVariable) +
                                " is not a ChartExtractionPresetRegistry in protobuf text "
                                "format: " +
                                errors.text());
  }
  return chart_extraction_presets_from_registry(registry);
}

ChartExtractionPolicy chart_extraction_policy_from_env(
    const std::function<const char*(const char*)>& lookup) {
  PresetPolicySettings<ChartExtractionPreset> settings;
  settings.default_preset =
      configured(lookup, "GRPARSE_DEFAULT_CHART_EXTRACTION_PRESET")
          .transform([](const std::string& value) { return trimmed(value); })
          .value_or("granite_vision_v4");
  if (const auto allowed = configured(lookup, "GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS")) {
    settings.allowed_presets = parse_list("GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", *allowed);
  }
  if (const auto custom = configured(lookup, "GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS")) {
    settings.custom_presets = parse_chart_extraction_presets(*custom);
  }
  if (const auto engines = configured(lookup, "GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES")) {
    settings.allowed_engines.emplace();
    for (const std::string& name :
         parse_list("GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", *engines)) {
      const auto engine = vlm_engine_from_name(name);
      if (!engine.has_value()) {
        throw std::invalid_argument("GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES names '" + name +
                                    "', which is not a known engine");
      }
      settings.allowed_engines->push_back(*engine);
    }
  }
  if (const auto allow = configured(lookup, "GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG")) {
    const std::string value = trimmed(*allow);
    if (value != "true" && value != "false") {
      throw std::invalid_argument(
          "GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG must be true or false");
    }
    settings.allow_custom_config = value == "true";
  }
  try {
    return ChartExtractionPolicy(std::string(kStage), builtin_chart_extraction_presets(),
                                 std::move(settings));
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("chart extraction policy: ") + error.what());
  }
}

ChartExtractionPreset chart_preset_from_custom_config(
    const parsev1::ChartExtractionVlmEngineOptions& config) {
  ChartExtractionPreset preset;
  preset.id = "custom";
  const parsev1::VlmEngineType engine = config.engine_options().engine_type();
  preset.engine = engine;
  // Docling's VlmModelSpec.get_api_params: {"model": default_repo_id},
  // overridden by the engine's api_overrides entry.
  preset.model = config.model_spec().default_repo_id();
  for (const parsev1::ApiModelConfigEntry& entry : config.model_spec().api_overrides()) {
    if (entry.engine_type() != engine) continue;
    const auto model = entry.config().params().find("model");
    if (model != entry.config().params().end() && model->second.has_string_value()) {
      preset.model = model->second.string_value();
    }
  }
  preset.chart2csv = !config.has_chart2csv() || config.chart2csv();
  preset.chart2summary = config.has_chart2summary() && config.chart2summary();
  preset.chart2code = config.has_chart2code() && config.chart2code();
  preset.use_natural_language_prompts =
      config.has_use_natural_language_prompts() && config.use_natural_language_prompts();
  return preset;
}

}  // namespace grparse
