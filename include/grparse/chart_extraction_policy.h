#pragma once

// The chart-extraction stage's typed preset and its server-side policy
// (docling-serve 1.35: default_chart_extraction_preset,
// allowed_chart_extraction_presets, custom_chart_extraction_presets,
// allowed_chart_extraction_engines, allow_custom_chart_extraction_config).
//
// A preset is what one chart extraction asks grpc-enrich for: which of the
// three Docling outputs (chart2csv, chart2summary, chart2code), which prompt
// dialect, and which model and endpoint answer. Upstream a custom preset is a
// free-form dict validated per request; here it is a struct parsed once at
// startup from GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS, so a typo stops the
// process instead of failing a conversion.

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/preset_policy.h"

namespace grparse {

struct ChartExtractionPreset {
  // The preset id this came from ("custom" for a request's own config).
  std::string id;
  // Model name sent to the VLM endpoint; empty leaves the endpoint on its
  // default model.
  std::string model;
  // VLM endpoint for the chart calls only; empty falls back to the
  // request's or the server's enrich endpoint.
  std::string vlm_endpoint;
  // Docling engine name ("api_openai", "transformers", ...), checked
  // against allowed_chart_extraction_engines. grpc-enrich always reaches the
  // model over the OpenAI-compatible HTTP API; the name is policy input.
  std::string engine = "api_openai";
  bool chart2csv = true;
  bool chart2summary = false;
  bool chart2code = false;
  bool use_natural_language_prompts = false;
};

using ChartExtractionPolicy = PresetPolicy<ChartExtractionPreset>;

// The built-in presets: granite_vision_v4 (Docling's default), csv only,
// special-token prompts, model "granite-vision-4.1-4b" (the name Docling's
// preset sends through its OpenAI-compatible API engines).
std::map<std::string, ChartExtractionPreset> builtin_chart_extraction_presets();

// The policy docling-serve ships with: default granite_vision_v4, every
// built-in preset and engine allowed, no custom presets, custom configs off.
ChartExtractionPolicy default_chart_extraction_policy();

// Docling's engine name for a wire VlmEngineType ("transformers", "mlx",
// "vllm", "api", "api_ollama", "api_lmstudio", "api_openai",
// "auto_inline"); empty for UNSPECIFIED or an unknown value.
std::string vlm_engine_name(ai::pipestream::parse::v1::VlmEngineType engine);

// Parses GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS: a JSON object mapping
// preset id to an object with the optional keys model, url, engine_type,
// chart2csv, chart2summary, chart2code, use_natural_language_prompts.
// Throws std::invalid_argument naming the preset and key on an unknown key,
// a wrong type, an unknown engine, or a preset that enables no output.
std::map<std::string, ChartExtractionPreset> parse_chart_extraction_presets(
    std::string_view json);

// Reads the five GRPARSE_*CHART_EXTRACTION* variables through `lookup`
// (std::getenv in the server, a map in tests) and builds the policy. Lists
// accept a JSON array or a comma-separated string, as docling-serve does;
// an empty value counts as unset. Throws std::invalid_argument naming the
// variable on any unusable value.
ChartExtractionPolicy chart_extraction_policy_from_env(
    const std::function<const char*(const char*)>& lookup);

// The preset a request's custom configuration describes: outputs and prompt
// dialect as given, the model Docling's API engines would send
// (api_overrides[engine].params.model, else model_spec.default_repo_id),
// and the engine name. The endpoint stays empty (the request has no field
// for one; the server's enrich endpoint answers).
ChartExtractionPreset chart_preset_from_custom_config(
    const ai::pipestream::parse::v1::ChartExtractionVlmEngineOptions& config);

}  // namespace grparse
