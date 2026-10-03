// The structural exports live one renderer per translation unit under
// src/render/ (markdown_renderer.cpp, html_renderer.cpp, doctags_renderer.cpp,
// doclang_renderer.cpp, vtt_renderer.cpp over the shared renderer_base). This
// unit keeps the two exports that need no tree walk: canonical JSON and its
// YAML re-emission.
#include "grparse/document_render.h"

#include <algorithm>
#include <cctype>
#include <google/protobuf/util/json_util.h>
#include <set>
#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

#include "render/json_key_order.h"

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
  // The printer writes map fields in the map instance's own order; the
  // export is a function of the document's contents (render/json_key_order.h).
  static const std::set<std::string> kMapFields =
      render::map_field_names(docv1::Document::descriptor());
  return render::sort_map_objects(out, kMapFields);
}

namespace {

// A key is written plain only when it is an identifier no YAML 1.1 or 1.2
// resolver reads as a bool or null; every other key is double-quoted.
bool plain_key(const std::string& key) {
  static const std::set<std::string> kResolved = {
      "y", "Y", "yes", "Yes", "YES", "n", "N", "no", "No", "NO",
      "true", "True", "TRUE", "false", "False", "FALSE",
      "on", "On", "ON", "off", "Off", "OFF", "null", "Null", "NULL"};
  if (key.empty() || kResolved.contains(key)) return false;
  if (!(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
  return std::ranges::all_of(key, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  });
}

// Emits the parsed JSON tree in block style. yaml-cpp's node emitter drops
// the tag that marks a JSON string as quoted and then writes "2024", "true"
// or "off" plain, which a YAML loader reads back as an int or a bool. Every
// JSON string value (tag "!") is therefore emitted double-quoted, so the
// export keeps exactly the scalar types of render_json.
void emit_json_node(YAML::Emitter& out, const YAML::Node& node) {
  switch (node.Type()) {
    case YAML::NodeType::Map:
      out << YAML::BeginMap;
      for (const auto& entry : node) {
        const std::string& key = entry.first.Scalar();
        out << YAML::Key;
        if (plain_key(key)) {
          out << key;
        } else {
          out << YAML::DoubleQuoted << key;
        }
        out << YAML::Value;
        emit_json_node(out, entry.second);
      }
      out << YAML::EndMap;
      break;
    case YAML::NodeType::Sequence:
      out << YAML::BeginSeq;
      for (const auto& entry : node) emit_json_node(out, entry);
      out << YAML::EndSeq;
      break;
    case YAML::NodeType::Scalar:
      if (node.Tag() == "!") {
        out << YAML::DoubleQuoted << node.Scalar();
      } else {
        out << node.Scalar();
      }
      break;
    default:
      out << YAML::Null;
      break;
  }
}

}  // namespace

std::string render_yaml(const docv1::Document& document) {
  // The canonical JSON is already the exact structure this export promises;
  // YAML is a superset of JSON, so the parsed tree re-emits as the same
  // document in block-style YAML form, with every JSON string quoted.
  try {
    const YAML::Node tree = YAML::Load(render_json(document));
    YAML::Emitter emitter;
    emit_json_node(emitter, tree);
    if (!emitter.good()) {
      throw std::runtime_error("document YAML export failed: " + emitter.GetLastError());
    }
    return std::string(emitter.c_str());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("document YAML export failed: " + std::string(error.what()));
  }
}

}  // namespace grparse
