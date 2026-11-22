// The VLM custom-config fields docling-serve sends as an enum beside its raw
// string: NemotronParseOptions passes and triple_regions, and an engine
// override's torch dtype. Checked through validate_options and directly,
// along with how the Nemotron Parse options reach the convert collector.

#include <string>

#include "../src/source_parse.h"
#include "../src/vlm_custom_options.h"
#include "grparse/vlm_convert.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "google/rpc/status.pb.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;

namespace {

using grparse_test::require;

constexpr const char* kSurface = "ConvertSource";

parsev1::OptionRejection rejection_of(const grpc::Status& status) {
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()), "error_details is a google.rpc.Status");
  require(rich.details_size() == 1, "the rich status carries one detail");
  parsev1::OptionRejection rejection;
  require(rich.details(0).UnpackTo(&rejection), "the detail is an OptionRejection");
  return rejection;
}

parsev1::ConvertDocumentOptions vlm_custom_base() {
  parsev1::ConvertDocumentOptions options;
  auto* cfg = options.mutable_vlm_pipeline_custom_config();
  cfg->mutable_engine_options()->set_engine_type(parsev1::VLM_ENGINE_TYPE_TRANSFORMERS);
  cfg->mutable_engine_options()->set_trust_remote_code(false);
  cfg->mutable_model_spec()->set_name("custom");
  cfg->mutable_model_spec()->set_default_repo_id("example/model");
  cfg->mutable_model_spec()->set_trust_remote_code(false);
  return options;
}

