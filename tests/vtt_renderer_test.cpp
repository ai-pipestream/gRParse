// The WebVTT export, unit by unit: the header and its title, which items
// become cues, the timestamp field widths and rounding, voice spans,
// identifiers, and the rules that merge consecutive items into one cue.
// Whole-document parity for the same renderer lives in
// document_render_test.cpp; these cases pin the pieces that file does not
// reach.

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_vtt;
using grparse_test::add_collector_source;
using grparse_test::add_group;
using grparse_test::add_paragraph;
using grparse_test::add_text;
using grparse_test::base_document;
using grparse_test::require_equal;

namespace {

// Appends a text item carrying one TrackSource, the only thing that turns a
// body item into a cue.
void add_cue(docv1::Document* document, const std::string& parent, const std::string& text,
             double start, double end, const std::string& voice = "",
             const std::string& identifier = "") {
  add_paragraph(document, parent, text);
  auto* base = document->mutable_texts(document->texts_size() - 1)
                   ->mutable_text()
                   ->mutable_base();
  auto* track = base->add_source()->mutable_track();
  track->set_start_time(start);
  track->set_end_time(end);
  if (!voice.empty()) track->set_voice(voice);
  if (!identifier.empty()) track->set_identifier(identifier);
}

void verify_a_document_with_no_timed_items_is_the_bare_header() {
  require_equal(render_vtt(base_document("silence.wav")), "WEBVTT",
                "an empty document renders the bare header");

  docv1::Document untimed = base_document("prose.txt");
  add_paragraph(&untimed, "#/body", "no timing at all");
  require_equal(render_vtt(untimed), "WEBVTT",
                "a body item with no track source produces no cue");
}

void verify_the_title_item_names_the_header() {
  docv1::Document document = base_document("meeting.wav");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
           "  Weekly sync  ");
  require_equal(render_vtt(document), "WEBVTT Weekly sync",
                "the title item's trimmed text rides the header line");

  docv1::Document labelled = base_document("meeting.wav");
  add_text(&labelled, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TITLE,
           "Labelled");
  require_equal(render_vtt(labelled), "WEBVTT Labelled",
                "a plain text item labelled as the title names the header too");

  docv1::Document empty_title = base_document("meeting.wav");
  add_text(&empty_title, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE, "");
  require_equal(render_vtt(empty_title), "WEBVTT",
                "an empty title leaves the header bare");
}

void verify_timestamps_pad_their_fields_and_round_to_milliseconds() {
  docv1::Document document = base_document("clock.wav");
  add_cue(&document, "#/body", "start", 0.0, 0.001);
  add_cue(&document, "#/body", "rounded", 1.0004, 1.9996);
  add_cue(&document, "#/body", "hours", 3661.0, 3723.25);

  require_equal(render_vtt(document),
                "WEBVTT\n"
                "\n"
                "00:00:00.000 --> 00:00:00.001\n"
                "start\n"
                "\n"
                "00:00:01.000 --> 00:00:02.000\n"
                "rounded\n"
                "\n"
                "01:01:01.000 --> 01:02:03.250\n"
                "hours",
                "every field is two digits and the fractional part rounds into milliseconds");
}

void verify_a_negative_start_reads_as_zero() {
  docv1::Document document = base_document("clock.wav");
  add_cue(&document, "#/body", "before the start", -5.0, 1.0);
  require_equal(render_vtt(document),
                "WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nbefore the start",
                "a negative timestamp floors at zero rather than printing a sign");
}

void verify_a_voice_wraps_the_cue_text() {
  docv1::Document document = base_document("meeting.wav");
  add_cue(&document, "#/body", "Hello", 1.0, 2.0, "Alice");
  require_equal(render_vtt(document),
                "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n<v Alice>Hello</v>",
                "a named voice wraps the cue payload in its span");
}

void verify_an_identifier_takes_the_line_above_the_timing() {
  docv1::Document document = base_document("meeting.wav");
  add_cue(&document, "#/body", "Hello", 1.0, 2.0, "", "cue-1");
  require_equal(render_vtt(document),
                "WEBVTT\n\ncue-1\n00:00:01.000 --> 00:00:02.000\nHello",
                "an identifier stands on its own line before the timing");
}

