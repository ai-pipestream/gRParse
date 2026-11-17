// Links the header declarations of validate_nemotron_parse and
// validate_engine_overrides. Those definitions live in source_parse.cpp
// outside its anonymous namespace; internal linkage fails this binary at
// link time, which is what cpu-image, cuda-image, and openvino-image did
// to convert-options-test.

#include <string>

#include "../src/source_parse.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "google/rpc/status.pb.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;

constexpr const char* kSurface = "ConvertSource";

using NemotronValidator = grpc::Status (*)(const parsev1::NemotronParseOptions&,
                                           const std::string&, const std::string&);
using EngineValidator = grpc::Status (*)(const parsev1::VlmModelSpec&, const std::string&,
                                         const std::string&);

const NemotronValidator kValidateNemotron = &grparse::validate_nemotron_parse;
const EngineValidator kValidateEngine = &grparse::validate_engine_overrides;

parsev1::OptionRejection rejection_of(const grpc::Status& status) {
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()), "error_details is a google.rpc.Status");
  parsev1::OptionRejection rejection;
  require(rich.details(0).UnpackTo(&rejection), "the detail is an OptionRejection");
  return rejection;
}

void verify_exported_nemotron_parse_accepts_agreeing_raw() {
  parsev1::NemotronParseOptions options;
  options.set_passes(parsev1::NEMOTRON_PARSE_PASSES_DOUBLE);
  options.set_passes_raw("double");
  options.set_triple_regions(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR);
  options.set_triple_regions_raw("detector");
  options.set_crop_batch_size(1);
  require(kValidateNemotron(options, "nemotron_parse", kSurface).ok(),
          "agreeing raw strings and enums pass the exported validator");
  require(options.passes() == parsev1::NEMOTRON_PARSE_PASSES_DOUBLE &&
              options.passes_raw() == "double" &&
              options.triple_regions() == parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR &&
              options.triple_regions_raw() == "detector" && options.crop_batch_size() == 1,
          "agreeing fields are left as set");
}

void verify_exported_engine_override_names_the_later_index() {
  parsev1::VlmModelSpec spec;
  spec.add_engine_overrides()->mutable_config()->set_torch_dtype_enum(
      parsev1::TORCH_DTYPE_FLOAT32);
  auto* later = spec.add_engine_overrides()->mutable_config();
  later->set_torch_dtype_enum(parsev1::TORCH_DTYPE_BFLOAT16);
  later->set_torch_dtype("float16");
  const auto rejection = rejection_of(kValidateEngine(spec, "model_spec", kSurface));
  require(rejection.reason() == parsev1::OPTION_REJECTION_REASON_CONFLICT &&
              rejection.option_path() ==
                  "model_spec.engine_overrides[1].config.torch_dtype" &&
              rejection.rejected_value_raw() == "float16",
          "the exported validator names the later override that disagrees");
}

}  // namespace

int main() {
  return grparse_test::run_test_main(
      {.on_failure = "FAILED", .on_success = "validator link test passed"},
      {verify_exported_nemotron_parse_accepts_agreeing_raw,
       verify_exported_engine_override_names_the_later_index});
}
