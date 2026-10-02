#include <string>

#include "../src/parse_support.h"
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

// The PDF signature outranks the name: a header in the first kilobyte is a
// PDF whatever it is called, bytes that sniff as anything else are not one,
// and only bytes that say nothing fall back to the extension.
void verify_is_pdf_prefers_the_signature() {
  require(grparse::is_pdf("%PDF-1.7\n", "upload"), "a signature needs no name");
  require(grparse::is_pdf(std::string(512, ' ') + "%PDF-1.4\n", "x.bin"),
          "a signature within the first kilobyte counts");
  require(!grparse::is_pdf(std::string(2048, ' ') + "%PDF-1.4\n", "x.bin"),
          "a signature past the first kilobyte does not");
  const std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
  require(!grparse::is_pdf(png, "scan.pdf"), "a PNG named .pdf is a PNG");
  require(grparse::is_pdf(std::string("\0\x01\x02\x03", 4), "REPORT.PDF"),
          "bytes that say nothing fall back to a case-insensitive extension");
}

// Every thrown failure maps to a status, including one that is not a
// std::exception, and a scheduler shutdown is UNAVAILABLE, not a full queue.
void verify_status_from_exception_covers_every_throw() {
  require_equal(static_cast<int>(grparse::status_from_exception(std::make_exception_ptr(42))
                                     .error_code()),
                static_cast<int>(grpc::StatusCode::UNKNOWN), "a non-standard throw");
  require_equal(
      static_cast<int>(grparse::status_from_exception(
                           std::make_exception_ptr(grparse::SchedulerShuttingDown("stopping")))
                           .error_code()),
      static_cast<int>(grpc::StatusCode::UNAVAILABLE), "a scheduler shutdown");
  require_equal(
      static_cast<int>(grparse::status_from_exception(
                           std::make_exception_ptr(grparse::SchedulerSaturated("full")))
                           .error_code()),
      static_cast<int>(grpc::StatusCode::RESOURCE_EXHAUSTED), "a saturated scheduler");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("convert-options-test", "ok", {
      verify_chart_and_caption_options,
      verify_is_pdf_prefers_the_signature,
      verify_status_from_exception_covers_every_throw,
  });
}
