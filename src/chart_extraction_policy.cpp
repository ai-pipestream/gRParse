#include "grparse/chart_extraction_policy.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

namespace grparse {

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

constexpr std::string_view kStage = "Chart extraction";

constexpr std::array<std::string_view, 8> kEngineNames = {
    "transformers", "mlx", "vllm", "api", "api_ollama", "api_lmstudio", "api_openai",
    "auto_inline"};

bool known_engine(std::string_view name) {
  return std::ranges::find(kEngineNames, name) != kEngineNames.end();
}

std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::string();
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

// A list setting: a JSON array of strings, or comma-separated words.
std::vector<std::string> parse_list(const char* variable, std::string_view value) {
  std::vector<std::string> items;
  const std::string text = trimmed(value);
  if (text.starts_with('[')) {
    google::protobuf::ListValue list;
    if (!google::protobuf::util::JsonStringToMessage(text, &list).ok()) {
      throw std::invalid_argument(std::string(variable) + " is not a JSON array");
    }
    for (const google::protobuf::Value& entry : list.values()) {
      if (!entry.has_string_value() || trimmed(entry.string_value()).empty()) {
        throw std::invalid_argument(std::string(variable) +
                                    " must list non-empty strings only");
      }
      items.push_back(trimmed(entry.string_value()));
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

std::optional<std::string> configured(const std::function<const char*(const char*)>& lookup,
                                      const char* variable) {
  const char* value = lookup(variable);
  if (value == nullptr || trimmed(value).empty()) return std::nullopt;
  return std::string(value);
}

}  // namespace

std::map<std::string, ChartExtractionPreset> builtin_chart_extraction_presets() {
  ChartExtractionPreset granite;
  granite.id = "granite_vision_v4";
  granite.model = "granite-vision-4.1-4b";
  granite.engine = "api_openai";
  return {{granite.id, granite}};
}

ChartExtractionPolicy default_chart_extraction_policy() {
  PresetPolicySettings<ChartExtractionPreset> settings;
  settings.default_preset = "granite_vision_v4";
  return ChartExtractionPolicy(std::string(kStage), builtin_chart_extraction_presets(),
                               std::move(settings));
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

std::map<std::string, ChartExtractionPreset> parse_chart_extraction_presets(
    std::string_view json) {
  constexpr const char* kVariable = "GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS";
  google::protobuf::Struct root;
  if (!google::protobuf::util::JsonStringToMessage(std::string(json), &root).ok()) {
    throw std::invalid_argument(std::string(kVariable) + " is not a JSON object");
  }
  std::map<std::string, ChartExtractionPreset> presets;
  for (const auto& [id, value] : root.fields()) {
    const std::string where = std::string(kVariable) + " preset '" + id + "'";
    if (trimmed(id).empty() || trimmed(id) != id) {
      throw std::invalid_argument(std::string(kVariable) + " has an empty or padded preset id");
    }
    if (!value.has_struct_value()) {
      throw std::invalid_argument(where + " must be a JSON object");
    }
    ChartExtractionPreset preset;
    preset.id = id;
    for (const auto& [key, field] : value.struct_value().fields()) {
      const auto text = [&]() -> std::string {
        if (!field.has_string_value()) {
          throw std::invalid_argument(where + ": " + key + " must be a string");
        }
        return field.string_value();
      };
      const auto flag = [&]() -> bool {
        if (!field.has_bool_value()) {
          throw std::invalid_argument(where + ": " + key + " must be true or false");
        }
        return field.bool_value();
      };
      if (key == "model") {
        preset.model = text();
      } else if (key == "url") {
        preset.vlm_endpoint = text();
      } else if (key == "engine_type") {
        preset.engine = text();
        if (!known_engine(preset.engine)) {
          throw std::invalid_argument(where + ": engine_type '" + preset.engine +
                                      "' is not a known engine");
        }
      } else if (key == "chart2csv") {
        preset.chart2csv = flag();
      } else if (key == "chart2summary") {
        preset.chart2summary = flag();
      } else if (key == "chart2code") {
        preset.chart2code = flag();
      } else if (key == "use_natural_language_prompts") {
        preset.use_natural_language_prompts = flag();
      } else {
        throw std::invalid_argument(where + ": unknown key '" + key +
                                    "' (known: model, url, engine_type, chart2csv, "
                                    "chart2summary, chart2code, use_natural_language_prompts)");
      }
    }
    if (!preset.chart2csv && !preset.chart2summary && !preset.chart2code) {
      throw std::invalid_argument(where +
                                  ": at least one of chart2csv, chart2summary, or chart2code "
                                  "must be true");
    }
    presets.emplace(id, std::move(preset));
  }
  return presets;
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
    settings.allowed_engines =
        parse_list("GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", *engines);
    for (const std::string& engine : *settings.allowed_engines) {
      if (!known_engine(engine)) {
        throw std::invalid_argument("GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES names '" + engine +
                                    "', which is not a known engine");
      }
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
  preset.engine = vlm_engine_name(engine);
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
