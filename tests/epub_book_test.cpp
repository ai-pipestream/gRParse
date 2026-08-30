// Proves the epub book fold on hand-built Documents: href resolution the
// way a reading system does it, chapter content re-parented under the
// chapter group the skeleton emitted, the skeleton's body-level picture
// retired when a chapter places the image (manifest facts and the epub
// source riding along), image bytes inlined as data URIs with the manifest
// media type winning, and every degradation reported rather than silent.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/base64.h"
#include "grparse/epub_book.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::RefItem ref(const std::string& value) {
  docv1::RefItem item;
  item.set_ref(value);
  return item;
}

// The skeleton the epub collector emits: empty chapter groups named by
// archive path, pictures by reference, all under the body.
docv1::Document skeleton(const std::vector<std::string>& chapter_hrefs,
                         const std::vector<std::string>& image_hrefs) {
  docv1::Document book;
  book.set_schema_name("docling_document_v2");
  book.mutable_body()->set_self_ref("#/body");
  book.mutable_furniture()->set_self_ref("#/furniture");
  book.mutable_origin()->set_mimetype("application/epub+zip");
  book.mutable_source_meta()->set_title("The Book");
  for (const auto& href : chapter_hrefs) {
    auto* group = book.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(book.groups_size() - 1));
    *group->mutable_parent() = ref("#/body");
    group->set_label(docv1::GROUP_LABEL_CHAPTER);
    group->set_name(href);
    (*group->mutable_meta()->mutable_custom_fields())["epub.idref"].set_string_value(href);
    *book.mutable_body()->add_children() = ref(group->self_ref());
  }
  for (const auto& href : image_hrefs) {
    auto* picture = book.add_pictures();
    picture->set_self_ref("#/pictures/" + std::to_string(book.pictures_size() - 1));
    *picture->mutable_parent() = ref("#/body");
    picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
    picture->mutable_image()->set_mimetype("image/jpeg");
    picture->mutable_image()->set_uri("epub:" + href);
    auto& fields = *picture->mutable_meta()->mutable_custom_fields();
    fields["epub.href"].set_string_value(href);
    fields["epub.manifest_id"].set_string_value("img-" + href);
    picture->add_source()->mutable_collector()->set_collector("epub");
    *book.mutable_body()->add_children() = ref(picture->self_ref());
  }
  return book;
}

// What the markup collector projects from one chapter's XHTML: a heading,
// a paragraph, and a picture with the author's relative src.
docv1::Document chapter_document(const std::string& heading, const std::string& src) {
  docv1::Document chapter;
  chapter.set_name("chapter.xhtml");
  chapter.mutable_body()->set_self_ref("#/body");
  chapter.mutable_furniture()->set_self_ref("#/furniture");
  chapter.mutable_origin()->set_mimetype("text/html");
  chapter.mutable_source_meta()->set_title(heading + " (page title)");
  auto* header = chapter.add_texts()->mutable_section_header()->mutable_base();
  header->set_self_ref("#/texts/0");
  *header->mutable_parent() = ref("#/body");
  header->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  header->set_text(heading);
  header->add_source()->mutable_collector()->set_collector("markup");
  auto* paragraph = chapter.add_texts()->mutable_text()->mutable_base();
  paragraph->set_self_ref("#/texts/1");
  *paragraph->mutable_parent() = ref("#/body");
  paragraph->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  paragraph->set_text("Body of " + heading);
  paragraph->add_source()->mutable_collector()->set_collector("markup");
  *chapter.mutable_body()->add_children() = ref("#/texts/0");
  *chapter.mutable_body()->add_children() = ref("#/texts/1");
  if (!src.empty()) {
    auto* picture = chapter.add_pictures();
    picture->set_self_ref("#/pictures/0");
    *picture->mutable_parent() = ref("#/body");
    picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
    picture->mutable_image()->set_mimetype("image/jpeg");
    picture->mutable_image()->set_uri(src);
    picture->add_source()->mutable_collector()->set_collector("markup");
    *chapter.mutable_body()->add_children() = ref("#/pictures/0");
  }
  return chapter;
}

const docv1::GroupItem* group_named(const docv1::Document& book, const std::string& name) {
  for (const auto& group : book.groups()) {
    if (group.has_name() && group.name() == name) return &group;
  }
  throw std::runtime_error("no group named " + name);
}

