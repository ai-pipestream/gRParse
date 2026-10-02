#include <string>

#include "../src/source_parse.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
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
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_chart2csv(false);
    require_invalid(grparse::validate_options(options, kSurface), "cannot forward");
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_chart2summary(true);
    require_invalid(grparse::validate_options(options, kSurface), "cannot forward");
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_chart2code(true);
    require_invalid(grparse::validate_options(options, kSurface), "cannot forward");
  }
  {
    parsev1::ConvertDocumentOptions options;
    *options.mutable_chart_extraction_custom_config() = representable_chart_config();
    options.mutable_chart_extraction_custom_config()->set_use_natural_language_prompts(true);
    require_invalid(grparse::validate_options(options, kSurface), "cannot forward");
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

// image_export_mode and doclang_include_namespace reach the DocLang exports;
// EMBEDDED is refused only where the archive is asked for.
void verify_doclang_export_options() {
  using Mode = grparse::DoclangOptions::ImageMode;
  {
    parsev1::ConvertDocumentOptions options;
    options.add_to_formats(parsev1::OUTPUT_FORMAT_DCLX);
    options.set_image_export_mode(parsev1::IMAGE_REF_MODE_EMBEDDED);
    require_invalid(grparse::validate_options(options, kSurface), "OUTPUT_FORMAT_DCLX");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.add_to_formats(parsev1::OUTPUT_FORMAT_DOCLANG);
    options.add_to_formats(parsev1::OUTPUT_FORMAT_MARKDOWN);
    options.set_image_export_mode(parsev1::IMAGE_REF_MODE_EMBEDDED);
    options.set_doclang_include_namespace(false);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), "EMBEDDED without the archive and the namespace switch are accepted: " +
                             status.error_message());
    const grparse::DoclangOptions doclang = grparse::doclang_options(options);
    require(doclang.image_mode == Mode::kEmbedded, "EMBEDDED maps to kEmbedded");
    require(!doclang.include_namespace, "doclang_include_namespace=false reaches the renderer");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.add_to_formats(parsev1::OUTPUT_FORMAT_DCLX);
    options.set_image_export_mode(parsev1::IMAGE_REF_MODE_PLACEHOLDER);
    require(grparse::validate_options(options, kSurface).ok(), "PLACEHOLDER suits the archive");
    require(grparse::doclang_options(options).image_mode == Mode::kPlaceholder,
            "PLACEHOLDER maps to kPlaceholder");
    options.set_image_export_mode(parsev1::IMAGE_REF_MODE_REFERENCED);
    require(grparse::doclang_options(options).image_mode == Mode::kReferenced,
            "REFERENCED maps to kReferenced");
  }
  {
    parsev1::ConvertDocumentOptions unset;
    const grparse::DoclangOptions doclang = grparse::doclang_options(unset);
    require(!doclang.image_mode.has_value() && doclang.include_namespace,
            "unset options leave each format's default and the namespace on");
    unset.set_image_export_mode(parsev1::IMAGE_REF_MODE_UNSPECIFIED);
    require(!grparse::doclang_options(unset).image_mode.has_value(),
            "UNSPECIFIED is the same as unset");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("convert-options-test", "ok", {
      verify_chart_and_caption_options,
      verify_doclang_export_options,
  });
}
