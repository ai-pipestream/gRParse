// Proves the chart-extraction policy settings as the server reads them at
// startup: the five GRPARSE_*CHART_EXTRACTION* variables (lists as JSON or
// comma-separated, custom presets as typed JSON), the registry docling-jobkit
// builds from them (default, allowed built-ins, custom presets), and every
// startup rejection naming its variable.

#include <map>
#include <stdexcept>
#include <string>

#include "grparse/chart_extraction_policy.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::ChartExtractionPolicy policy_with(std::map<std::string, std::string> environment) {
  return grparse::chart_extraction_policy_from_env([&](const char* name) -> const char* {
    const auto found = environment.find(name);
    return found == environment.end() ? nullptr : found->second.c_str();
  });
}

void require_startup_failure(std::map<std::string, std::string> environment,
                             const std::string& fragment) {
  try {
    policy_with(std::move(environment));
  } catch (const std::invalid_argument& error) {
    require(std::string(error.what()).contains(fragment), error.what());
    return;
  }
  require(false, "expected a startup failure mentioning: " + fragment);
}

void verify_defaults_match_docling_serve() {
  const grparse::ChartExtractionPolicy policy = policy_with({});
  require(policy.settings().default_preset == "granite_vision_v4",
          "default_chart_extraction_preset is granite_vision_v4");
  require(!policy.settings().allowed_presets.has_value() &&
              !policy.settings().allowed_engines.has_value() &&
              policy.settings().custom_presets.empty() && !policy.settings().allow_custom_config,
          "every built-in and engine allowed, no custom presets, custom config off");
  require(policy.preset_ids() == std::vector<std::string>{"default", "granite_vision_v4"},
          "the registry lists default first");
  const auto empty = policy_with({{"GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", "  "},
                                  {"GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG", ""}});
  require(!empty.settings().allowed_presets.has_value() &&
              !empty.settings().allow_custom_config,
          "an empty variable counts as unset");
}

void verify_lists_parse_as_json_or_comma_separated() {
  const auto json = policy_with(
      {{"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", R"(["api_openai", "api_ollama"])"}});
  require(json.settings().allowed_engines ==
              std::vector<std::string>{"api_openai", "api_ollama"},
          "a JSON array parses");
  const auto csv =
      policy_with({{"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", "api_openai, api_ollama"}});
  require(csv.settings().allowed_engines == json.settings().allowed_engines,
          "the comma form gives the same list, trimmed");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", "api_openai,,api"}},
                          "GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES has an empty entry");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", "openai"}},
                          "names 'openai', which is not a known engine");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", "[1, 2]"}},
                          "must list non-empty strings only");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", "[\"a\""}},
                          "is not a JSON array");
}

void verify_custom_presets_are_typed() {
  const auto presets = grparse::parse_chart_extraction_presets(
      R"({"full":{"model":"granite-vision-4.1-4b","url":"http://vlm:8000",)"
      R"("engine_type":"api_ollama","chart2summary":true,"chart2code":true,)"
      R"("use_natural_language_prompts":true}})");
  require(presets.size() == 1, "one preset");
  const grparse::ChartExtractionPreset& full = presets.at("full");
  require(full.id == "full" && full.model == "granite-vision-4.1-4b" &&
              full.vlm_endpoint == "http://vlm:8000" && full.engine == "api_ollama" &&
              full.chart2csv && full.chart2summary && full.chart2code &&
              full.use_natural_language_prompts,
          "every key lands in its typed field; chart2csv defaults on");
  const auto bare = grparse::parse_chart_extraction_presets(R"({"bare":{}})");
  require(bare.at("bare").engine == "api_openai" && bare.at("bare").model.empty() &&
              bare.at("bare").chart2csv && !bare.at("bare").chart2summary,
          "an empty preset is csv only on the endpoint's default model");

  const auto rejects = [](const std::string& json, const std::string& fragment) {
    try {
      grparse::parse_chart_extraction_presets(json);
    } catch (const std::invalid_argument& error) {
      require(std::string(error.what()).contains(fragment), error.what());
      return;
    }
    require(false, "expected a rejection mentioning: " + fragment);
  };
  rejects("not json", "is not a JSON object");
  rejects(R"({"p":3})", "preset 'p' must be a JSON object");
  rejects(R"({"p":{"modle":"x"}})", "unknown key 'modle'");
  rejects(R"({"p":{"chart2csv":"yes"}})", "chart2csv must be true or false");
  rejects(R"({"p":{"model":1}})", "model must be a string");
  rejects(R"({"p":{"engine_type":"torch"}})", "engine_type 'torch' is not a known engine");
  rejects(R"({"p":{"chart2csv":false}})", "at least one of chart2csv, chart2summary");
}