std::string text_of(const docv1::Document& book, const std::string& item_ref) {
  const int index = std::stoi(item_ref.substr(std::string("#/texts/").size()));
  const auto& item = book.texts(index);
  switch (item.item_case()) {
    case docv1::BaseTextItem::kSectionHeader: return item.section_header().base().text();
    case docv1::BaseTextItem::kText: return item.text().base().text();
    default: return std::string();
  }
}

void verify_href_resolution() {
  require(grparse::resolve_epub_href("OPS/text/ch01.xhtml", "../images/a.jpg") ==
              "OPS/images/a.jpg",
          "a relative src climbs out of the chapter's directory");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "images/a.jpg") == "OPS/images/a.jpg",
          "a sibling path resolves beside the chapter");
  require(grparse::resolve_epub_href("ch01.xhtml", "./a%20b.png") == "a b.png",
          "a root-level chapter, a dot segment, and a percent escape all resolve");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "/cover.jpg") == "cover.jpg",
          "a leading slash names the archive root");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "images/a.jpg#frag?x=1") ==
              "OPS/images/a.jpg",
          "fragment and query are dropped");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "../../../a.jpg") == "a.jpg",
          "climbing past the root stops at the root");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "https://example.org/a.jpg").empty(),
          "an absolute URL is not an archive entry");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "data:image/png;base64,AAAA").empty(),
          "a data URI is not an archive entry");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "#top").empty(),
          "a fragment-only reference names nothing");
  require(grparse::resolve_epub_href("OPS/ch01.xhtml", "c:file.jpg").empty(),
          "anything with a scheme is left alone");
}

void verify_chapters_plug_into_their_groups() {
  docv1::Document book = skeleton({"OPS/ch01.xhtml", "OPS/ch02.xhtml"}, {});
  std::vector<grparse::ParsedChapter> chapters;
  chapters.push_back({"OPS/ch01.xhtml", chapter_document("One", "")});
  chapters.push_back({"OPS/ch02.xhtml", chapter_document("Two", "")});
  std::vector<std::string> warnings;
  grparse::fold_epub_book(std::move(chapters), {}, &book, &warnings);

  require(warnings.empty(), "a clean fold reports nothing");
  require(book.texts_size() == 4, "both chapters' texts landed in the book");
  const auto& first = *group_named(book, "OPS/ch01.xhtml");
  const auto& second = *group_named(book, "OPS/ch02.xhtml");
  require(first.children_size() == 2 && second.children_size() == 2,
          "each chapter group holds its own two items");
  require(text_of(book, first.children(0).ref()) == "One" &&
              text_of(book, first.children(1).ref()) == "Body of One" &&
              text_of(book, second.children(0).ref()) == "Two",
          "the items sit under the chapter that produced them, in order");
  for (const auto& item : book.texts()) {
    const auto& base = item.item_case() == docv1::BaseTextItem::kSectionHeader
                           ? item.section_header().base()
                           : item.text().base();
    require(base.parent().ref() == "#/groups/0" || base.parent().ref() == "#/groups/1",
            "every chapter item's parent is its chapter group, not the body");
  }
  require(book.body().children_size() == 2,
          "the body still lists exactly the two chapter groups");
  require(book.source_meta().title() == "The Book" && book.name().empty(),
          "a chapter's page title never becomes the book's identity");
  require(book.origin().mimetype() == "application/epub+zip",
          "the chapter's text/html origin does not overwrite the book's");
}

