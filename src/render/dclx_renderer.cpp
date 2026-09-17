// DocLang OPC archive (`.dclx`) export: ZIP with root document.xml.
#include "grparse/document_render.h"

#include <vector>

#include "../targets/zip_writer.h"

namespace grparse {
namespace {

// Minimal OPC furniture matching grpc-xml's dclx fixtures: the archive is a
// DocLang ZIP when it carries a root document.xml member.
constexpr char kContentTypes[] = "<Types/>";
constexpr char kRels[] = "<Relationships/>";

}  // namespace

std::string render_dclx(const ai::pipestream::document::v1::Document& document) {
  const std::string doclang = render_doclang(document);
  const std::vector<targets::BundleFile> members = {
      {"[Content_Types].xml", kContentTypes},
      {"_rels/.rels", kRels},
      {"document.xml", doclang},
  };
  return targets::write_zip(members);
}

}  // namespace grparse
