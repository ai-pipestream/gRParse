// The document's attachment list: an OLE payload is a nested document the
// fold does not open, so it is registered as an addressable attachment
// instead of leaving its class id as a bare string on a picture.
#pragma once

#include <string>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class AttachmentRegistry {
 public:
  explicit AttachmentRegistry(DocumentArena& arena) : arena_(arena) {}

  // Registers one embedded object as an attachment of the document: its
  // container class id, the container's own word for what it is, and the
  // item the payload became.
  void register_object(const officev1::EmbeddedObject& object,
                       const std::string& item_ref);

 private:
  DocumentArena& arena_;
  // Attachments are numbered in arrival order.
  int next_index_ = 0;
};

}  // namespace grparse::office_fold
