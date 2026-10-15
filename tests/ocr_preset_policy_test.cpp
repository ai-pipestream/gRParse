// The OCR preset policy with docling-serve's semantics: built-in kinds,
// "default", GRPARSE_ALLOWED_OCR_PRESETS narrowing the built-ins only, typed
// custom presets that stay selectable whatever that list says
// (docling-serve #723), GRPARSE_ALLOWED_OCR_KINDS, and the startup errors a
// bad GRPARSE_CUSTOM_OCR_PRESETS raises.

#include <map>
#include <string>

#include "google/rpc/status.pb.h"
#include "grparse/ocr_preset_policy.h"
#include "support/check.h"

namespace {

namespace parsev1 = ai::pipestream::parse::v1;
using grparse::OcrPresetPolicy;
using grparse::ocr_preset_policy_from_env;
using grparse_test::require;
using grparse_test::require_equal;

OcrPresetPolicy from(const std::map<std::string, std::string>& values) {
  return ocr_preset_policy_from_env([&values](const char* name) -> const char* {
    const auto found = values.find(name);
    return found == values.end() ? nullptr : found->second.c_str();
  });
}

parsev1::ConvertDocumentOptions preset(const std::string& name) {
  parsev1::ConvertDocumentOptions options;
  options.set_ocr_preset(name);
  return options;
}

parsev1::OptionRejection rejection_of(const grpc::Status& status) {
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "INVALID_ARGUMENT, got: " + status.error_message());
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()) && rich.details_size() == 1,
          "one typed detail");
  parsev1::OptionRejection rejection;
  require(rich.details(0).UnpackTo(&rejection), "an OptionRejection");
  return rejection;
}

void expect_refused(const OcrPresetPolicy& policy, const std::string& name,
                    parsev1::OptionRejectionReason reason, const std::string& message_part) {
  const auto resolved = policy.resolve(preset(name), "ConvertSource");
  require(!resolved.has_value(), "'" + name + "' must be refused");
  const auto rejection = rejection_of(resolved.error());
  require_equal(rejection.option_path(), std::string("ocr_preset"), "option path");
  require_equal(parsev1::OptionRejectionReason_Name(rejection.reason()),
                parsev1::OptionRejectionReason_Name(reason), "reason for " + name);
  require_equal(rejection.rejected_value_raw(), name, "rejected value");
  require(resolved.error().error_message().find(message_part) != std::string::npos,
          "message '" + resolved.error().error_message() + "' carries '" + message_part + "'");
}

constexpr const char* kKorean =
    R"pb(presets { key: "chinese_doc" value { kind: OCR_ENGINE_KIND_RAPIDOCR lang: "ch" } })pb";

void defaults() {
  const auto& policy = grparse::default_ocr_preset_policy();
  const auto ids = policy.preset_ids();
  require(ids == std::vector<std::string>{"auto", "default", "rapidocr"}, "built-in registry");
  for (const std::string name : {"", "default", "auto", "rapidocr"}) {
    const auto resolved = policy.resolve(preset(name), "ConvertSource");
    require(resolved.has_value(), "'" + name + "' resolves");
  }
  require(policy.resolve(parsev1::ConvertDocumentOptions(), "s").value().id == "default",
          "an unset ocr_preset is default");
  expect_refused(policy, "easyocr", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED,
                 "ocr_preset 'easyocr' is not allowed. Allowed values: ['auto', 'default', "
                 "'rapidocr'].");
}

void custom_presets_stay_selectable() {
  // docling-serve test_build_service_policy_allows_custom_ocr_presets.
  const auto open = from({{"GRPARSE_CUSTOM_OCR_PRESETS", kKorean}});
  const auto resolved = open.resolve(preset("chinese_doc"), "s");
  require(resolved.has_value(), "a custom preset is allowed by default");
  require_equal(parsev1::OcrEngineKind_Name(resolved->kind), std::string("OCR_ENGINE_KIND_RAPIDOCR"), "custom kind");
  require(resolved->lang == std::vector<std::string>{"ch"}, "custom lang");
  // docling-serve test_allowed_ocr_presets_keeps_custom_ocr_presets_selectable.
  const auto narrowed = from({{"GRPARSE_ALLOWED_OCR_PRESETS", "rapidocr"},
                              {"GRPARSE_CUSTOM_OCR_PRESETS", kKorean}});
  require(narrowed.preset_ids() == std::vector<std::string>{"chinese_doc", "default", "rapidocr"},
          "the allow list narrows built-ins and keeps custom presets and default");
  require(narrowed.resolve(preset("chinese_doc"), "s").has_value(), "custom stays selectable");
  expect_refused(narrowed, "easyocr", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED,
                 "ocr_preset 'easyocr' is not allowed");
  expect_refused(narrowed, "auto", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED,
                 "ocr_preset 'auto' is not allowed");
  // A JSON array is accepted too, and unknown names drop out as upstream.
  const auto json = from({{"GRPARSE_ALLOWED_OCR_PRESETS", R"(["auto", "easyocr"])"}});
  require(json.preset_ids() == std::vector<std::string>{"auto", "default"}, "JSON list");
}

