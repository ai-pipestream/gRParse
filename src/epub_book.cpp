#include "grparse/epub_book.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "grparse/base64.h"
#include "grparse/document_merge.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

constexpr std::string_view kEpubScheme = "epub:";
constexpr std::string_view kBodyRef = "#/body";

bool has_scheme(std::string_view reference) {
  if (reference.empty() || std::isalpha(static_cast<unsigned char>(reference[0])) == 0) {
    return false;
  }
  for (const char c : reference) {
    if (c == ':') return true;
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '+' && c != '-' &&
        c != '.') {
      return false;
    }
  }
  return false;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percent_decode(std::string_view encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      const int high = hex_value(encoded[i + 1]);
      const int low = hex_value(encoded[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(encoded[i]);
  }
  return decoded;
}

// Collapses `.` and `..` segments and empty segments the way a reading
// system resolves an archive path; a `..` with nothing left to climb is
// dropped, because the archive root is as far up as a path can go.
std::string normalize_path(std::string_view path) {
  std::vector<std::string> segments;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view segment = path.substr(start, end - start);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.emplace_back(segment);
    }
    start = end + 1;
  }
  std::string joined;
  for (const auto& segment : segments) {
    if (!joined.empty()) joined.push_back('/');
    joined += segment;
  }
  return joined;
}

std::string epub_reference(const std::string& href) {
  return std::string(kEpubScheme) + href;
}

// The archive path an `epub:` reference names, or nullopt for any other URI.
std::optional<std::string> epub_href_of(const std::string& uri) {
  if (!uri.starts_with(kEpubScheme)) return std::nullopt;
  return uri.substr(kEpubScheme.size());
}

std::string arena_ref(std::string_view prefix, int index) {
  return std::string(prefix) + std::to_string(index);
}