void verify_consecutive_items_with_the_same_cue_merge() {
  docv1::Document document = base_document("meeting.wav");
  add_cue(&document, "#/body", "line one", 10.0, 12.0, "", "cue-3");
  add_cue(&document, "#/body", "line two", 10.0, 12.0, "", "cue-3");
  require_equal(render_vtt(document),
                "WEBVTT\n\ncue-3\n00:00:10.000 --> 00:00:12.000\nline one\nline two",
                "two items sharing an identifier and a timing become one multi-line cue");
}

void verify_a_different_timing_or_identifier_opens_a_new_cue() {
  docv1::Document timing = base_document("meeting.wav");
  add_cue(&timing, "#/body", "one", 10.0, 12.0, "", "cue-3");
  add_cue(&timing, "#/body", "two", 10.0, 12.5, "", "cue-3");
  require_equal(render_vtt(timing),
                "WEBVTT\n"
                "\n"
                "cue-3\n00:00:10.000 --> 00:00:12.000\none\n"
                "\n"
                "cue-3\n00:00:10.000 --> 00:00:12.500\ntwo",
                "the same identifier with a different timing is a second cue");

  docv1::Document identifier = base_document("meeting.wav");
  add_cue(&identifier, "#/body", "one", 10.0, 12.0, "", "a");
  add_cue(&identifier, "#/body", "two", 10.0, 12.0, "", "b");
  require_equal(render_vtt(identifier),
                "WEBVTT\n"
                "\n"
                "a\n00:00:10.000 --> 00:00:12.000\none\n"
                "\n"
                "b\n00:00:10.000 --> 00:00:12.000\ntwo",
                "the same timing under a different identifier is a second cue");
}

void verify_an_untimed_item_between_two_cues_does_not_merge_them() {
  docv1::Document document = base_document("meeting.wav");
  add_cue(&document, "#/body", "one", 1.0, 2.0);
  add_paragraph(&document, "#/body", "not a cue");
  add_cue(&document, "#/body", "two", 1.0, 2.0);
  require_equal(render_vtt(document),
                "WEBVTT\n"
                "\n"
                "00:00:01.000 --> 00:00:02.000\none\n"
                "two",
                "an item with no track source is skipped, so the cues around it still merge");
}

void verify_attribution_sources_do_not_hide_the_track() {
  docv1::Document document = base_document("meeting.wav");
  add_cue(&document, "#/body", "spoken", 1.0, 2.0);
  auto* base = document.mutable_texts(0)->mutable_text()->mutable_base();
  const docv1::TrackSource track = base->source(0).track();
  base->clear_source();
  add_collector_source(base->mutable_source(), "asr");
  *base->add_source()->mutable_track() = track;

  require_equal(render_vtt(document),
                "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nspoken",
                "the scan looks past collector attribution for the track source");
}

void verify_cues_inside_a_group_are_reached() {
  docv1::Document document = base_document("meeting.wav");
  const std::string chapter = add_group(&document, "#/body", docv1::GROUP_LABEL_CHAPTER);
  add_cue(&document, chapter, "inside", 1.0, 2.0);
  require_equal(render_vtt(document),
                "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\ninside",
                "the walk descends into groups to find timed items");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("vtt-renderer-test", "ok", {
      verify_a_document_with_no_timed_items_is_the_bare_header,
      verify_the_title_item_names_the_header,
      verify_timestamps_pad_their_fields_and_round_to_milliseconds,
      verify_a_negative_start_reads_as_zero,
      verify_a_voice_wraps_the_cue_text,
      verify_an_identifier_takes_the_line_above_the_timing,
      verify_consecutive_items_with_the_same_cue_merge,
      verify_a_different_timing_or_identifier_opens_a_new_cue,
      verify_an_untimed_item_between_two_cues_does_not_merge_them,
      verify_attribution_sources_do_not_hide_the_track,
      verify_cues_inside_a_group_are_reached,
  });
}
