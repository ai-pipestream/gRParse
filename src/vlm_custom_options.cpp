#include "vlm_custom_options.h"

#include <optional>
#include <span>
#include <string_view>

#include "option_rejection.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace vlmv1 = ai::pipestream::vlm::v1;

namespace grparse {
namespace {

// A docling string and the enum member it names.
struct NamedMember {
  std::string_view name;
  int member;
};

constexpr NamedMember kTorchDtypes[] = {
    {"float32", parsev1::TORCH_DTYPE_FLOAT32},
    {"float16", parsev1::TORCH_DTYPE_FLOAT16},
    {"bfloat16", parsev1::TORCH_DTYPE_BFLOAT16},
};

constexpr NamedMember kNemotronPasses[] = {
    {"single", parsev1::NEMOTRON_PARSE_PASSES_SINGLE},
    {"double", parsev1::NEMOTRON_PARSE_PASSES_DOUBLE},
    {"triple", parsev1::NEMOTRON_PARSE_PASSES_TRIPLE},
};

constexpr NamedMember kNemotronTripleRegions[] = {
    {"detector", parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR},
    {"union", parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION},
};

std::optional<int> member_named(std::string_view raw, std::span<const NamedMember> members) {
  for (const auto& entry : members) {
    if (entry.name == raw) return entry.member;
  }
  return std::nullopt;
}

// One field docling sends as a closed enum beside its raw string. `typed` is
// the enum's wire value when the field is present; `typed_known` says it is
// a member other than UNSPECIFIED.
struct TypedAndRaw {
  std::string path;
  std::optional<int> typed;
  bool typed_known = false;
  std::string raw_path;
  std::optional<std::string> raw;
  std::span<const NamedMember> members;
  // docling's field accepts only the members, so a raw string alone must
  // name one.
  bool closed = false;
};

grpc::Status check_typed_and_raw(const TypedAndRaw& field, const std::string& surface) {
  if (field.typed.has_value() && !field.typed_known) {
    return option_rejected(field.path, parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE,
                           std::to_string(*field.typed),
                           surface + ": " + field.path + " is not a known value");
  }
  if (!field.raw.has_value()) return grpc::Status::OK;
  const std::optional<int> named = member_named(*field.raw, field.members);
  if (!field.typed.has_value()) {
    if (named.has_value() || !field.closed) return grpc::Status::OK;
    return option_rejected(field.raw_path, parsev1::OPTION_REJECTION_REASON_UNKNOWN_VALUE,
                           *field.raw, surface + ": " + field.raw_path + " is not a known value");
  }
  if (!named.has_value()) {
    return option_rejected(field.raw_path, parsev1::OPTION_REJECTION_REASON_CONFLICT, *field.raw,
                           surface + ": " + field.raw_path +
                               " is not a known value for the set " + field.path);
  }
  if (*named != *field.typed) {
    return option_rejected(field.raw_path, parsev1::OPTION_REJECTION_REASON_CONFLICT, *field.raw,
                           surface + ": " + field.path + " and " + field.raw_path + " disagree");
  }
  return grpc::Status::OK;
}

template <typename Enum>
std::optional<int> present(bool has, Enum value) {
  return has ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
}

std::optional<std::string> present(bool has, const std::string& value) {
  return has ? std::optional<std::string>(value) : std::nullopt;
}

grpc::Status validate_torch_dtype(const parsev1::EngineModelConfig& config,
                                  const std::string& path, const std::string& surface) {
  // torch_dtype is the raw string (field 3); torch_dtype_enum is the typed value.
  const auto dtype = config.torch_dtype_enum();
  return check_typed_and_raw(
      {.path = path + ".torch_dtype_enum",
       .typed = present(config.has_torch_dtype_enum(), dtype),
       .typed_known =
           parsev1::TorchDtype_IsValid(dtype) && dtype != parsev1::TORCH_DTYPE_UNSPECIFIED,
       .raw_path = path + ".torch_dtype",
       .raw = present(config.has_torch_dtype(), config.torch_dtype()),
       .members = kTorchDtypes},
      surface);
}

}  // namespace

grpc::Status validate_engine_overrides(const parsev1::VlmModelSpec& spec, const std::string& path,
                                       const std::string& surface) {
  for (int i = 0; i < spec.engine_overrides_size(); ++i) {
    const auto status = validate_torch_dtype(
        spec.engine_overrides(i).config(),
        path + ".engine_overrides[" + std::to_string(i) + "].config", surface);
    if (!status.ok()) return status;
  }
  return grpc::Status::OK;
}

grpc::Status validate_nemotron_parse(const parsev1::NemotronParseOptions& options,
                                     const std::string& path, const std::string& surface) {
  const auto passes = options.passes();
  if (auto status = check_typed_and_raw(
          {.path = path + ".passes",
           .typed = present(options.has_passes(), passes),
           .typed_known = parsev1::NemotronParsePasses_IsValid(passes) &&
                          passes != parsev1::NEMOTRON_PARSE_PASSES_UNSPECIFIED,
           .raw_path = path + ".passes_raw",
           .raw = present(options.has_passes_raw(), options.passes_raw()),
           .members = kNemotronPasses,
           .closed = true},
          surface);
      !status.ok()) {
    return status;
  }
  const auto regions = options.triple_regions();
  if (auto status = check_typed_and_raw(
          {.path = path + ".triple_regions",
           .typed = present(options.has_triple_regions(), regions),
           .typed_known = parsev1::NemotronParseTripleRegions_IsValid(regions) &&
                          regions != parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNSPECIFIED,
           .raw_path = path + ".triple_regions_raw",
           .raw = present(options.has_triple_regions_raw(), options.triple_regions_raw()),
           .members = kNemotronTripleRegions,
           .closed = true},
          surface);
      !status.ok()) {
    return status;
  }
  if (options.has_crop_batch_size() && options.crop_batch_size() < 1) {
    return option_rejected(path + ".crop_batch_size",
                           parsev1::OPTION_REJECTION_REASON_OUT_OF_RANGE,
                           std::to_string(options.crop_batch_size()),
                           surface + ": " + path + ".crop_batch_size must be >= 1");
  }
  return grpc::Status::OK;
}

vlmv1::NemotronParseOptions collector_nemotron_parse(
    const parsev1::NemotronParseOptions& options) {
  vlmv1::NemotronParseOptions out;
  std::optional<int> passes = present(options.has_passes(), options.passes());
  if (!passes && options.has_passes_raw()) {
    passes = member_named(options.passes_raw(), kNemotronPasses);
  }
  switch (passes.value_or(parsev1::NEMOTRON_PARSE_PASSES_UNSPECIFIED)) {
    case parsev1::NEMOTRON_PARSE_PASSES_SINGLE:
      out.set_passes(vlmv1::NEMOTRON_PARSE_PASSES_SINGLE);
      break;
    case parsev1::NEMOTRON_PARSE_PASSES_DOUBLE:
      out.set_passes(vlmv1::NEMOTRON_PARSE_PASSES_DOUBLE);
      break;
    case parsev1::NEMOTRON_PARSE_PASSES_TRIPLE:
      out.set_passes(vlmv1::NEMOTRON_PARSE_PASSES_TRIPLE);
      break;
    default:
      break;
  }
  std::optional<int> regions = present(options.has_triple_regions(), options.triple_regions());
  if (!regions && options.has_triple_regions_raw()) {
    regions = member_named(options.triple_regions_raw(), kNemotronTripleRegions);
  }
  switch (regions.value_or(parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNSPECIFIED)) {
    case parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR:
      out.set_triple_regions(vlmv1::NEMOTRON_PARSE_TRIPLE_REGIONS_DETECTOR);
      break;
    case parsev1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION:
      out.set_triple_regions(vlmv1::NEMOTRON_PARSE_TRIPLE_REGIONS_UNION);
      break;
    default:
      break;
  }
  if (options.has_crop_batch_size()) {
    out.set_crop_batch_size(static_cast<uint32_t>(options.crop_batch_size()));
  }
  return out;
}

}  // namespace grparse
