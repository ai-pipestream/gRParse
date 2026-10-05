#pragma once

#include <map>
#include <string>

#include <google/protobuf/message.h>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Additively merges `source` into `target`: source's arena items append
// with renumbered self_refs so references stay unique, every reference
// inside the moved items is rewritten to match, body and furniture children
// and metadata append, and pages merge by page number with target winning a
// collision. Everything else merges by reflection under one rule: a
// singular field the target has not answered takes the source's answer, a
// message both answered merges field by field, a list appends, and a map
// keeps the target's entry for a key both carry. Nothing already in target
// is modified, which is the scatter-gather rule: sources never overwrite
// each other.
void merge_documents(ai::pipestream::document::v1::Document&& source,
                     ai::pipestream::document::v1::Document* target);

// The same merge with the source's collector named. The collector's whole
// document-level account is kept on Document.claims, and on the messages
// that track provenance (DocumentMeta, DocumentOrigin) every singular field
// records which collector's answer it carries. Where both sides answered
// one field, the one that ranks higher for the document's format wins
// (see document_claim_rank); on a tie the target's answer stands.
void merge_documents(ai::pipestream::document::v1::Document&& source,
                     ai::pipestream::document::v1::Document* target,
                     const ai::pipestream::document::v1::CollectorSource& claimant);

// The standing a collector's document-level answers have for a document of
// `mimetype`: the service's own stamp outranks everything, the format's
// native reader outranks a converter, and a collector with no standing for
// the format ranks zero. Confidence, when both sides state one, is compared
// before rank.
int document_claim_rank(const std::string& collector, const std::string& mimetype);

// Reduces `source` to the document-level account the claims machinery keeps
// (the same set claim_of carries onto Document.claims: source_meta, origin,
// page_styles, email, media). Arenas, body and furniture children,
// attachments, named ranges and pages drop. The coordinator applies it to a
// routed office fan-out leg once the plan's primary has contributed a body:
// the leg read the same bytes, and merging its reading would print the body
// a second time, while its claims still merge and rank.
void retain_claims_only(ai::pipestream::document::v1::Document* source);

// Clears the document-level identity of a fragment that is about to merge
// into a host document as content: its name, origin, source metadata,
// claims, media, email envelope, page styles, meta tags and changes. An epub
// chapter's <title> is not the book's title, and an email's HTML body is not
// the message: only the fragment's content belongs in the host.
void strip_document_identity(ai::pipestream::document::v1::Document* fragment);

// Records `claimant` as the source of every singular field `tracked`
// currently answers, for a message that carries a `field_sources` list.
// The service uses it on the identity it stamps before any collector runs.
void claim_fields(google::protobuf::Message* tracked,
                  const ai::pipestream::document::v1::CollectorSource& claimant);

// Rewrites every item reference (RefItem.ref, FineRef.ref, an item's own
// self_ref, SubDocumentRef.item_ref) that `renumbering` maps, anywhere under `message`; values it
// does not map pass through. The merge uses it to renumber appended arenas,
// and anything else that removes or reorders arena items owes the same
// rewrite to every reference into them.
void rewrite_references(const std::map<std::string, std::string>& renumbering,
                        google::protobuf::Message* message);

// Records every renumbering rewrite_references applies on this thread while
// it lives, composed into one map from each ref as it stood when the log
// opened to the ref it names now. The streaming surface wraps the
// whole-document passes in one, so it can tell clients which refs the page
// events carried were renamed rather than resending every item after a
// retired one. Nested logs each see the renumberings made in their scope.
class ReferenceRenameLog {
 public:
  ReferenceRenameLog();
  ~ReferenceRenameLog();
  ReferenceRenameLog(const ReferenceRenameLog&) = delete;
  ReferenceRenameLog& operator=(const ReferenceRenameLog&) = delete;

  // Composes one renumbering step onto what the log holds.
  void record(const std::map<std::string, std::string>& step);
  // Original ref to current ref, for every ref whose name changed.
  const std::map<std::string, std::string>& renames() const { return current_of_; }

 private:
  ReferenceRenameLog* previous_;
  std::map<std::string, std::string> current_of_;
};

}  // namespace grparse
