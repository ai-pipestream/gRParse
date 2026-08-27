#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/docling_map.h"
#include "grparse/document_merge.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

void add_text(docv1::Document* document, const std::string& parent,
              const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(ref);
  }
}

// A collector-shaped source document: a group holding a text, a table whose
// comment points at the text, and a page. Every reference must survive the
// renumbering.
docv1::Document collector_document() {
  docv1::Document source = base_document();
  auto* group = source.add_groups();
  group->set_self_ref("#/groups/0");
  group->mutable_parent()->set_ref("#/body");
  group->set_label(docv1::GROUP_LABEL_SHEET);
  source.mutable_body()->add_children()->set_ref("#/groups/0");

  auto* base = source.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/groups/0");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text("collected text");
  group->add_children()->set_ref("#/texts/0");

  auto* table = source.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->add_comments()->set_ref("#/texts/0");
  source.mutable_body()->add_children()->set_ref("#/tables/0");

  (*source.mutable_pages())[2].set_page_no(2);
  (*source.mutable_body()->mutable_meta()->mutable_custom_fields())["named_range:R"]
      .set_string_value("A1:B2");
  source.set_schema_name("docling_document_v2");
  source.set_version("1.10.0");
  return source;
}

void verify_merge_renumbers_and_rewrites() {
  docv1::Document target = base_document();
  // The base document's identity is authoritative; a collector document
  // carrying its own identity must never overwrite it.
  target.set_schema_name("docling_document_v2");
  target.set_version("1.10.0");
  add_text(&target, "#/body", "existing text");
  auto* existing_table = target.add_tables();
  existing_table->set_self_ref("#/tables/0");
  existing_table->mutable_parent()->set_ref("#/body");
  existing_table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  target.mutable_body()->add_children()->set_ref("#/tables/0");
  (*target.mutable_pages())[1].set_page_no(1);

  grparse::merge_documents(collector_document(), &target);

  require(target.schema_name() == "docling_document_v2" &&
              target.version() == "1.10.0",
          "the merged document keeps the base identity");
  require(target.texts_size() == 2 && target.tables_size() == 2 &&
              target.groups_size() == 1,
          "merge appends every arena");
  require(target.texts(0).text().base().self_ref() == "#/texts/0" &&
              target.texts(0).text().base().text() == "existing text",
          "existing items stay untouched");
  const auto& moved = target.texts(1).text().base();
  require(moved.self_ref() == "#/texts/1" && moved.text() == "collected text",
          "moved text is renumbered past the existing arena");
  require(moved.parent().ref() == "#/groups/0",
          "moved text keeps its parent group");
  require(target.groups(0).children(0).ref() == "#/texts/1",
          "group child references follow the renumbering");
  require(target.tables(1).self_ref() == "#/tables/1",
          "moved table is renumbered past the existing table");
  require(target.tables(1).comments(0).ref() == "#/texts/1",
          "fine references follow the renumbering");
  require(target.body().children_size() == 4 &&
              target.body().children(2).ref() == "#/groups/0" &&
              target.body().children(3).ref() == "#/tables/1",
          "body children append with rewritten references");
  require(target.pages().size() == 2 && target.pages().at(2).page_no() == 2,
          "pages merge by page number");
  require(target.body().meta().custom_fields().count("named_range:R") == 1,
          "body metadata merges additively");
  const auto errors = grparse::docling_integrity_errors(target);
  for (const auto& error : errors) std::println(stderr, "integrity: {}", error);
  require(errors.empty(), "merged document ref tree stays well formed");
}

// Merging the same collector output twice keeps both copies addressable:
// additive means never overwriting, even on replay.
void verify_merge_is_additive_on_replay() {
  docv1::Document target = base_document();
  grparse::merge_documents(collector_document(), &target);
  grparse::merge_documents(collector_document(), &target);
  require(target.texts_size() == 2 && target.groups_size() == 2,
          "replayed merge appends instead of overwriting");
  require(target.texts(1).text().base().self_ref() == "#/texts/1" &&
              target.texts(1).text().base().parent().ref() == "#/groups/1",
          "second copy renumbers into its own refs");
  require(grparse::docling_integrity_errors(target).empty(),
          "replayed merge stays well formed");
}