void verify_images_are_placed_and_inlined() {
  docv1::Document book = skeleton({"OPS/ch01.xhtml"}, {"OPS/images/a.jpg", "OPS/cover.png"});
  std::vector<grparse::ParsedChapter> chapters;
  chapters.push_back({"OPS/ch01.xhtml", chapter_document("One", "images/a.jpg")});
  std::vector<grparse::EpubResource> resources = {
      {"OPS/images/a.jpg", "image/jpeg", "JPEGBYTES"},
      {"OPS/cover.png", "image/png", "PNGBYTES"},
  };
  std::vector<std::string> warnings;
  grparse::fold_epub_book(std::move(chapters), resources, &book, &warnings);

  require(warnings.empty(), "a fold with every image present reports nothing");
  require(book.pictures_size() == 2,
          "the placed image has one picture, not a skeleton one plus a chapter one");

  // The cover, which no chapter placed, keeps its body-level seat.
  const auto& cover = book.pictures(0);
  require(cover.parent().ref() == "#/body" && cover.self_ref() == "#/pictures/0",
          "the unplaced cover stays at the body as picture 0");
  require(cover.image().uri() == "data:image/png;base64," + grparse::encode_base64("PNGBYTES", 8),
          "the cover's bytes are inlined under its manifest media type");

  // The chapter's picture carries the bytes and the manifest facts.
  const auto& chapter = *group_named(book, "OPS/ch01.xhtml");
  require(chapter.children_size() == 3 && chapter.children(2).ref() == "#/pictures/1",
          "the chapter's picture is its third child, renumbered after the cover");
  const auto& placed = book.pictures(1);
  require(placed.parent().ref() == "#/groups/0", "the placed picture's parent is the chapter");
  require(placed.image().uri() == "data:image/jpeg;base64," + grparse::encode_base64("JPEGBYTES", 9),
          "the placed picture's bytes are inlined");
  const auto& fields = placed.meta().custom_fields();
  require(fields.contains("html.src") && fields.at("html.src").string_value() == "images/a.jpg",
          "the author's relative src survives as a custom field");
  require(fields.contains("epub.manifest_id") &&
              fields.at("epub.manifest_id").string_value() == "img-OPS/images/a.jpg",
          "the retired skeleton picture's manifest facts moved onto the chapter picture");
  bool epub_source = false;
  bool markup_source = false;
  for (const auto& source : placed.source()) {
    if (source.collector().collector() == "epub") epub_source = true;
    if (source.collector().collector() == "markup") markup_source = true;
  }
  require(epub_source && markup_source,
          "the placed picture is attributed to both the locator and the supplier");
  for (const auto& child : book.body().children()) {
    require(child.ref() != "#/pictures/1" && child.ref() != "#/pictures/2",
            "no body child still names a retired or renumbered picture");
  }
}

void verify_degradations_are_reported() {
  docv1::Document book = skeleton({"OPS/ch01.xhtml"}, {"OPS/images/missing.jpg"});
  std::vector<grparse::ParsedChapter> chapters;
  chapters.push_back({"OPS/ch01.xhtml", chapter_document("One", "https://example.org/x.png")});
  chapters.push_back({"OPS/stray.xhtml", chapter_document("Stray", "")});
  std::vector<grparse::EpubResource> resources = {
      {"OPS/images/huge.jpg", "image/jpeg",
       std::string(grparse::kEpubInlineImageCap + 1, 'x')},
  };
  book.mutable_pictures(0)->mutable_image()->set_uri("epub:OPS/images/huge.jpg");
  auto* absent = book.add_pictures();
  absent->set_self_ref("#/pictures/1");
  *absent->mutable_parent() = ref("#/body");
  absent->mutable_image()->set_uri("epub:OPS/images/missing.jpg");
  *book.mutable_body()->add_children() = ref("#/pictures/1");

  std::vector<std::string> warnings;
  grparse::fold_epub_book(std::move(chapters), resources, &book, &warnings);

  require(group_named(book, "OPS/ch01.xhtml")->children_size() == 3,
          "the chapter with a group still folds");
  const auto& stray_last = book.body().children(book.body().children_size() - 1);
  require(text_of(book, stray_last.ref()) == "Body of Stray",
          "a chapter without a group joins the body's end instead of vanishing");
  bool remote_untouched = false;
  for (const auto& picture : book.pictures()) {
    if (picture.image().uri() == "https://example.org/x.png") remote_untouched = true;
    require(!picture.image().uri().starts_with("data:") ||
                picture.image().uri().find("huge") == std::string::npos,
            "an oversized image is never inlined");
  }
  require(remote_untouched, "a remote image keeps its URL");
  require(book.pictures(0).image().uri() == "epub:OPS/images/huge.jpg",
          "the oversized image keeps its reference");
  require(book.pictures(1).image().uri() == "epub:OPS/images/missing.jpg",
          "an image whose bytes never arrived keeps its reference");

  int stray = 0;
  int huge = 0;
  int missing = 0;
  for (const auto& warning : warnings) {
    if (warning.contains("OPS/stray.xhtml") && warning.contains("no chapter group")) stray++;
    if (warning.contains("huge.jpg") && warning.contains("inline cap")) huge++;
    if (warning.contains("missing.jpg") && warning.contains("not in the stream")) missing++;
  }
  require(stray == 1 && huge == 1 && missing == 1,
          "each degradation is reported exactly once");
}

}  // namespace

int main() {
  try {
    verify_href_resolution();
    verify_chapters_plug_into_their_groups();
    verify_images_are_placed_and_inlined();
    verify_degradations_are_reported();
  } catch (const std::exception& failure) {
    std::println(stderr, "FAILED: {}", failure.what());
    return 1;
  }
  std::println("epub book test passed");
  return 0;
}
