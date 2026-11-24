#pragma once

// Decodes a structured data block a source embeds (a JSON-LD script) into
// document.v1.StructuredData: the things the block describes, as typed
// items with types, identifiers and properties. A consumer reads the items;
// it never parses the block's text.

#include <cstdint>
#include <string>
#include <string_view>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

struct JsonLdDecode {
  // True when the block is JSON and a JSON-LD document (an object, or an
  // array of objects). When false, `data` holds no items and `error` says
  // why, with the byte offset for a syntax error.
  bool decoded = false;
  std::string error;
  // Inline @context objects (term definitions) the block holds. They have
  // no place in StructuredData.contexts, which lists vocabulary IRIs, so
  // they are counted and the caller says so.
  uint64_t inline_contexts = 0;
};

// Decodes `text`, the content of one JSON-LD script, onto `data`: dialect,
// contexts and items. The mapping loses nothing a JSON-LD document states
// outside an inline context:
//
// - a node object is a LinkedDataItem: @id is its id, @type its types,
//   every other member a property under the name the source wrote;
// - a top-level object that holds only a @graph (and a @context) is the
//   graph's nodes; a top-level array is its nodes;
// - a member's value is one LinkedDataValue per value: a string, an
//   integer, a number, a boolean, a nested node, or a reference for an
//   object that holds @id alone; an array is its elements in order, and a
//   null is no value;
// - a value object ({"@value": ..., "@language": ..., "@type": ...}) is
//   the scalar with its language and datatype; a @list or @set is its
//   members in order.
//
// An object that mixes these shapes in a way JSON-LD does not define is
// kept as a nested item with its members as written, so nothing is
// dropped to make it fit.
JsonLdDecode decode_json_ld(std::string_view text,
                            ai::pipestream::document::v1::StructuredData* data);

}  // namespace grparse