// Field regions and field items merge like every other arena: renumbered
// past the target's entries with their references rewritten, and the
// extension carriers ride along.
void verify_merge_carries_field_arenas_and_extensions() {
  docv1::Document target = base_document();
  auto* existing = target.add_field_regions();
  existing->set_self_ref("#/field_regions/0");
  existing->mutable_parent()->set_ref("#/body");
  target.mutable_body()->add_children()->set_ref("#/field_regions/0");

  docv1::Document source = base_document();
  auto* region = source.add_field_regions();
  region->set_self_ref("#/field_regions/0");
  region->mutable_parent()->set_ref("#/body");
  source.mutable_body()->add_children()->set_ref("#/field_regions/0");
  auto* field = source.add_field_items();
  field->set_self_ref("#/field_items/0");
  field->mutable_parent()->set_ref("#/field_regions/0");
  region->add_children()->set_ref("#/field_items/0");
  auto* attachment = source.add_attachments();
  attachment->set_id("part:7");
  attachment->set_media_type("application/pdf");
  source.mutable_source_meta()->set_title("Quarterly");

  grparse::merge_documents(std::move(source), &target);
  require(target.field_regions_size() == 2 && target.field_items_size() == 1,
          "field arenas append");
  require(target.field_regions(1).self_ref() == "#/field_regions/1",
          "moved field region renumbers past the existing one");
  require(target.field_regions(1).children(0).ref() == "#/field_items/0",
          "field item references follow the renumbering");
  require(target.field_items(0).parent().ref() == "#/field_regions/1",
          "field item parents follow the renumbering");
  require(target.attachments_size() == 1 && target.attachments(0).id() == "part:7",
          "attachment registry appends");
  require(target.source_meta().title() == "Quarterly",
          "first source metadata claim sticks");
}

// The service stamps the archive's own identity into the origin before any
// collector runs, so a collector's web provenance only survives if the
// origin merges field by field.
void verify_metadata_merges_beside_a_stamped_origin() {
  docv1::Document target = base_document();
  auto* stamped = target.mutable_origin();
  stamped->set_filename("capture.warc.gz");
  stamped->set_mimetype("application/warc");
  stamped->set_binary_hash(4096);

  docv1::Document crawl = collector_document();
  auto* web = crawl.mutable_origin()->mutable_web();
  web->set_target_uri("https://example.com/page");
  web->mutable_crawl_time()->set_seconds(1704164645);
  web->set_crawl_time_raw("2024-01-02T03:04:05Z");
  web->set_http_status(200);
  (*web->mutable_headers())["etag"] = "\"deadbeef\"";
  grparse::merge_documents(std::move(crawl), &target);

  docv1::Document page = collector_document();
  page.mutable_origin()->mutable_web()->set_canonical_uri(
      "https://example.com/canonical");
  (*page.mutable_origin()->mutable_web()->mutable_headers())["etag"] = "\"other\"";
  page.mutable_source_meta()->set_title("Example");
  page.mutable_source_meta()->set_language("en-GB");
  auto* tag = page.add_meta_tags();
  tag->set_name("description");
  tag->set_content("A page");
  grparse::merge_documents(std::move(page), &target);

  require(target.origin().filename() == "capture.warc.gz" &&
              target.origin().mimetype() == "application/warc" &&
              target.origin().binary_hash() == 4096,
          "the stamped archive identity is never overwritten");
  require(target.origin().web().target_uri() == "https://example.com/page" &&
              target.origin().web().crawl_time().seconds() == 1704164645 &&
              target.origin().web().crawl_time_raw() == "2024-01-02T03:04:05Z" &&
              target.origin().web().http_status() == 200,
          "the archive leg's web provenance survives the stamped origin");
  require(target.origin().web().canonical_uri() == "https://example.com/canonical",
          "a second leg fills the field the first left unset");
  require(target.origin().web().headers().at("etag") == "\"deadbeef\"",
          "a header the first leg answered is not overwritten by the second");
  require(target.source_meta().title() == "Example" &&
              target.source_meta().language() == "en-GB",
          "source-declared metadata reaches the merged document");
  require(target.meta_tags_size() == 1 &&
              target.meta_tags(0).name() == "description",
          "page-level meta pairs append");
}

