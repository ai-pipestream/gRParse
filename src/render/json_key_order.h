// Protobuf's JSON printer walks a map field in the order the map instance
// hands its entries over, and a proto map's iteration order is a property
// of the instance, not of its contents: two equal Documents can print
// different bytes. The printer offers no hook, so the order is fixed after
// the fact: every object the printer produced from a map field has its
// members sorted (numerically when every key is an integer, as page numbers
// are, bytewise otherwise). Objects printed from ordinary messages already
// follow field-number order and are copied through untouched.
#ifndef GRPARSE_RENDER_JSON_KEY_ORDER_H
#define GRPARSE_RENDER_JSON_KEY_ORDER_H

#include <set>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.h>

namespace grparse::render {

// The names of every map field reachable from `root`, as the printer writes
// them with preserve_proto_field_names (the proto field names).
std::set<std::string> map_field_names(const google::protobuf::Descriptor* root);

// `json` with the members of every object that is the value of a key in
// `map_fields` sorted. Every other byte is copied through unchanged, so the
// output of a compact printer stays compact. Throws std::invalid_argument
// on text that is not JSON.
std::string sort_map_objects(std::string_view json, const std::set<std::string>& map_fields);

}  // namespace grparse::render

#endif