void verify_nemotron_parse_and_torch_dtype() {
  {
    auto options = vlm_custom_base();
    auto* nemotron = options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse();
    nemotron->set_passes(parsev1::NEMOTRON_PARSE_PASSES_SINGLE);
    nemotron->set_passes_raw("single");
    nemotron->set_triple_regions(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION);
    nemotron->set_triple_regions_raw("union");
    nemotron->set_crop_batch_size(8);
    const auto status = grparse::validate_options(options, kSurface);
    require(status.ok(), "a single-pass nemotron_parse is accepted: " + status.error_message());
  }
  {
    auto options = vlm_custom_base();
    options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse();
    require(grparse::validate_options(options, kSurface).ok(),
            "an empty nemotron_parse keeps docling's single-pass default");
  }
  {
    auto options = vlm_custom_base();
    options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse()->set_passes(
        parsev1::NEMOTRON_PARSE_PASSES_TRIPLE);
    const auto status = grparse::validate_options(options, kSurface);
    require(status.ok(), "a triple pass is accepted for the collector to run: " +
                             status.error_message());
  }
  {
    auto options = vlm_custom_base();
    options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse()->set_passes_raw(
        "quadruple");
    const auto rejection = rejection_of(grparse::validate_options(options, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() ==
                    "vlm_pipeline_custom_config.nemotron_parse.passes_raw",
            "a pass name docling does not define is unknown");
  }
  require(grparse::validate_options(vlm_custom_base(), kSurface).ok(),
          "the base custom config without nemotron_parse is accepted");
  {
    auto options = vlm_custom_base();
    auto* nemotron = options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse();
    nemotron->set_passes(parsev1::NEMOTRON_PARSE_PASSES_SINGLE);
    nemotron->set_passes_raw("double");
    const auto rejection = rejection_of(grparse::validate_options(options, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() ==
                    "vlm_pipeline_custom_config.nemotron_parse.passes_raw",
            "a known passes value must agree with passes_raw");
  }
  {
    auto options = vlm_custom_base();
    options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse()->set_crop_batch_size(0);
    const auto rejection = rejection_of(grparse::validate_options(options, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_OUT_OF_RANGE &&
                rejection.option_path() ==
                    "vlm_pipeline_custom_config.nemotron_parse.crop_batch_size",
            "crop_batch_size below 1 is out of range");
  }
  {
    auto options = vlm_custom_base();
    auto* config = options.mutable_vlm_pipeline_custom_config()
                       ->mutable_model_spec()
                       ->add_engine_overrides()
                       ->mutable_config();
    config->set_torch_dtype_enum(parsev1::TORCH_DTYPE_FLOAT16);
    config->set_torch_dtype("bfloat16");
    const auto rejection = rejection_of(grparse::validate_options(options, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path().ends_with(".torch_dtype"),
            "torch_dtype_enum and torch_dtype must agree");
  }
  {
    auto options = vlm_custom_base();
    options.mutable_vlm_pipeline_custom_config()
        ->mutable_model_spec()
        ->add_engine_overrides()
        ->mutable_config()
        ->set_torch_dtype("float8");
    const auto status = grparse::validate_options(options, kSurface);
    require(status.error_code() == grpc::StatusCode::UNIMPLEMENTED,
            "an unknown torch_dtype string passes the check, then engine_overrides is "
            "refused: " + status.error_message());
  }
}

void verify_validate_nemotron_parse_directly() {
  const std::string path = "nemotron_parse";
  {
    parsev1::NemotronParseOptions options;
    options.set_passes(parsev1::NEMOTRON_PARSE_PASSES_SINGLE);
    options.set_triple_regions(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION);
    options.set_crop_batch_size(8);
    require(grparse::validate_nemotron_parse(options, path, kSurface).ok(),
            "validate_nemotron_parse accepts a known enum with no raw");
    require(options.passes() == parsev1::NEMOTRON_PARSE_PASSES_SINGLE &&
                options.triple_regions() == parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION,
            "the set enums stand when raw is absent");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_passes_raw("quadruple");
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() == "nemotron_parse.passes_raw" &&
                rejection.rejected_value_raw() == "quadruple",
            "validate_nemotron_parse rejects a passes_raw docling does not define");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_triple_regions_raw("both");
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() == "nemotron_parse.triple_regions_raw",
            "validate_nemotron_parse rejects a triple_regions_raw docling does not define");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_passes_raw("triple");
    require(grparse::validate_nemotron_parse(options, path, kSurface).ok(),
            "validate_nemotron_parse accepts a known passes_raw alone");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_passes(parsev1::NEMOTRON_PARSE_PASSES_SINGLE);
    options.set_passes_raw("double");
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() == "nemotron_parse.passes_raw" &&
                rejection.rejected_value_raw() == "double",
            "validate_nemotron_parse rejects a passes raw that disagrees with the enum");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_triple_regions(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR);
    options.set_triple_regions_raw("union");
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() == "nemotron_parse.triple_regions_raw",
            "validate_nemotron_parse rejects a triple_regions raw that disagrees");
  }
  {
    parsev1::NemotronParseOptions options;
    const auto* field = options.GetDescriptor()->FindFieldByName("passes");
    options.GetReflection()->SetEnumValue(&options, field, 9);
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() == "nemotron_parse.passes",
            "validate_nemotron_parse rejects a passes value outside the enum");
  }
  {
    parsev1::NemotronParseOptions options;
    const auto* field = options.GetDescriptor()->FindFieldByName("triple_regions");
    options.GetReflection()->SetEnumValue(&options, field, 9);
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() == "nemotron_parse.triple_regions",
            "validate_nemotron_parse rejects a triple_regions value outside the enum");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_crop_batch_size(0);
    const auto rejection = rejection_of(grparse::validate_nemotron_parse(options, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_OUT_OF_RANGE &&
                rejection.option_path() == "nemotron_parse.crop_batch_size" &&
                rejection.rejected_value_raw() == "0",
            "validate_nemotron_parse rejects crop_batch_size 0");
  }
}

