#include <map>
#include <string>

#include "../src/source_parse.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/chart_extraction_policy.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

constexpr const char* kSurface = "ConvertSource";

void require_invalid(const grpc::Status& status, const std::string& fragment) {
  require_equal(static_cast<int>(status.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "expected INVALID_ARGUMENT, got: " + status.error_message());
  require(status.error_message().contains(fragment), status.error_message());
}

parsev1::ChartExtractionVlmEngineOptions representable_chart_config() {
  parsev1::ChartExtractionVlmEngineOptions config;
  config.mutable_engine_options()->set_engine_type(parsev1::VLM_ENGINE_TYPE_TRANSFORMERS);
  config.mutable_model_spec()->set_name("granite_vision_v4");
  config.mutable_model_spec()->set_default_repo_id("ibm-granite/granite-vision-4.0-chart");
  config.set_output_format(parsev1::CHART_EXTRACTION_OUTPUT_FORMAT_GRANITE_VISION_CHARTS);
  return config;
}

void verify_chart_and_caption_options() {
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("granite_vision_v4");
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    require_invalid(grparse::validate_options(options, kSurface), "mutually exclusive");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("");
    require_invalid(grparse::validate_options(options, kSurface), "chart_extraction_preset is empty");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.mutable_chart_extraction_custom_config();
    require_invalid(grparse::validate_options(options, kSurface), "engine_type is required");
  }
  {
    parsev1::ConvertDocumentOptions options;
    auto* config = options.mutable_chart_extraction_custom_config();
    config->mutable_engine_options()->set_engine_type(parsev1::VLM_ENGINE_TYPE_TRANSFORMERS);
    require_invalid(grparse::validate_options(options, kSurface),
                    "model_spec requires name and default_repo_id");
  }
  {
    // Docling's validator: the outputs are independent, but one must be on.
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_chart2csv(false);
    require_invalid(grparse::validate_options(options, kSurface),
                    "at least one of chart2csv, chart2summary, or chart2code must be true");
  }
  {
    // Summary, code and natural-language prompts all travel to grpc-enrich
    // now, alone or together.
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    auto* config = options.mutable_chart_extraction_custom_config();
    config->set_chart2csv(false);
    config->set_chart2summary(true);
    config->set_chart2code(true);
    config->set_use_natural_language_prompts(true);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), status.error_message());
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_output_format_raw("not-a-format");
    require_invalid(grparse::validate_options(options, kSurface), "disagree");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_caption_placement(parsev1::CAPTION_PLACEMENT_UNSPECIFIED);
    require_invalid(grparse::validate_options(options, kSurface),
                    "caption_placement is unspecified or unknown");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_caption_placement(static_cast<parsev1::CaptionPlacement>(99));
    require_invalid(grparse::validate_options(options, kSurface),
                    "caption_placement is unspecified or unknown");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("granite_vision_v4");
    options.set_caption_placement(parsev1::CAPTION_PLACEMENT_LAYOUT);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), status.error_message());
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.set_caption_placement(parsev1::CAPTION_PLACEMENT_STANDARD);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), status.error_message());
  }
}

void require_code(const grpc::Status& status, grpc::StatusCode code, const std::string& fragment) {
  require_equal(static_cast<int>(status.error_code()), static_cast<int>(code),
                "unexpected status code: " + status.error_message());
  require(status.error_message().contains(fragment), status.error_message());
}

grparse::ChartExtractionPolicy policy_with(
    std::map<std::string, std::string> environment) {
  return grparse::chart_extraction_policy_from_env([&](const char* name) -> const char* {
    const auto found = environment.find(name);
    return found == environment.end() ? nullptr : found->second.c_str();
  });
}

