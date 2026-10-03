#include <new>
#include <map>
#include <string>
#include <system_error>

#include "../src/parse_support.h"
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
    (*options.mutable_ocr_custom_config())["lang"].set_string_value("en");
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
    (*o->mutable_picture_description_api()->mutable_params())["frequency_penalty"]
        .set_double_value(0.5);
  });
  rejects("vlm_pipeline_model_api.headers", [](parsev1::ConvertDocumentOptions* o) {
    o->mutable_vlm_pipeline_model_api()->set_url("granite-docling");
    (*o->mutable_vlm_pipeline_model_api()->mutable_headers())["Authorization"] = "Bearer x";
  });
  rejects("vlm_pipeline_model_api.params", [](parsev1::ConvertDocumentOptions* o) {
    o->mutable_vlm_pipeline_model_api()->set_url("granite-docling");
    (*o->mutable_vlm_pipeline_model_api()->mutable_params())["model"].set_string_value("x");
  });
  rejects("ocr_custom_config.lang", [](parsev1::ConvertDocumentOptions* o) {
    (*o->mutable_ocr_custom_config())["lang"].set_string_value("fr");
  });
  rejects("ocr_custom_config.det_limit_side_len", [](parsev1::ConvertDocumentOptions* o) {
    (*o->mutable_ocr_custom_config())["lang"].set_string_value("en");
    (*o->mutable_ocr_custom_config())["det_limit_side_len"].set_int_value(960);
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

// The per-collector rules travel typed; the deprecated JSON forms still work
// alone, a request that sets both forms of one is refused, and lol-html JSON
// that does not parse is refused up front instead of failing the leg.
void verify_collector_rules_are_typed() {
  {
    parsev1::ConvertDocumentOptions options;
    options.mutable_ebcdic_layout()->add_records()->set_name("CUSTOMER");
    options.mutable_lol_html_options()->add_rules()->set_id("links");
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), "the typed rules pass: " + status.error_message());
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_ebcdic_layout_json(R"({"records": []})");
    options.set_lol_html_options_json(R"({"rules":[{"id":"links","selector":"a"}]})");
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), "the deprecated JSON forms still pass alone: " + status.error_message());
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.mutable_ebcdic_layout()->add_records()->set_name("CUSTOMER");
    options.set_ebcdic_layout_json(R"({"records": []})");
    require_invalid(grparse::validate_options(options, kSurface),
                    "ebcdic_layout and the deprecated ebcdic_layout_json");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.mutable_lol_html_options()->add_rules()->set_id("links");
    options.set_lol_html_options_json(R"({"rules":[]})");
    require_invalid(grparse::validate_options(options, kSurface),
                    "lol_html_options and the deprecated lol_html_options_json");
  }
  {
    parsev1::ConvertDocumentOptions options;
    options.set_lol_html_options_json("not json");
    require_invalid(grparse::validate_options(options, kSurface), "lolhtml.v1.ExtractOptions");
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

// picture_description_api's prompt, params and headers travel to enrich in
// its typed form: the typed param names pass, any other is refused by name,
// a value of the wrong kind is refused naming the param, and a header the
// HTTP client sets itself or a value holding a control character is refused
// naming the header, never echoing its value.
void verify_picture_description_api_call_is_typed() {
  const auto api_options = [](auto mutate) {
    parsev1::ConvertDocumentOptions options;
    auto* api = options.mutable_picture_description_api();
    api->set_url("http://vlm.test");
    mutate(api);
    return options;
  };
  {
    const auto options = api_options([](parsev1::PictureDescriptionApi* api) {
      api->set_prompt("Name the chart type.");
      (*api->mutable_params())["model"].set_string_value("granite-vision");
      (*api->mutable_params())["max_tokens"].set_int_value(300);
      (*api->mutable_params())["temperature"].set_int_value(0);
      (*api->mutable_params())["top_p"].set_double_value(1.0);
      (*api->mutable_params())["seed"].set_int_value(-3);
      (*api->mutable_headers())["Authorization"] = "Bearer sk-x";
    });
    const grpc::Status status = grparse::validate_options(options, kSurface);
    require(status.ok(), "the typed api fields pass: " + status.error_message());
  }
  const auto invalid = [&](const std::string& expected, auto mutate) {
    const grpc::Status status = grparse::validate_options(api_options(mutate), kSurface);
    require_invalid(status, expected);
    require(!status.error_message().contains("sk-never-echoed"),
            "a refusal never echoes a header value: " + status.error_message());
  };
  invalid("'picture_description_api.params.logit_bias'", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["logit_bias"].set_string_value("{}");
  });
  invalid("params.model", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["model"].set_int_value(3);
  });
  invalid("params.max_completion_tokens", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["max_completion_tokens"].set_int_value(0);
  });
  invalid("same generation cap", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["max_tokens"].set_int_value(10);
    (*api->mutable_params())["max_completion_tokens"].set_int_value(10);
  });
  invalid("params.temperature", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["temperature"].set_double_value(-1.0);
  });
  invalid("params.top_p", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["top_p"].set_double_value(1.5);
  });
  invalid("params.seed", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_params())["seed"].set_double_value(1.5);
  });
  invalid("headers 'Host'", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_headers())["Host"] = "sk-never-echoed";
  });
  invalid("headers 'Proxy-Authorization'", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_headers())["Proxy-Authorization"] = "sk-never-echoed";
  });
  invalid("headers name 'Bad Name'", [](parsev1::PictureDescriptionApi* api) {
    (*api->mutable_headers())["Bad Name"] = "sk-never-echoed";
  });
  invalid("headers 'Authorization' value holds a control character",
          [](parsev1::PictureDescriptionApi* api) {
            (*api->mutable_headers())["Authorization"] = "sk-never-echoed\r\nX-Injected: 1";
          });
  invalid("'Authorization' and 'authorization' name the same header",
          [](parsev1::PictureDescriptionApi* api) {
            (*api->mutable_headers())["Authorization"] = "sk-never-echoed";
            (*api->mutable_headers())["authorization"] = "sk-never-echoed";
          });
}