void verify_the_collector_gets_the_named_passes() {
  const auto forwarded = [](const parsev1::NemotronParseOptions& nemotron) {
    auto options = vlm_custom_base();
    *options.mutable_vlm_pipeline_custom_config()->mutable_nemotron_parse() = nemotron;
    require(grparse::validate_options(options, kSurface).ok(), "the options are valid");
    grparse::VlmConvertOptions collector;
    grparse::apply_vlm_convert_options(options, &collector);
    require(collector.nemotron_parse.has_value(), "nemotron_parse reaches the collector");
    return *collector.nemotron_parse;
  };

  parsev1::NemotronParseOptions typed;
  typed.set_passes(parsev1::NEMOTRON_PARSE_PASSES_TRIPLE);
  typed.set_triple_regions(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR);
  typed.set_crop_batch_size(2);
  const auto from_typed = forwarded(typed);
  require(from_typed.passes() == vlmv1::NEMOTRON_PARSE_PASSES_TRIPLE &&
              from_typed.triple_regions() == vlmv1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR &&
              from_typed.crop_batch_size() == 2,
          "typed passes, regions and batch size are forwarded");

  parsev1::NemotronParseOptions raw;
  raw.set_passes_raw("double");
  raw.set_triple_regions_raw("union");
  const auto from_raw = forwarded(raw);
  require(from_raw.passes() == vlmv1::NEMOTRON_PARSE_PASSES_DOUBLE &&
              from_raw.triple_regions() == vlmv1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION &&
              from_raw.crop_batch_size() == 0,
          "raw names alone pick the members; an unset batch size stays unset");

  const auto empty = forwarded(parsev1::NemotronParseOptions());
  require(empty.passes() == vlmv1::NEMOTRON_PARSE_PASSES_UNSPECIFIED &&
              empty.triple_regions() == vlmv1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNSPECIFIED,
          "an empty nemotron_parse leaves the collector's single pass default");

  grparse::VlmConvertOptions without;
  grparse::apply_vlm_convert_options(vlm_custom_base(), &without);
  require(!without.nemotron_parse.has_value(), "no nemotron_parse, nothing forwarded");
}