// The parent slot of the item `ref` names, across every arena an item can
// live in; nullptr for a reference that names nothing here.
docv1::RefItem* mutable_parent_of(docv1::Document* document, const std::string& ref) {
  const auto parse = [&ref](std::string_view prefix) -> int {
    if (!ref.starts_with(prefix)) return -1;
    const std::string_view rest(ref.data() + prefix.size(), ref.size() - prefix.size());
    if (rest.empty() || !std::ranges::all_of(rest, [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      return -1;
    }
    return std::stoi(std::string(rest));
  };
  if (const int index = parse("#/texts/"); index >= 0 && index < document->texts_size()) {
    auto* item = document->mutable_texts(index);
    switch (item->item_case()) {
      case docv1::BaseTextItem::kTitle: return item->mutable_title()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kSectionHeader:
        return item->mutable_section_header()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kListItem:
        return item->mutable_list_item()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kFormula:
        return item->mutable_formula()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kText: return item->mutable_text()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kCode: return item->mutable_code()->mutable_parent();
      case docv1::BaseTextItem::kFieldHeading:
        return item->mutable_field_heading()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::kFieldValue:
        return item->mutable_field_value()->mutable_base()->mutable_parent();
      case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
    }
    return nullptr;
  }
  if (const int index = parse("#/pictures/"); index >= 0 && index < document->pictures_size()) {
    return document->mutable_pictures(index)->mutable_parent();
  }
  if (const int index = parse("#/tables/"); index >= 0 && index < document->tables_size()) {
    return document->mutable_tables(index)->mutable_parent();
  }
  if (const int index = parse("#/groups/"); index >= 0 && index < document->groups_size()) {
    return document->mutable_groups(index)->mutable_parent();
  }
  if (const int index = parse("#/key_value_items/");
      index >= 0 && index < document->key_value_items_size()) {
    return document->mutable_key_value_items(index)->mutable_parent();
  }
  if (const int index = parse("#/form_items/");
      index >= 0 && index < document->form_items_size()) {
    return document->mutable_form_items(index)->mutable_parent();
  }
  if (const int index = parse("#/field_regions/");
      index >= 0 && index < document->field_regions_size()) {
    return document->mutable_field_regions(index)->mutable_parent();
  }
  if (const int index = parse("#/field_items/");
      index >= 0 && index < document->field_items_size()) {
    return document->mutable_field_items(index)->mutable_parent();
  }
  return nullptr;
}

// A chapter Document's document-level identity is the chapter's, not the
// book's: its <title> is not the book's title and its origin is a
// fragment's. Only content survives into the book.
void strip_chapter_identity(docv1::Document* chapter) {
  chapter->clear_name();
  chapter->clear_origin();
  chapter->clear_source_meta();
  chapter->clear_claims();
  chapter->clear_media();
  chapter->clear_email();
  chapter->clear_page_styles();
  chapter->clear_meta_tags();
  chapter->clear_changes();
}

// Re-points every chapter picture whose `src` names an archive entry at
// `epub:<href>`, and returns the hrefs so referenced. The raw attribute
// value survives as a custom field for anyone who needs the author's text.
std::set<std::string> localize_chapter_pictures(const std::string& chapter_href,
                                                docv1::Document* chapter) {
  std::set<std::string> referenced;
  for (auto& picture : *chapter->mutable_pictures()) {
    if (!picture.has_image()) continue;
    const std::string resolved = resolve_epub_href(chapter_href, picture.image().uri());
    if (resolved.empty()) continue;
    auto& fields = *picture.mutable_meta()->mutable_custom_fields();
    if (!fields.contains("html.src")) {
      fields["html.src"].set_string_value(picture.image().uri());
    }
    picture.mutable_image()->set_uri(epub_reference(resolved));
    referenced.insert(resolved);
  }
  return referenced;
}

// The skeleton's body-level picture for an image resource, by href.
struct SkeletonPicture {
  google::protobuf::Map<std::string, google::protobuf::Value> fields;
  google::protobuf::RepeatedPtrField<docv1::SourceType> sources;
};

// Retires the skeleton pictures the chapters now place, renumbering the
// pictures arena and every reference into it, and hands back their
// manifest facts keyed by href so they can ride on the chapter pictures.
std::map<std::string, SkeletonPicture> retire_placed_pictures(
    const std::set<std::string>& placed, docv1::Document* book) {
  std::map<std::string, SkeletonPicture> retired;
  // Decide first, move nothing until something is retired: the common case
  // (no chapter places a skeleton picture) leaves the arena untouched.
  std::set<std::string> retired_refs;
  for (const auto& picture : book->pictures()) {
    const auto href = picture.has_image() ? epub_href_of(picture.image().uri()) : std::nullopt;
    const bool body_level = picture.has_parent() && picture.parent().ref() == kBodyRef;
    if (href.has_value() && body_level && placed.contains(*href) &&
        !retired.contains(*href)) {
      SkeletonPicture facts;
      if (picture.has_meta()) facts.fields = picture.meta().custom_fields();
      facts.sources = picture.source();
      retired.emplace(*href, std::move(facts));
      retired_refs.insert(picture.self_ref());
    }
  }
  if (retired.empty()) return retired;

  std::map<std::string, std::string> renumbering;
  google::protobuf::RepeatedPtrField<docv1::PictureItem> kept;
  int next = 0;
  for (auto& picture : *book->mutable_pictures()) {
    if (retired_refs.contains(picture.self_ref())) continue;
    const std::string new_ref = arena_ref("#/pictures/", next++);
    if (picture.self_ref() != new_ref) {
      renumbering[picture.self_ref()] = new_ref;
      picture.set_self_ref(new_ref);
    }
    *kept.Add() = std::move(picture);
  }
  book->mutable_pictures()->Swap(&kept);

  const auto prune_children = [&retired_refs](docv1::GroupItem* group) {
    auto* children = group->mutable_children();
    children->erase(std::remove_if(children->begin(), children->end(),
                                   [&retired_refs](const docv1::RefItem& child) {
                                     return retired_refs.contains(child.ref());
                                   }),
                    children->end());
  };
  prune_children(book->mutable_body());
  prune_children(book->mutable_furniture());
  for (auto& group : *book->mutable_groups()) prune_children(&group);
  if (!renumbering.empty()) rewrite_references(renumbering, book);
  return retired;
}

void adopt_manifest_facts(const std::map<std::string, SkeletonPicture>& facts,
                          docv1::Document* book, int first_picture) {
  for (int index = first_picture; index < book->pictures_size(); ++index) {
    auto* picture = book->mutable_pictures(index);
    if (!picture->has_image()) continue;
    const auto href = epub_href_of(picture->image().uri());
    if (!href.has_value()) continue;
    const auto found = facts.find(*href);
    if (found == facts.end()) continue;
    auto& fields = *picture->mutable_meta()->mutable_custom_fields();
    for (const auto& [key, value] : found->second.fields) {
      if (!fields.contains(key)) fields[key] = value;
    }
    for (const auto& source : found->second.sources) {
      const bool known = std::ranges::any_of(picture->source(), [&source](const auto& have) {
        return have.has_collector() && source.has_collector() &&
               have.collector().collector() == source.collector().collector();
      });
      if (!known) *picture->add_source() = source;
    }
  }
}

// Moves the body children appended past `first_child` under `group`,
// re-parenting each item so the tree and the items agree.
void reparent_tail(docv1::Document* book, int first_child, int group_index) {
  auto* body = book->mutable_body();
  if (first_child >= body->children_size()) return;
  auto* group = book->mutable_groups(group_index);
  const std::string group_ref = group->self_ref().empty()
                                    ? arena_ref("#/groups/", group_index)
                                    : group->self_ref();
  for (int index = first_child; index < body->children_size(); ++index) {
    const std::string ref = body->children(index).ref();
    *group->add_children() = body->children(index);
    if (auto* parent = mutable_parent_of(book, ref); parent != nullptr) {
      parent->set_ref(group_ref);
    }
  }
  body->mutable_children()->DeleteSubrange(first_child,
                                           body->children_size() - first_child);
}

std::string data_uri(const std::string& media_type, const std::string& bytes) {
  return "data:" + (media_type.empty() ? std::string("application/octet-stream") : media_type) +
         ";base64," + encode_base64(bytes.data(), bytes.size());
}

void inline_images(const std::vector<EpubResource>& resources, docv1::Document* book,
                   std::vector<std::string>* warnings) {
  std::map<std::string, const EpubResource*> by_href;
  for (const auto& resource : resources) by_href.emplace(resource.href, &resource);
  std::set<std::string> reported;
  const auto warn_once = [&](const std::string& href, std::string text) {
    if (warnings == nullptr || !reported.insert(href).second) return;
    warnings->push_back(std::move(text));
  };
  for (auto& picture : *book->mutable_pictures()) {
    if (!picture.has_image()) continue;
    const auto href = epub_href_of(picture.image().uri());
    if (!href.has_value()) continue;
    const auto found = by_href.find(*href);
    if (found == by_href.end()) {
      warn_once(*href, "image '" + *href + "' is referenced but its bytes were not in the stream");
      continue;
    }
    const EpubResource& resource = *found->second;
    if (resource.content.size() > kEpubInlineImageCap) {
      warn_once(*href, "image '" + *href + "' (" + std::to_string(resource.content.size()) +
                           " bytes) exceeds the inline cap; kept as a reference");
      continue;
    }
    auto* image = picture.mutable_image();
    if (!resource.media_type.empty()) image->set_mimetype(resource.media_type);
    image->set_uri(data_uri(image->mimetype(), resource.content));
  }
}

}  // namespace

std::string resolve_epub_href(const std::string& chapter_href, const std::string& reference) {
  std::string_view trimmed(reference);
  if (const size_t cut = trimmed.find_first_of("#?"); cut != std::string_view::npos) {
    trimmed = trimmed.substr(0, cut);
  }
  if (trimmed.empty() || has_scheme(trimmed)) return std::string();
  const std::string decoded = percent_decode(trimmed);
  if (decoded.starts_with('/')) return normalize_path(decoded);
  std::string base_dir;
  if (const size_t slash = chapter_href.rfind('/'); slash != std::string::npos) {
    base_dir = chapter_href.substr(0, slash + 1);
  }
  return normalize_path(base_dir + decoded);
}

void fold_epub_book(std::vector<ParsedChapter> chapters,
                    const std::vector<EpubResource>& resources, docv1::Document* book,
                    std::vector<std::string>* warnings) {
  std::map<std::string, int> chapter_groups;
  for (int index = 0; index < book->groups_size(); ++index) {
    const auto& group = book->groups(index);
    if (group.label() == docv1::GROUP_LABEL_CHAPTER && group.has_name()) {
      chapter_groups.emplace(group.name(), index);
    }
  }

  std::set<std::string> placed;
  for (auto& chapter : chapters) {
    strip_chapter_identity(&chapter.document);
    placed.merge(localize_chapter_pictures(chapter.href, &chapter.document));
  }
  const auto manifest_facts = retire_placed_pictures(placed, book);
  const int first_chapter_picture = book->pictures_size();

  for (auto& chapter : chapters) {
    const int first_child = book->body().children_size();
    merge_documents(std::move(chapter.document), book);
    const auto group = chapter_groups.find(chapter.href);
    if (group == chapter_groups.end()) {
      if (warnings != nullptr) {
        warnings->push_back("chapter '" + chapter.href +
                            "' has no chapter group; its content joins the body");
      }
      continue;
    }
    reparent_tail(book, first_child, group->second);
  }

  adopt_manifest_facts(manifest_facts, book, first_chapter_picture);
  inline_images(resources, book, warnings);
}

}  // namespace grparse
