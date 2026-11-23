// Pins EngineModelConfig against docling-serve 16713d5.
//
// Field 3 stays a string on both contracts (serve names it torch_dtype_raw;
// gRParse keeps the published name torch_dtype, because buf breaking rejects
// a rename). Field 5 is the TorchDtype enum on both (serve names it
// torch_dtype; gRParse names it torch_dtype_enum). The test compiles serve's
// messages from tests/fixtures/docling_serve_engine_model.proto and compares
// tag to type, then round-trips the bytes.

#include <string>

#include <google/protobuf/descriptor.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "docling_serve_engine_model.pb.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace servev1 = ai::docling::serve::v1;

namespace {

using grparse_test::require;

const google::protobuf::FieldDescriptor* field_by_number(const google::protobuf::Descriptor* desc,
                                                         int number) {
  for (int i = 0; i < desc->field_count(); ++i) {
    if (desc->field(i)->number() == number) return desc->field(i);
  }
  return nullptr;
}

void require_same_type(const google::protobuf::FieldDescriptor* serve,
                       const google::protobuf::FieldDescriptor* ours) {
  require(serve != nullptr && ours != nullptr, "shared EngineModelConfig field is present");
  require(serve->type() == ours->type(),
          "EngineModelConfig field " + std::to_string(serve->number()) +
              " type matches serve");
  require(serve->has_presence() == ours->has_presence() && serve->is_map() == ours->is_map() &&
              serve->is_repeated() == ours->is_repeated(),
          "EngineModelConfig field " + std::to_string(serve->number()) +
              " cardinality matches serve");
  if (serve->is_map()) {
    const auto* serve_key = serve->message_type()->map_key();
    const auto* ours_key = ours->message_type()->map_key();
    const auto* serve_value = serve->message_type()->map_value();
    const auto* ours_value = ours->message_type()->map_value();
    require(serve_key->type() == ours_key->type() && serve_value->type() == ours_value->type(),
            "extra_config map key and value types match serve");
  }
}

void verify_engine_model_config_matches_serve() {
  const auto* serve = servev1::EngineModelConfig::descriptor();
  const auto* ours = parsev1::EngineModelConfig::descriptor();
  require(serve->field_count() == 5 && ours->field_count() == 5,
          "EngineModelConfig has the five serve fields");
  for (int i = 0; i < serve->field_count(); ++i) {
    const auto* serve_field = serve->field(i);
    require_same_type(serve_field, field_by_number(ours, serve_field->number()));
  }

  const auto* serve_raw = serve->FindFieldByName("torch_dtype_raw");
  const auto* ours_raw = ours->FindFieldByName("torch_dtype");
  require(serve_raw->number() == 3 && ours_raw->number() == 3 &&
              serve_raw->type() == google::protobuf::FieldDescriptor::TYPE_STRING &&
              ours_raw->type() == google::protobuf::FieldDescriptor::TYPE_STRING,
          "field 3 is the raw dtype string on both contracts");

  const auto* serve_enum = serve->FindFieldByName("torch_dtype");
  const auto* ours_enum = ours->FindFieldByName("torch_dtype_enum");
  require(serve_enum->number() == 5 && ours_enum->number() == 5 &&
              serve_enum->type() == google::protobuf::FieldDescriptor::TYPE_ENUM &&
              ours_enum->type() == google::protobuf::FieldDescriptor::TYPE_ENUM,
          "field 5 is the TorchDtype enum on both contracts");

  const auto* serve_values = servev1::TorchDtype_descriptor();
  const auto* ours_values = parsev1::TorchDtype_descriptor();
  require(serve_values->value_count() == ours_values->value_count(),
          "TorchDtype has the same number of values as serve");
  for (int i = 0; i < serve_values->value_count(); ++i) {
    const auto* serve_value = serve_values->value(i);
    const auto* ours_value = ours_values->FindValueByName(serve_value->name());
    require(ours_value != nullptr && ours_value->number() == serve_value->number(),
            "TorchDtype value " + std::string(serve_value->name()) + " keeps serve's number");
  }
  require(parsev1::TORCH_DTYPE_FLOAT32 == 1 && parsev1::TORCH_DTYPE_FLOAT16 == 2 &&
              parsev1::TORCH_DTYPE_BFLOAT16 == 3,
          "float32, float16 and bfloat16 are serve numbers 1, 2 and 3");

  const auto* serve_scalar = servev1::ScalarValue::descriptor();
  const auto* ours_scalar = parsev1::ScalarValue::descriptor();
  require(serve_scalar->field_count() == ours_scalar->field_count(),
          "ScalarValue, the extra_config value, has serve's fields");
  for (int i = 0; i < serve_scalar->field_count(); ++i) {
    const auto* serve_field = serve_scalar->field(i);
    const auto* ours_field = field_by_number(ours_scalar, serve_field->number());
    require(ours_field != nullptr && ours_field->type() == serve_field->type() &&
                ours_field->name() == serve_field->name(),
            "ScalarValue field " + std::to_string(serve_field->number()) + " matches serve");
  }
}

void verify_engine_model_config_wire_round_trip() {
  parsev1::EngineModelConfig ours;
  ours.set_repo_id("org/model");
  ours.set_revision("main");
  ours.set_torch_dtype("float8");
  ours.set_torch_dtype_enum(parsev1::TORCH_DTYPE_BFLOAT16);
  (*ours.mutable_extra_config())["max"].set_int_value(4);

  servev1::EngineModelConfig serve;
  require(serve.ParseFromString(ours.SerializeAsString()),
          "serve parses gRParse EngineModelConfig bytes");
  require(serve.repo_id() == "org/model" && serve.revision() == "main",
          "repo_id and revision stay on fields 1 and 2");
  require(serve.torch_dtype_raw() == "float8",
          "field 3 arrives as serve's torch_dtype_raw string");
  require(serve.torch_dtype() == servev1::TORCH_DTYPE_BFLOAT16,
          "field 5 arrives as serve's torch_dtype enum");
  require(serve.extra_config().at("max").int_value() == 4,
          "field 4 map value keeps its scalar");

  servev1::EngineModelConfig back;
  back.set_torch_dtype_raw("float16");
  back.set_torch_dtype(servev1::TORCH_DTYPE_FLOAT16);
  parsev1::EngineModelConfig decoded;
  require(decoded.ParseFromString(back.SerializeAsString()),
          "gRParse parses serve EngineModelConfig bytes");
  require(decoded.torch_dtype() == "float16" &&
              decoded.torch_dtype_enum() == parsev1::TORCH_DTYPE_FLOAT16,
          "serve field 3 is the raw string and field 5 is the enum");
}

}  // namespace

int main() {
  return grparse_test::run_test_main(
      {.on_failure = "FAILED", .on_success = "engine model config wire test passed"},
      {verify_engine_model_config_matches_serve, verify_engine_model_config_wire_round_trip});
}
