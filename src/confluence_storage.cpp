// The wiki storage handler's entry points: format detection and the guarded
// parse. The dialect itself is split across src/confluence/ - the character
// and entity helpers, the node tree, the pull parser, and the fold into a
// Document.
#include "grparse/confluence_storage.h"

#include <exception>
#include <new>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "confluence/storage_fold.h"
#include "confluence/storage_node.h"
#include "confluence/storage_parser.h"
#include "confluence/storage_text.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

// The parse proper. The exported entry point wraps it so that no failure
// mode, allocation included, leaves this handler by exception: a collector
// reports its failures as outcomes.
CollectorOutcome parse_storage_body(const std::string& bytes) {
  CollectorOutcome outcome;
  docv1::Document& document = outcome.document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  confluence::StorageParser parser(bytes, &outcome.warnings);
  const confluence::Node root = parser.parse();
  if (!parser.saw_element()) {
    outcome.warnings.clear();
    outcome.error =
        "confluence-storage: the body carries no markup, so it is not a "
        "storage-format document";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  confluence::StorageFold fold(&document, &outcome.warnings);
  fold.fold_blocks(root, "#/body");
  if (fold.emitted() == 0) {
    // Markup with nothing in it is a real page state (a stub, a page whose
    // only content is an unmapped bodiless macro); it parses, and says so.
    outcome.warnings.push_back(
        "the storage body carried markup but no mappable content");
  }
  outcome.success = true;
  return outcome;
}

}  // namespace

bool confluence_storage_format(const std::string& filename,
                               const std::string& content_type) {
  const std::string name = confluence::lowercase(filename);
  const std::string type = confluence::lowercase(content_type);
  return confluence::ends_with(name, ".confluence") ||
         confluence::ends_with(name, ".storage.xhtml") ||
         type == kConfluenceStorageMimetype;
}

CollectorOutcome parse_confluence_storage(const std::string& bytes) {
  CollectorOutcome outcome;
  try {
    return parse_storage_body(bytes);
  } catch (const std::bad_alloc&) {
    outcome.error = "confluence-storage: the body did not fit in memory";
    outcome.code = grpc::StatusCode::RESOURCE_EXHAUSTED;
  } catch (const std::exception& failure) {
    outcome.error = std::string("confluence-storage: ") + failure.what();
    outcome.code = grpc::StatusCode::INTERNAL;
  }
  return outcome;
}

}  // namespace grparse
