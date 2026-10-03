#include <new>
#include <string>
#include <system_error>

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

// ConvertSource's hybrid chunking validates its budget and tokenizer up
// front, exactly like ChunkHybridSource, instead of chunking with a zero
// budget or silently falling back to wordish/1.
void verify_hybrid_chunking_is_validated() {
  const auto hybrid = [] {
    parsev1::ConvertDocumentOptions options;
    options.add_to_formats(parsev1::OUTPUT_FORMAT_CHUNKS);
    options.mutable_hybrid_chunking();
    return options;
  };
  {
    const auto options = hybrid();
    require_invalid(grparse::validate_options(options, kSurface), "max_tokens");
  }
  {
    auto options = hybrid();
    options.mutable_hybrid_chunking()->set_max_tokens(0);
    require_invalid(grparse::validate_options(options, kSurface), "max_tokens");
  }
  {
    auto options = hybrid();
    options.mutable_hybrid_chunking()->set_max_tokens(64);
    options.mutable_hybrid_chunking()->set_tokenizer("bogus");
    require_invalid(grparse::validate_options(options, kSurface), "tokenizer");
  }
  {
    auto options = hybrid();
    options.mutable_hybrid_chunking()->set_max_tokens(64);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), status.error_message());
  }
}

// Options no leg reads pass at their Docling defaults and are turned down by
// name at any other value.
void verify_unread_options_are_rejected_unless_default() {
  {
    parsev1::ConvertDocumentOptions options;
    options.add_ocr_lang("english");
    options.add_ocr_lang("chinese");
    options.set_table_cell_matching(true);
    options.set_abort_on_error(false);
    options.set_ocr_preset("default");
    options.set_table_structure_preset("default");
    options.set_layout_preset("");
    options.set_picture_classification_preset("default");
    options.set_table_mode(parsev1::TABLE_FORMER_MODE_FAST);
    options.set_pdf_backend(parsev1::PDF_BACKEND_DLPARSE_V4);
    options.mutable_picture_description_api()->set_url("http://vlm.test");
    options.mutable_picture_description_api()->set_prompt(
        "Describe this image in a few sentences.");
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), "Docling defaults must pass: " + status.error_message());
  }
  const auto rejects = [](const std::string& name, auto mutate) {
    parsev1::ConvertDocumentOptions options;
    mutate(&options);
    require_invalid(grparse::validate_options(options, kSurface), "'" + name);
  };
  rejects("ocr_lang", [](parsev1::ConvertDocumentOptions* o) { o->add_ocr_lang("fr"); });
  rejects("table_cell_matching",
          [](parsev1::ConvertDocumentOptions* o) { o->set_table_cell_matching(false); });
  rejects("abort_on_error",
          [](parsev1::ConvertDocumentOptions* o) { o->set_abort_on_error(true); });
  rejects("ocr_preset", [](parsev1::ConvertDocumentOptions* o) { o->set_ocr_preset("easyocr"); });
  rejects("table_structure_preset",
          [](parsev1::ConvertDocumentOptions* o) { o->set_table_structure_preset("v2"); });
  rejects("layout_preset", [](parsev1::ConvertDocumentOptions* o) { o->set_layout_preset("egret"); });
  rejects("picture_classification_preset", [](parsev1::ConvertDocumentOptions* o) {
    o->set_picture_classification_preset("v2");
  });
  rejects("chunking_preset", [](parsev1::ConvertDocumentOptions* o) {
    o->add_to_formats(parsev1::OUTPUT_FORMAT_CHUNKS);
    o->set_chunking_preset("hybrid");
  });
  rejects("picture_description_api.params", [](parsev1::ConvertDocumentOptions* o) {
    o->mutable_picture_description_api()->set_url("http://vlm.test");
    (*o->mutable_picture_description_api()->mutable_params())["model"].set_string_value("x");
  });
  {
    // The VLM pipeline does stop at the first failed page.
    parsev1::ConvertDocumentOptions options;
    options.set_pipeline(parsev1::PROCESSING_PIPELINE_VLM);
    options.set_abort_on_error(true);
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), status.error_message());
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.add_to_formats(parsev1::OUTPUT_FORMAT_CHUNKS);
    options.set_chunking_preset("hierarchical");
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

// A leg that throws reports an ordinary failed outcome carrying the mapped
// status, never success.
void verify_outcome_from_exception_is_a_failure() {
  const grparse::CollectorOutcome outcome = grparse::outcome_from_exception(
      std::make_exception_ptr(std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again), "thread")));
  require(!outcome.success, "a thrown leg is not a success");
  require_equal(static_cast<int>(outcome.code), static_cast<int>(grpc::StatusCode::INTERNAL),
                "a thread that could not start");
  require(outcome.error.find("thread") != std::string::npos, "the error keeps the reason");
  require_equal(static_cast<int>(grparse::outcome_from_exception(
                                     std::make_exception_ptr(std::bad_alloc()))
                                     .code),
                static_cast<int>(grpc::StatusCode::RESOURCE_EXHAUSTED), "an allocation failure");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("convert-options-test", "ok", {
      verify_chart_and_caption_options,
      verify_hybrid_chunking_is_validated,
      verify_unread_options_are_rejected_unless_default,
      verify_is_pdf_prefers_the_signature,
      verify_status_from_exception_covers_every_throw,
      verify_outcome_from_exception_is_a_failure,
  });
}
