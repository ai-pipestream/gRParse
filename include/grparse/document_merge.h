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

// Records `claimant` as the source of every singular field `tracked`
// currently answers, for a message that carries a `field_sources` list.
// The service uses it on the identity it stamps before any collector runs.
void claim_fields(google::protobuf::Message* tracked,
                  const ai::pipestream::document::v1::CollectorSource& claimant);

// Rewrites every item reference (RefItem.ref, FineRef.ref, an item's own
// self_ref) that `renumbering` maps, anywhere under `message`; values it
// does not map pass through. The merge uses it to renumber appended arenas,
// and anything else that removes or reorders arena items owes the same
// rewrite to every reference into them.
void rewrite_references(const std::map<std::string, std::string>& renumbering,
                        google::protobuf::Message* message);

}  // namespace grparse