// Sets every field of `message` to a value that is not its default, one
// element per list, one entry per map, recursing into messages to a
// bounded depth so self-referential shapes terminate.
void populate(google::protobuf::Message* message, int depth) {
  const auto* descriptor = message->GetDescriptor();
  const auto* reflection = message->GetReflection();
  using google::protobuf::FieldDescriptor;
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE && depth == 0) continue;
    // One arm per oneof is enough, and only the first is set.
    if (field->containing_oneof() != nullptr &&
        reflection->HasOneof(*message, field->containing_oneof())) {
      continue;
    }
    const auto set_scalar = [&](google::protobuf::Message* into, const FieldDescriptor* f,
                                bool repeated) {
      switch (f->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
          repeated ? into->GetReflection()->AddInt32(into, f, 7)
                   : into->GetReflection()->SetInt32(into, f, 7);
          break;
        case FieldDescriptor::CPPTYPE_INT64:
          repeated ? into->GetReflection()->AddInt64(into, f, 7)
                   : into->GetReflection()->SetInt64(into, f, 7);
          break;
        case FieldDescriptor::CPPTYPE_UINT32:
          repeated ? into->GetReflection()->AddUInt32(into, f, 7)
                   : into->GetReflection()->SetUInt32(into, f, 7);
          break;
        case FieldDescriptor::CPPTYPE_UINT64:
          repeated ? into->GetReflection()->AddUInt64(into, f, 7)
                   : into->GetReflection()->SetUInt64(into, f, 7);
          break;
        case FieldDescriptor::CPPTYPE_DOUBLE:
          repeated ? into->GetReflection()->AddDouble(into, f, 0.5)
                   : into->GetReflection()->SetDouble(into, f, 0.5);
          break;
        case FieldDescriptor::CPPTYPE_FLOAT:
          repeated ? into->GetReflection()->AddFloat(into, f, 0.5F)
                   : into->GetReflection()->SetFloat(into, f, 0.5F);
          break;
        case FieldDescriptor::CPPTYPE_BOOL:
          repeated ? into->GetReflection()->AddBool(into, f, true)
                   : into->GetReflection()->SetBool(into, f, true);
          break;
        case FieldDescriptor::CPPTYPE_ENUM: {
          const auto* value = f->enum_type()->value(f->enum_type()->value_count() > 1 ? 1 : 0);
          repeated ? into->GetReflection()->AddEnum(into, f, value)
                   : into->GetReflection()->SetEnum(into, f, value);
          break;
        }
        case FieldDescriptor::CPPTYPE_STRING:
          repeated ? into->GetReflection()->AddString(into, f, "x")
                   : into->GetReflection()->SetString(into, f, "x");
          break;
        case FieldDescriptor::CPPTYPE_MESSAGE:
          break;
      }
    };
    if (field->is_map()) {
      auto* entry = reflection->AddMessage(message, field);
      const auto* key = field->message_type()->map_key();
      const auto* value = field->message_type()->map_value();
      set_scalar(entry, key, false);
      if (value->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        populate(entry->GetReflection()->MutableMessage(entry, value), depth - 1);
      } else {
        set_scalar(entry, value, false);
      }
      continue;
    }
    if (field->is_repeated()) {
      if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        populate(reflection->AddMessage(message, field), depth - 1);
      } else {
        set_scalar(message, field, true);
      }
      continue;
    }
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      populate(reflection->MutableMessage(message, field), depth - 1);
    } else {
      set_scalar(message, field, false);
    }
  }
}