void verify_validate_engine_overrides_directly() {
  const std::string path = "model_spec";
  parsev1::VlmModelSpec empty;
  require(grparse::validate_engine_overrides(empty, path, kSurface).ok(),
          "validate_engine_overrides accepts a spec with no overrides");
  const auto config_of = [](parsev1::VlmModelSpec* spec) {
    return spec->add_engine_overrides()->mutable_config();
  };
  {
    parsev1::VlmModelSpec spec;
    config_of(&spec)->set_torch_dtype_enum(parsev1::TORCH_DTYPE_FLOAT16);
    require(grparse::validate_engine_overrides(spec, path, kSurface).ok(),
            "validate_engine_overrides accepts a known enum with no raw string");
    require(spec.engine_overrides(0).config().torch_dtype_enum() == parsev1::TORCH_DTYPE_FLOAT16 &&
                !spec.engine_overrides(0).config().has_torch_dtype(),
            "the enum stands when the raw string is absent");
  }
  {
    parsev1::VlmModelSpec spec;
    auto* config = config_of(&spec);
    config->set_torch_dtype_enum(parsev1::TORCH_DTYPE_FLOAT16);
    config->set_torch_dtype("float16");
    require(grparse::validate_engine_overrides(spec, path, kSurface).ok(),
            "validate_engine_overrides accepts a raw string that names the set enum");
  }
  {
    parsev1::VlmModelSpec spec;
    auto* config = config_of(&spec);
    config->set_torch_dtype_enum(parsev1::TORCH_DTYPE_FLOAT16);
    config->set_torch_dtype("bfloat16");
    const auto rejection =
        rejection_of(grparse::validate_engine_overrides(spec, path, kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() ==
                    "model_spec.engine_overrides[0].config.torch_dtype" &&
                rejection.rejected_value_raw() == "bfloat16",
            "validate_engine_overrides rejects a raw string that disagrees with the enum");
  }
  {
    parsev1::VlmModelSpec spec;
    config_of(&spec)->set_torch_dtype("float8");
    require(grparse::validate_engine_overrides(spec, path, kSurface).ok(),
            "validate_engine_overrides keeps an unknown dtype string");
    require(spec.engine_overrides(0).config().torch_dtype() == "float8" &&
                !spec.engine_overrides(0).config().has_torch_dtype_enum(),
            "the unknown non-empty raw string is preserved and the enum stays unset");
  }
}

void verify_an_unknown_raw_beside_a_set_enum_conflicts() {
  {
    parsev1::NemotronParseOptions options;
    options.set_passes(parsev1::NEMOTRON_PARSE_PASSES_SINGLE);
    options.set_passes_raw("quadruple");
    const auto rejection =
        rejection_of(grparse::validate_nemotron_parse(options, "nemotron_parse", kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() == "nemotron_parse.passes_raw" &&
                rejection.rejected_value_raw() == "quadruple",
            "an unknown passes_raw is refused once passes is set");
  }
  {
    parsev1::VlmModelSpec spec;
    auto* config = spec.add_engine_overrides()->mutable_config();
    config->set_torch_dtype_enum(parsev1::TORCH_DTYPE_FLOAT16);
    config->set_torch_dtype("float8");
    const auto rejection =
        rejection_of(grparse::validate_engine_overrides(spec, "model_spec", kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
                rejection.option_path() == "model_spec.engine_overrides[0].config.torch_dtype" &&
                rejection.rejected_value_raw() == "float8",
            "an unknown torch_dtype string is refused once torch_dtype_enum is set");
  }
}

void verify_a_typed_value_outside_its_enum_is_unknown() {
  {
    parsev1::VlmModelSpec spec;
    auto* config = spec.add_engine_overrides()->mutable_config();
    config->GetReflection()->SetEnumValue(
        config, config->GetDescriptor()->FindFieldByName("torch_dtype_enum"), 9);
    const auto rejection =
        rejection_of(grparse::validate_engine_overrides(spec, "model_spec", kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() ==
                    "model_spec.engine_overrides[0].config.torch_dtype_enum" &&
                rejection.rejected_value_raw() == "9",
            "a torch_dtype_enum value outside the enum is unknown");
  }
  {
    parsev1::NemotronParseOptions options;
    options.set_passes(parsev1::NEMOTRON_PARSE_PASSES_UNSPECIFIED);
    const auto rejection =
        rejection_of(grparse::validate_nemotron_parse(options, "nemotron_parse", kSurface));
    require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE &&
                rejection.option_path() == "nemotron_parse.passes" &&
                rejection.rejected_value_raw() == "0",
            "an explicitly set UNSPECIFIED passes is not a member");
  }
}

void verify_the_disagreeing_override_is_named_by_index() {
  parsev1::VlmModelSpec spec;
  spec.add_engine_overrides()->mutable_config()->set_torch_dtype_enum(
      parsev1::TORCH_DTYPE_FLOAT32);
  auto* later = spec.add_engine_overrides()->mutable_config();
  later->set_torch_dtype_enum(parsev1::TORCH_DTYPE_BFLOAT16);
  later->set_torch_dtype("float16");
  const auto rejection =
      rejection_of(grparse::validate_engine_overrides(spec, "model_spec", kSurface));
  require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
              rejection.option_path() == "model_spec.engine_overrides[1].config.torch_dtype" &&
              rejection.rejected_value_raw() == "float16",
          "the override that disagrees is named by its index");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("vlm-custom-options-test", "ok", {
      verify_nemotron_parse_and_torch_dtype,
      verify_validate_nemotron_parse_directly,
      verify_the_collector_gets_the_named_passes,
      verify_validate_engine_overrides_directly,
      verify_an_unknown_raw_beside_a_set_enum_conflicts,
      verify_a_typed_value_outside_its_enum_is_unknown,
      verify_the_disagreeing_override_is_named_by_index,
  });
}
