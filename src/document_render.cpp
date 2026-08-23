// The structural exports live one renderer per translation unit under
// src/render/ (markdown_renderer.cpp, html_renderer.cpp, doctags_renderer.cpp,
// doclang_renderer.cpp, vtt_renderer.cpp over the shared renderer_base). This
// unit keeps the two exports that need no tree walk: canonical JSON and its
// YAML re-emission.
#include "grparse/document_render.h"

#include <google/protobuf/util/json_util.h>
#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {

std::string render_json(const docv1::Document& document) {
  std::string out;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  const auto status = google::protobuf::util::MessageToJsonString(document, &out, options);
  if (!status.ok()) {
    throw std::runtime_error("document JSON export failed: " +
                             std::string(status.message()));
  }
  return out;
}

namespace {

// yaml-cpp keeps the flow style it parsed from JSON input; the export
// promises block style, so every container is restyled before emitting.
void set_block_style(YAML::Node node) {  // NOLINT(performance-unnecessary-value-param): YAML::Node is a shared handle
  if (node.IsMap()) {
    node.SetStyle(YAML::EmitterStyle::Block);
    for (auto entry : node) set_block_style(entry.second);
  } else if (node.IsSequence()) {
    node.SetStyle(YAML::EmitterStyle::Block);
    for (auto entry : node) set_block_style(entry);
  }
}

}  // namespace

std::string render_yaml(const docv1::Document& document) {
  // The canonical JSON is already the exact structure this export promises;
  // YAML is a superset of JSON, so the parsed tree re-emits as the same
  // document in block-style YAML form.
  try {
    YAML::Node tree = YAML::Load(render_json(document));
    set_block_style(tree);
    YAML::Emitter emitter;
    emitter << tree;
    if (!emitter.good()) {
      throw std::runtime_error("document YAML export failed: " + emitter.GetLastError());
    }
    return std::string(emitter.c_str());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("document YAML export failed: " + std::string(error.what()));
  }
}

}  // namespace grparse
