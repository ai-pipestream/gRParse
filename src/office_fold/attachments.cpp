#include "grparse/office_fold/attachments.h"

#include <algorithm>
#include <cctype>

namespace grparse::office_fold {

namespace {

// The container's own word for what the payload is, as the schema spells
// it: the enum name without its prefix, lowercased.
std::string kind_name(officev1::EmbeddedObjectKind kind) {
  std::string name = officev1::EmbeddedObjectKind_Name(kind);
  const std::string prefix = "EMBEDDED_OBJECT_KIND_";
  if (name.starts_with(prefix)) name = name.substr(prefix.size());
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  return name;
}

}  // namespace

void AttachmentRegistry::register_object(
    const officev1::EmbeddedObject& object, const std::string& item_ref) {
  docv1::SubDocumentRef* attachment = arena_.document().add_attachments();
  attachment->set_id("object:" + std::to_string(next_index_++));
  attachment->set_name(object.name());
  attachment->set_media_type(object.replacement_mime_type());
  attachment->set_size_bytes(object.replacement_image().size());
  if (!item_ref.empty()) attachment->set_item_ref(item_ref);
  if (!object.clsid().empty()) attachment->set_class_id(object.clsid());
  const std::string kind = kind_name(object.kind());
  if (kind != "unspecified") attachment->set_kind(kind);
}

}  // namespace grparse::office_fold