void request_lang_overrides() {
  const auto policy = from({{"GRPARSE_CUSTOM_OCR_PRESETS", kKorean}});
  auto options = preset("chinese_doc");
  options.add_ocr_lang("en");
  const auto resolved = policy.resolve(options, "s");
  require(resolved.has_value() && resolved->lang == std::vector<std::string>{"en"},
          "ocr_lang overrides the preset's lang");
}

void kinds() {
  const auto easy = from({{"GRPARSE_CUSTOM_OCR_PRESETS",
                           R"pb(presets { key: "easy" value { kind: OCR_ENGINE_KIND_EASYOCR } })pb"}});
  expect_refused(easy, "easy", parsev1::OPTION_REJECTION_REASON_NOT_IMPLEMENTED,
                 "OCR kind 'easyocr' (preset easy) is not available");
  const auto only_auto = from({{"GRPARSE_ALLOWED_OCR_KINDS", "auto"},
                               {"GRPARSE_CUSTOM_OCR_PRESETS", kKorean}});
  expect_refused(only_auto, "chinese_doc", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED,
                 "OCR kind 'rapidocr' (preset chinese_doc) is not allowed by administrator. "
                 "Allowed kinds: ['auto', 'auto'].");
  require(only_auto.resolve(preset("auto"), "s").has_value(), "the default kind is always allowed");
  const auto rapid_default = from({{"GRPARSE_ALLOWED_OCR_KINDS", "easyocr"},
                                   {"GRPARSE_DEFAULT_OCR_KIND", "rapidocr"}});
  require(rapid_default.resolve(preset("rapidocr"), "s").has_value(),
          "GRPARSE_DEFAULT_OCR_KIND is allowed whatever the list says");
}

void startup_errors() {
  const auto throws = [](const std::map<std::string, std::string>& values, const std::string& part) {
    try {
      from(values);
    } catch (const std::invalid_argument& error) {
      require(std::string(error.what()).find(part) != std::string::npos,
              std::string("error '") + error.what() + "' carries '" + part + "'");
      return;
    }
    throw std::runtime_error("expected a startup error carrying '" + part + "'");
  };
  throws({{"GRPARSE_CUSTOM_OCR_PRESETS", R"pb(presets { key: "x" value { lang: "en" } })pb"}},
         "preset 'x' must name a kind");
  throws({{"GRPARSE_CUSTOM_OCR_PRESETS",
           R"pb(presets { key: "k" value { kind: OCR_ENGINE_KIND_RAPIDOCR lang: "korean" } })pb"}},
         "lang 'korean' is not a language the installed OCR models read");
  throws({{"GRPARSE_CUSTOM_OCR_PRESETS",
           R"pb(presets { key: "k" value { kind: OCR_ENGINE_KIND_RAPIDOCR text_score: 0.5 } })pb"}},
         "is not an OcrPresetRegistry in protobuf text format");
  throws({{"GRPARSE_CUSTOM_OCR_PRESETS", R"pb(presets { key: "k" value { kind: "rapidocr" } })pb"}},
         "is not an OcrPresetRegistry in protobuf text format");
  throws({{"GRPARSE_CUSTOM_OCR_PRESETS",
           R"pb(presets { key: " k" value { kind: OCR_ENGINE_KIND_AUTO } })pb"}},
         "empty or padded preset id");
  throws({{"GRPARSE_DEFAULT_OCR_PRESET", "easyocr"}}, "is not an OCR kind this server runs");
  throws({{"GRPARSE_DEFAULT_OCR_PRESET", "korean"}}, "which is not an OCR kind");
  throws({{"GRPARSE_ALLOWED_OCR_KINDS", "auto,,rapidocr"}}, "has an empty entry");
  throws({{"GRPARSE_ALLOWED_OCR_KINDS", "paddle"}}, "names 'paddle', which is not an OCR kind");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-preset-policy-test", "ok",
                                     {defaults, custom_presets_stay_selectable,
                                      request_lang_overrides, kinds, startup_errors});
}
