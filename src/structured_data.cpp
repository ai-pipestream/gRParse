#include "grparse/structured_data.h"

#include <algorithm>
#include <string>
#include <utility>

#include "grparse/json_reader.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using json::Member;
using json::Value;

class JsonLdFold {
 public:
  JsonLdFold(docv1::StructuredData* data, JsonLdDecode* report)
      : data_(data), report_(report) {}

  // The top of a JSON-LD document: one node, or a graph of them.
  void top(const Value& object) {
    const Value* graph = object.find("@graph");
    const bool only_graph =
        graph != nullptr && graph->kind == Value::Kind::kArray &&
        std::all_of(object.members.begin(), object.members.end(), [](const Member& member) {
          return member.name == "@graph" || member.name == "@context";
        }) &&
        std::all_of(graph->elements.begin(), graph->elements.end(), [](const Value& element) {
          return element.kind == Value::Kind::kObject;
        });
    if (!only_graph) {
      node(object, data_->add_items());
      return;
    }
    for (const Member& member : object.members) {
      if (member.name == "@context") {
        context(member.value);
      } else {
        for (const Value& element : member.value.elements) node(element, data_->add_items());
      }
    }
  }

 private:
  void add_context(const std::string& iri) {
    const auto& known = data_->contexts();
    if (std::find(known.begin(), known.end(), iri) == known.end()) data_->add_contexts(iri);
  }

  void context(const Value& value) {
    switch (value.kind) {
      case Value::Kind::kString:
        add_context(value.string);
        return;
      case Value::Kind::kArray:
        for (const Value& element : value.elements) context(element);
        return;
      case Value::Kind::kNull:
        return;  // A null context resets the terms; it names no vocabulary.
      default:
        ++report_->inline_contexts;
        return;
    }
  }

  static bool all_strings(const Value& array) {
    return std::all_of(array.elements.begin(), array.elements.end(), [](const Value& element) {
      return element.kind == Value::Kind::kString;
    });
  }

  void node(const Value& object, docv1::LinkedDataItem* item) {
    for (const Member& member : object.members) {
      const Value& value = member.value;
      if (member.name == "@context") {
        context(value);
      } else if (member.name == "@id" && value.kind == Value::Kind::kString &&
                 !item->has_id()) {
        item->set_id(value.string);
      } else if (member.name == "@type" && value.kind == Value::Kind::kString) {
        item->add_types(value.string);
      } else if (member.name == "@type" && value.kind == Value::Kind::kArray &&
                 all_strings(value)) {
        for (const Value& type : value.elements) item->add_types(type.string);
      } else {
        docv1::LinkedDataProperty* property = item->add_properties();
        property->set_name(member.name);
        values(value, property);
      }
    }
  }

  static bool scalar(const Value& value) {
    return value.kind == Value::Kind::kString || value.kind == Value::Kind::kInteger ||
           value.kind == Value::Kind::kNumber || value.kind == Value::Kind::kBoolean;
  }

  static void set_scalar(const Value& value, docv1::LinkedDataValue* out) {
    switch (value.kind) {
      case Value::Kind::kString:
        out->set_text(value.string);
        return;
      case Value::Kind::kInteger:
        out->set_integer(value.integer);
        return;
      case Value::Kind::kNumber:
        out->set_number(value.number);
        return;
      case Value::Kind::kBoolean:
        out->set_boolean(value.boolean);
        return;
      default:
        return;
    }
  }

  // A value object: a scalar @value with, at most, a string @language and a
  // string @type, and nothing else.
  static bool value_object(const Value& object) {
    const Value* held = object.find("@value");
    if (held == nullptr || !scalar(*held)) return false;
    int value_members = 0;
    for (const Member& member : object.members) {
      if (member.name == "@value") {
        if (++value_members > 1) return false;
      } else if (member.name == "@language" || member.name == "@type") {
        if (member.value.kind != Value::Kind::kString) return false;
      } else {
        return false;
      }
    }
    return true;
  }

  void values(const Value& value, docv1::LinkedDataProperty* property) {
    switch (value.kind) {
      case Value::Kind::kNull:
        return;
      case Value::Kind::kArray:
        for (const Value& element : value.elements) values(element, property);
        return;
      case Value::Kind::kObject:
        break;
      default:
        set_scalar(value, property->add_values());
        return;
    }
    if (value_object(value)) {
      docv1::LinkedDataValue* out = property->add_values();
      for (const Member& member : value.members) {
        if (member.name == "@value") {
          set_scalar(member.value, out);
        } else if (member.name == "@language") {
          out->set_language(member.value.string);
        } else {
          out->set_datatype(member.value.string);
        }
      }
      return;
    }
    if (value.members.size() == 1) {
      const Member& only = value.members.front();
      if ((only.name == "@list" || only.name == "@set") &&
          only.value.kind == Value::Kind::kArray) {
        values(only.value, property);
        return;
      }
      if (only.name == "@id" && only.value.kind == Value::Kind::kString) {
        property->add_values()->set_reference(only.value.string);
        return;
      }
    }
    node(value, property->add_values()->mutable_item());
  }

  docv1::StructuredData* data_;
  JsonLdDecode* report_;
};

}  // namespace

JsonLdDecode decode_json_ld(std::string_view text, docv1::StructuredData* data) {
  JsonLdDecode report;
  data->set_dialect(docv1::STRUCTURED_DATA_KIND_JSON_LD);
  json::Parsed parsed = json::parse(text);
  if (!parsed.ok) {
    report.error = parsed.error + " at byte " + std::to_string(parsed.offset);
    return report;
  }
  const Value& root = parsed.value;
  const bool object = root.kind == Value::Kind::kObject;
  const bool objects =
      root.kind == Value::Kind::kArray &&
      std::all_of(root.elements.begin(), root.elements.end(), [](const Value& element) {
        return element.kind == Value::Kind::kObject;
      });
  if (!object && !objects) {
    report.error = "the block is JSON but not a JSON-LD document: its top level is neither an "
                   "object nor an array of objects";
    return report;
  }
  JsonLdFold fold(data, &report);
  if (object) {
    fold.top(root);
  } else {
    for (const Value& element : root.elements) fold.top(element);
  }
  report.decoded = true;
  return report;
}

}  // namespace grparse