void verify_registry_follows_jobkit() {
  // A custom preset named as the default wins, and is reachable by name.
  const auto custom_default = policy_with(
      {{"GRPARSE_DEFAULT_CHART_EXTRACTION_PRESET", "mine"},
       {"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS", R"({"mine":{"chart2summary":true}})"}});
  const auto resolved = custom_default.resolve("");
  require(resolved.has_value() && resolved->id == "mine" && resolved->chart2summary,
          "the default resolves to the custom preset");
  require(custom_default.resolve("mine").has_value() &&
              custom_default.resolve("granite_vision_v4").has_value(),
          "custom and built-in presets are both reachable");

  // An allow list narrows the built-ins; custom presets stay reachable.
  const auto narrowed = policy_with(
      {{"GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", "mine"},
       {"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS", R"({"mine":{}})"}});
  require(!narrowed.resolve("granite_vision_v4").has_value() &&
              narrowed.resolve("default").has_value() && narrowed.resolve("mine").has_value(),
          "an allow list without the built-in hides it but keeps default and custom presets");
  require(narrowed.resolve("granite_vision_v4").error().error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT,
          "a hidden preset is INVALID_ARGUMENT");

  // A custom preset may replace a built-in of the same id.
  const auto replaced = policy_with({{"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS",
                                      R"({"granite_vision_v4":{"url":"http://granite:9"}})"}});
  require(replaced.resolve("granite_vision_v4")->vlm_endpoint == "http://granite:9",
          "the admin's preset wins over the built-in of the same id");
}

void verify_startup_rejections_name_the_setting() {
  require_startup_failure({{"GRPARSE_DEFAULT_CHART_EXTRACTION_PRESET", "missing"}},
                          "default preset 'missing' is neither a built-in nor a custom preset");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_PRESETS", "granite_vision"}},
                          "allowed preset 'granite_vision' is neither");
  require_startup_failure({{"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", "api_ollama"}},
                          "uses engine 'api_openai', which the allowed engines exclude");
  require_startup_failure({{"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS", R"({"default":{}})"}},
                          "custom preset id 'default' is reserved");
  require_startup_failure({{"GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG", "yes"}},
                          "GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG must be true or false");
  const auto allowed = policy_with({{"GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG", "true"}});
  require(allowed.settings().allow_custom_config && allowed.check_custom_config("vllm").ok(),
          "the switch opens custom configs for any engine when engines are unrestricted");
}

void verify_engine_names_match_docling() {
  namespace parsev1 = ai::pipestream::parse::v1;
  require(grparse::vlm_engine_name(parsev1::VLM_ENGINE_TYPE_API_OPENAI) == "api_openai" &&
              grparse::vlm_engine_name(parsev1::VLM_ENGINE_TYPE_TRANSFORMERS) == "transformers" &&
              grparse::vlm_engine_name(parsev1::VLM_ENGINE_TYPE_AUTO_INLINE) == "auto_inline" &&
              grparse::vlm_engine_name(parsev1::VLM_ENGINE_TYPE_UNSPECIFIED).empty(),
          "wire engine types map to Docling's VlmEngineType values");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("chart-extraction-policy-test", "all checks passed", {
      verify_defaults_match_docling_serve,
      verify_lists_parse_as_json_or_comma_separated,
      verify_custom_presets_are_typed,
      verify_registry_follows_jobkit,
      verify_startup_rejections_name_the_setting,
      verify_engine_names_match_docling,
  });
}
