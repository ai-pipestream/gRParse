#pragma once

// A server-side policy over the presets of one model-backed stage, in the
// shape docling-serve gives each such stage (settings.py and the jobkit
// converter manager): a default preset, an optional allow list of built-in
// presets, admin-defined custom presets, an optional allow list of engines,
// and a switch for request-supplied custom configurations.
//
// The type is stage-agnostic on purpose. Chart extraction is the first user
// (chart_extraction_policy.h); picture description, code/formula and the VLM
// pipeline carry the same five settings upstream and can take this type with
// their own Preset struct when they get a server-side policy.
//
// Everything is decided at startup: the registry is built once from typed
// settings, a default that resolves to nothing or to a forbidden engine
// stops the process, and a request only looks names up. Nothing is read
// from disk per request.

#include <algorithm>
#include <expected>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace grparse {

// The five docling-serve settings of one stage, already parsed. `Preset` is
// the stage's typed preset and must expose `std::string engine` (the
// docling engine name, for example "api_openai").
template <typename Preset>
struct PresetPolicySettings {
  // default_<stage>_preset: what "default" (and an unnamed preset) means. A
  // custom preset of that id wins over a built-in one.
  std::string default_preset;
  // allowed_<stage>_presets: the built-in presets a request may name.
  // nullopt allows every built-in preset. Custom presets are always allowed.
  std::optional<std::vector<std::string>> allowed_presets;
  // custom_<stage>_presets: admin-defined presets by id.
  std::map<std::string, Preset> custom_presets;
  // allowed_<stage>_engines: engines a resolved preset or a custom
  // configuration may use. nullopt allows every engine.
  std::optional<std::vector<std::string>> allowed_engines;
  // allow_custom_<stage>_config: whether a request may carry its own
  // configuration instead of naming a preset. Off by default, as upstream.
  bool allow_custom_config = false;
};

template <typename Preset>
class PresetPolicy {
 public:
  // `stage` is the capitalised stage name the rejections start with
  // ("Chart extraction"); `builtin` holds the presets this build knows.
  // Throws std::invalid_argument naming the setting when the settings do
  // not describe a usable registry.
  PresetPolicy(std::string stage, std::map<std::string, Preset> builtin,
               PresetPolicySettings<Preset> settings)
      : stage_(std::move(stage)), settings_(std::move(settings)) {
    if (settings_.default_preset.empty()) {
      throw std::invalid_argument(stage_ + " default preset must not be empty");
    }
    if (const auto custom = settings_.custom_presets.find(settings_.default_preset);
        custom != settings_.custom_presets.end()) {
      add("default", custom->second);
    } else if (const auto known = builtin.find(settings_.default_preset);
               known != builtin.end()) {
      add("default", known->second);
    } else {
      throw std::invalid_argument(stage_ + " default preset '" + settings_.default_preset +
                                  "' is neither a built-in nor a custom preset");
    }
    if (settings_.allowed_presets.has_value()) {
      for (const std::string& id : *settings_.allowed_presets) {
        if (id == "default") continue;
        if (const auto known = builtin.find(id); known != builtin.end()) {
          add(id, known->second);
        } else if (!settings_.custom_presets.contains(id)) {
          throw std::invalid_argument(stage_ + " allowed preset '" + id +
                                      "' is neither a built-in nor a custom preset");
        }
      }
    } else {
      for (const auto& [id, preset] : builtin) add(id, preset);
    }
    for (const auto& [id, preset] : settings_.custom_presets) {
      if (id == "default") {
        throw std::invalid_argument(stage_ + " custom preset id 'default' is reserved");
      }
      add(id, preset);
    }
    if (!engine_allowed(registry_.at("default").engine)) {
      throw std::invalid_argument(stage_ + " default preset '" + settings_.default_preset +
                                  "' uses engine '" + registry_.at("default").engine +
                                  "', which the allowed engines exclude");
    }
  }

  // The preset a request names; empty means "default". INVALID_ARGUMENT for
  // a name outside the registry, PERMISSION_DENIED for a preset whose
  // engine the policy excludes. The wording is docling-jobkit's.
  std::expected<Preset, grpc::Status> resolve(std::string_view requested) const {
    const std::string id = requested.empty() ? std::string("default") : std::string(requested);
    const auto found = registry_.find(id);
    if (found == registry_.end()) {
      return std::unexpected(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          stage_ + " preset '" + id + "' is not allowed. Allowed presets: " + joined(order_)));
    }
    if (grpc::Status engine = check_engine(found->second.engine); !engine.ok()) {
      return std::unexpected(engine);
    }
    return found->second;
  }

  // A request-supplied configuration using `engine`: PERMISSION_DENIED when
  // the switch is off (docling-serve policy.py wording) or the engine is
  // excluded.
  grpc::Status check_custom_config(std::string_view engine) const {
    if (!settings_.allow_custom_config) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                          "Custom " + lowercase_stage() +
                              " configuration is disabled by server policy.");
    }
    return check_engine(engine);
  }

  // Preset ids a request may name, "default" first, then registry order.
  const std::vector<std::string>& preset_ids() const { return order_; }
  const PresetPolicySettings<Preset>& settings() const { return settings_; }
  const std::string& stage() const { return stage_; }

 private:
  void add(const std::string& id, const Preset& preset) {
    if (registry_.insert_or_assign(id, preset).second) order_.push_back(id);
  }

  bool engine_allowed(std::string_view engine) const {
    return !settings_.allowed_engines.has_value() ||
           std::ranges::find(*settings_.allowed_engines, engine) !=
               settings_.allowed_engines->end();
  }

  grpc::Status check_engine(std::string_view engine) const {
    if (engine_allowed(engine)) return grpc::Status::OK;
    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                        "Engine '" + std::string(engine) +
                            "' is not allowed. Allowed engines: " +
                            joined(*settings_.allowed_engines));
  }

  std::string lowercase_stage() const {
    std::string lower = stage_;
    if (!lower.empty() && lower[0] >= 'A' && lower[0] <= 'Z') lower[0] = lower[0] - 'A' + 'a';
    return lower;
  }

  static std::string joined(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
      if (!out.empty()) out += ", ";
      out += value;
    }
    return out;
  }

  std::string stage_;
  PresetPolicySettings<Preset> settings_;
  std::map<std::string, Preset> registry_;
  std::vector<std::string> order_;
};

}  // namespace grparse
