// render_json (and render_yaml, which re-emits it) is a pure function of
// the document's CONTENT, not of the wire map instance it happens to hold.
// The protobuf JSON printer walks a map field in the order that map hands its
// entries over, and a proto map's iteration order is a property of the
// instance; render/json_key_order.h orders those objects afterwards. Two
// documents MessageDifferencer calls equal must render identical bytes.
//
// The first case narrows it down: a document with no map field at all is
// stable without that pass, so the map is what moved.
#include <string>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse_test::add_page;
using grparse_test::add_paragraph;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

docv1::Document without_maps() {
  docv1::Document document = base_document("plain.pdf");
  add_paragraph(&document, "#/body", "one paragraph, no map field anywhere");
  return document;
}

docv1::Document with_a_page_map() {
  docv1::Document document = without_maps();
  // Inserted back to front, which is how a page-streamed parse fills the map.
  for (const int page_no : {5, 1, 9, 3, 7, 2}) add_page(&document, page_no, 612, 792);
  return document;
}

docv1::Document with_a_custom_field_map() {
  docv1::Document document = without_maps();
  auto& fields = *document.mutable_texts(0)->mutable_text()->mutable_base()->mutable_meta()
                      ->mutable_custom_fields();
  for (const char* name :
       {"zeta__z", "alpha__a", "mu__m", "beta__b", "omega__o", "kappa__k"}) {
    fields[name].set_string_value(name);
  }
  return document;
}

void require_stable(const docv1::Document& first, const docv1::Document& second,
                    const std::string& what) {
  require(google::protobuf::util::MessageDifferencer::Equals(first, second),
          what + ": the two documents must be equal to begin with");
  require_equal(grparse::render_json(first) == grparse::render_json(second), true,
                what + ": two equal documents must render identical protobuf JSON");
  require_equal(grparse::render_yaml(first) == grparse::render_yaml(second), true,
                what + ": two equal documents must render identical YAML");
}

void verify_a_document_with_no_map_field_is_already_stable() {
  require_stable(without_maps(), without_maps(), "a document with no map field");
}

void verify_a_page_map_does_not_decide_the_output_order() {
  require_stable(with_a_page_map(), with_a_page_map(), "a document with a page map");
}

void verify_a_custom_field_map_does_not_decide_the_output_order() {
  require_stable(with_a_custom_field_map(), with_a_custom_field_map(),
                 "a document with a custom meta field map");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("render-map-order-test", "ok", {
      verify_a_document_with_no_map_field_is_already_stable,
      verify_a_page_map_does_not_decide_the_output_order,
      verify_a_custom_field_map_does_not_decide_the_output_order,
  });
}