// The server-side chart policy as a Convert request meets it: preset names
// resolve through the registry, custom configs need the admin switch, and
// both obey the engine allow list.
void verify_chart_policy_resolution() {
  const grparse::ChartExtractionPolicy shipped = grparse::default_chart_extraction_policy();
  {
    // Nothing named: the default preset, so the env opt-in leg keeps running.
    parsev1::ConvertDocumentOptions options;
    const auto resolved = grparse::resolve_chart_extraction(options, shipped, kSurface);
    require(resolved.has_value() && resolved->has_value(), "unset resolves the default");
    require((*resolved)->id == "granite_vision_v4" &&
                (*resolved)->model == "granite-vision-4.1-4b" && (*resolved)->chart2csv &&
                !(*resolved)->chart2summary && !(*resolved)->chart2code &&
                !(*resolved)->use_natural_language_prompts,
            "the default is Docling's granite_vision_v4: csv only, special tokens");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_do_chart_extraction(false);
    const auto resolved = grparse::resolve_chart_extraction(options, shipped, kSurface);
    require(resolved.has_value() && !resolved->has_value(),
            "an explicit false with no preset turns charts off");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("default");
    const auto resolved = grparse::resolve_chart_extraction(options, shipped, kSurface);
    require(resolved.has_value() && (*resolved)->id == "granite_vision_v4",
            "\"default\" names the default preset");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("granite_vision");
    const auto resolved = grparse::resolve_chart_extraction(options, shipped, kSurface);
    require(!resolved.has_value(), "an unknown preset is rejected");
    require_code(resolved.error(), grpc::StatusCode::INVALID_ARGUMENT,
                 "ConvertSource: Chart extraction preset 'granite_vision' is not allowed. "
                 "Allowed presets: default, granite_vision_v4");
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    const auto resolved = grparse::resolve_chart_extraction(options, shipped, kSurface);
    require(!resolved.has_value(), "custom configs are off by default");
    require_code(resolved.error(), grpc::StatusCode::PERMISSION_DENIED,
                 "Custom chart extraction configuration is disabled by server policy.");
  }
  const grparse::ChartExtractionPolicy permissive = policy_with({
      {"GRPARSE_ALLOW_CUSTOM_CHART_EXTRACTION_CONFIG", "true"},
      {"GRPARSE_ALLOWED_CHART_EXTRACTION_ENGINES", "api_openai,api"},
      {"GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS",
       R"({"local":{"model":"m","url":"http://vlm:1","engine_type":"vllm"},)"
       R"("summaries":{"chart2csv":false,"chart2summary":true}})"},
  });
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    const auto resolved = grparse::resolve_chart_extraction(options, permissive, kSurface);
    require(!resolved.has_value(), "a transformers custom config is outside the engine list");
    require_code(resolved.error(), grpc::StatusCode::PERMISSION_DENIED,
                 "Engine 'transformers' is not allowed. Allowed engines: api_openai, api");
  }
  {
    parsev1::ConvertDocumentOptions options;
    auto* config = options.mutable_chart_extraction_custom_config();
    *config = representable_chart_config();
    config->mutable_engine_options()->set_engine_type(parsev1::VLM_ENGINE_TYPE_API_OPENAI);
    config->set_chart2summary(true);
    config->set_chart2code(true);
    config->set_use_natural_language_prompts(true);
    auto* override_entry = config->mutable_model_spec()->add_api_overrides();
    override_entry->set_engine_type(parsev1::VLM_ENGINE_TYPE_API_OPENAI);
    (*override_entry->mutable_config()->mutable_params())["model"].set_string_value(
        "granite-chart-served");
    const auto resolved = grparse::resolve_chart_extraction(options, permissive, kSurface);
    require(resolved.has_value() && resolved->has_value(), "an allowed custom config resolves");
    const grparse::ChartExtractionPreset& preset = **resolved;
    require(preset.id == "custom" && preset.engine == "api_openai" &&
                preset.model == "granite-chart-served" && preset.chart2csv &&
                preset.chart2summary && preset.chart2code &&
                preset.use_natural_language_prompts && preset.vlm_endpoint.empty(),
            "the custom config's outputs, prompts and API model name carry through");
  }
  {
    parsev1::ConvertDocumentOptions options;
    auto* config = options.mutable_chart_extraction_custom_config();
    *config = representable_chart_config();
    config->mutable_engine_options()->set_engine_type(parsev1::VLM_ENGINE_TYPE_API);
    const auto resolved = grparse::resolve_chart_extraction(options, permissive, kSurface);
    require(resolved.has_value() &&
                (*resolved)->model == "ibm-granite/granite-vision-4.0-chart",
            "without an api override the model is default_repo_id, as Docling sends it");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("local");
    const auto resolved = grparse::resolve_chart_extraction(options, permissive, kSurface);
    require(!resolved.has_value(), "a custom preset on an excluded engine is refused");
    require_code(resolved.error(), grpc::StatusCode::PERMISSION_DENIED, "Engine 'vllm'");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_chart_extraction_preset("summaries");
    const auto resolved = grparse::resolve_chart_extraction(options, permissive, kSurface);
    require(resolved.has_value() && !(*resolved)->chart2csv && (*resolved)->chart2summary,
            "a custom preset resolves to its own outputs");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("convert-options-test", "ok", {
      verify_chart_and_caption_options,
      verify_chart_policy_resolution,
  });
}