// The caller's headers are for its own endpoint. grpc-enrich sends them to
// every per-request endpoint of the job, so a chart preset naming another
// endpoint is refused while the chart leg is on, naming the preset and not
// its endpoint; the same endpoint, no headers, or charts off all pass.
void verify_picture_description_headers_stay_on_their_endpoint() {
  parsev1::ConvertDocumentOptions options;
  auto* api = options.mutable_picture_description_api();
  api->set_url("http://vlm.test");
  (*api->mutable_headers())["Authorization"] = "sk-never-echoed";
  grparse::ChartExtractionPreset preset;
  preset.id = "granite";
  preset.vlm_endpoint = "http://charts.internal:8000/v1";
  const auto refused = grparse::request_picture_description_call(options, preset, kSurface);
  require(!refused.has_value(), "headers beside another chart endpoint are refused");
  require_invalid(refused.error(), "chart extraction preset 'granite'");
  require(!refused.error().error_message().contains("charts.internal") &&
              !refused.error().error_message().contains("sk-never-echoed"),
          "the refusal names neither the preset's endpoint nor the header value");

  preset.vlm_endpoint = "http://vlm.test";
  const auto same = grparse::request_picture_description_call(options, preset, kSurface);
  require(same.has_value() && same->headers.size() == 1,
          "a chart preset on the request's own endpoint keeps the headers");
  preset.vlm_endpoint = "http://charts.internal:8000/v1";
  options.set_do_chart_extraction(false);
  require(grparse::request_picture_description_call(options, preset, kSurface).has_value(),
          "with the chart leg off the preset's endpoint gets no call");
  options.clear_do_chart_extraction();
  api->clear_headers();
  require(grparse::request_picture_description_call(options, preset, kSurface).has_value(),
          "without headers nothing travels to the preset's endpoint");
  require(grparse::request_picture_description_call(parsev1::ConvertDocumentOptions(), preset,
                                                    kSurface)
              .has_value(),
          "a request without picture_description_api resolves to an empty call");
}

// The parse identity's options digest: SHA-256 over the options that decide
// the Document, with the export, timing, report and transport fields and
// any unknown fields left out first.
void verify_options_digest_hashes_only_decisive_options() {
  const parsev1::ConvertDocumentOptions unset;
  require(grparse::options_digest(unset) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "no decisive option set digests the empty serialization");
  parsev1::ConvertDocumentOptions options;
  options.set_do_ocr(false);
  const std::string digest = grparse::options_digest(options);
  // SHA-256 of the bytes 0x20 0x00 (field 4, do_ocr, explicitly false).
  require(digest == "869f1dfb999a452f497a4cf7f44db2d6ee661f74a9e7e05251bc1420e50672d4",
          "the digest is fixed for a fixed option set: " + digest);

  parsev1::ConvertDocumentOptions neutral = options;
  neutral.add_to_formats(parsev1::OUTPUT_FORMAT_HTML);
  neutral.set_image_export_mode(parsev1::IMAGE_REF_MODE_EMBEDDED);
  neutral.set_md_page_break_placeholder("<!-- page -->");
  neutral.set_md_compact_tables(true);
  neutral.set_doclang_include_namespace(true);
  neutral.set_document_timeout(30.0);
  neutral.set_abort_on_error(true);
  neutral.mutable_hybrid_chunking()->set_max_tokens(64);
  neutral.set_structure_validation(parsev1::STRUCTURE_VALIDATION_REPORT);
  auto* api = neutral.mutable_vlm_pipeline_model_api();
  api->set_url("http://vlm:1");
  (*api->mutable_headers())["authorization"] = "Bearer secret";
  api->set_timeout(5.0);
  api->set_concurrency(2);
  neutral.GetReflection()->MutableUnknownFields(&neutral)->AddVarint(9999, 1);
  parsev1::ConvertDocumentOptions same_api = options;
  same_api.mutable_vlm_pipeline_model_api()->set_url("http://vlm:1");
  require(grparse::options_digest(neutral) == grparse::options_digest(same_api),
          "export, timing, report, chunking, transport and unknown fields leave it alone");

  parsev1::ConvertDocumentOptions decisive = options;
  decisive.set_do_ocr(true);
  require(grparse::options_digest(decisive) != digest, "a decisive option changes the digest");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("convert-options-test", "ok", {
      verify_chart_and_caption_options,
      verify_hybrid_chunking_is_validated,
      verify_unread_options_are_rejected_unless_default,
      verify_collector_rules_are_typed,
      verify_is_pdf_prefers_the_signature,
      verify_status_from_exception_covers_every_throw,
      verify_outcome_from_exception_is_a_failure,
      verify_chart_policy_resolution,
      verify_doclang_export_options,
      verify_picture_description_api_call_is_typed,
      verify_picture_description_headers_stay_on_their_endpoint,
      verify_options_digest_hashes_only_decisive_options,
  });
}