// Clears every reference string, because the merge renumbers them by
// design and this check is about everything else.
void strip_refs(google::protobuf::Message* message) {
  const auto* descriptor = message->GetDescriptor();
  const auto* reflection = message->GetReflection();
  using google::protobuf::FieldDescriptor;
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_STRING && !field->is_repeated() &&
        (field->name() == "ref" || field->name() == "self_ref")) {
      reflection->ClearField(message, field);
      continue;
    }
    if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) continue;
    if (field->is_repeated()) {
      const int size = reflection->FieldSize(*message, field);
      for (int entry = 0; entry < size; ++entry) {
        strip_refs(reflection->MutableRepeatedMessage(message, field, entry));
      }
    } else if (reflection->HasField(*message, field)) {
      strip_refs(reflection->MutableMessage(message, field));
    }
  }
}

// Every field of the model, filled, merged into nothing, comes out the
// other side. This is what keeps a field the schema grows from being
// dropped at the one seam every unary response passes through; a
// hand-maintained carry list was how six document-level carriers and most
// of DocumentMeta went missing.
void verify_every_field_survives_the_merge() {
  docv1::Document source;
  populate(&source, 4);
  docv1::Document expected = source;
  docv1::Document target;
  grparse::merge_documents(std::move(source), &target);
  strip_refs(&expected);
  strip_refs(&target);
  google::protobuf::util::MessageDifferencer differencer;
  std::string report;
  differencer.ReportDifferencesToString(&report);
  require(differencer.Compare(expected, target),
          "a populated document merged into an empty one loses fields:\n" + report);
}

// The target's answers are kept: a singular field the target holds, a map
// entry it already has, and the stamped root groups never change.
void verify_merge_never_overwrites_the_target() {
  docv1::Document target = base_document();
  target.mutable_source_meta()->set_title("first");
  (*target.mutable_source_meta()->mutable_extra())["k"] = "one";
  target.add_page_styles()->set_name("Default");
  docv1::Document source = base_document();
  source.mutable_source_meta()->set_title("second");
  source.mutable_source_meta()->set_subject("only here");
  (*source.mutable_source_meta()->mutable_extra())["k"] = "two";
  (*source.mutable_source_meta()->mutable_extra())["j"] = "new";
  source.add_page_styles()->set_name("Landscape");
  source.mutable_email()->set_message_id("<re-merge@x>");
  source.add_named_ranges()->set_name("R");
  source.add_anchors()->set_name("top");
  source.add_pivots()->set_name("P");
  source.add_changes()->set_author("editor");
  grparse::merge_documents(std::move(source), &target);
  require(target.source_meta().title() == "first", "the first claim stands");
  require(target.source_meta().subject() == "only here", "an unanswered field is filled");
  require(target.source_meta().extra().at("k") == "one" && target.source_meta().extra().at("j") == "new",
          "maps keep the target's entry and take new keys");
  require(target.page_styles_size() == 2 && target.page_styles(1).name() == "Landscape",
          "page styles append");
  require(target.email().message_id() == "<re-merge@x>" && target.named_ranges_size() == 1 &&
              target.anchors_size() == 1 && target.pivots_size() == 1 && target.changes_size() == 1,
          "the document-level carriers the old carry list missed arrive");
  require(target.body().self_ref() == "#/body" && target.furniture().self_ref() == "#/furniture",
          "root group names are the target's");
}

}  // namespace

int main() {
  try {
    verify_merge_renumbers_and_rewrites();
    verify_merge_is_additive_on_replay();
    verify_merge_carries_field_arenas_and_extensions();
    verify_metadata_merges_beside_a_stamped_origin();
    verify_every_field_survives_the_merge();
    verify_merge_never_overwrites_the_target();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "document-merge-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
