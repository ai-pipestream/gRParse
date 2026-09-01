#include "grparse/office_fold/run_text.h"

#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// Every character attribute of a run. Adjacent runs agreeing on all of them
// are one inline span; a portion boundary the reader cannot see is not a
// formatting boundary.
struct RunKey {
  std::string font;
  float size_pt = 0;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strikethrough = false;
  bool monospace = false;
  bool small_caps = false;
  bool overline = false;
  std::string char_style;
  int32_t highlight_rgb = -1;
  uint32_t color_rgb = 0;
  int escapement = 0;
  std::string language;
  std::string hyperlink;
  std::string field_code;
  std::string field_target;

  bool operator==(const RunKey& other) const = default;
};

RunKey run_key(const officev1::TextRun& run) {
  RunKey key;
  key.font = run.font();
  key.size_pt = run.size_pt();
  key.bold = run.weight() >= 150.0f;
  key.italic = run.italic();
  key.underline = run.underline();
  key.strikethrough = run.strikethrough();
  key.monospace = run.monospace();
  key.small_caps = run.small_caps();
  key.overline = run.overline();
  key.char_style = run.char_style();
  key.highlight_rgb = run.highlight_rgb();
  key.color_rgb = run.color_rgb();
  key.escapement = run.escapement();
  key.language = run.language();
  key.hyperlink = run.hyperlink_url();
  key.field_code = run.field_code();
  key.field_target = run.field_target();
  return key;
}

// The attributes of one coalesced run, once it is known to be worth a span.
void describe_span(const RunKey& key, const std::string& highlight,
                   const std::string& color, bool formatted,
                   bool language_differs, docv1::InlineSpan* span) {
  if (formatted) {
    docv1::Formatting* formatting = span->mutable_formatting();
    formatting->set_bold(key.bold);
    formatting->set_italic(key.italic);
    formatting->set_underline(key.underline);
    formatting->set_strikethrough(key.strikethrough);
    formatting->set_monospace(key.monospace);
    formatting->set_small_caps(key.small_caps);
    formatting->set_overline(key.overline);
    formatting->set_script(script_for(key.escapement));
  }
  if (!key.char_style.empty()) span->set_style_name(key.char_style);
  if (!highlight.empty()) span->set_highlight_color(highlight);
  if (!key.font.empty()) span->set_font_family(key.font);
  if (key.size_pt > 0) span->set_font_size_pt(key.size_pt);
  if (!color.empty()) span->set_color(color);
  if (language_differs) span->set_language(key.language);
  if (!key.hyperlink.empty()) span->set_hyperlink(key.hyperlink);
  if (!key.field_code.empty()) span->set_field_code(key.field_code);
}

}  // namespace

std::string concat_runs(const TextRuns& runs) {
  std::string text;
  for (const officev1::TextRun& run : runs) text += run.text();
  return text;
}

long long runs_length(const TextRuns& runs) {
  long long total = 0;
  for (const officev1::TextRun& run : runs) total += run.char_length();
  return total;
}

void set_uniform_formatting(const TextRuns& runs, docv1::TextItemBase* base) {
  if (runs.empty()) return;
  bool bold = runs[0].weight() >= 150.0f;
  docv1::Script script = script_for(runs[0].escapement());
  for (const officev1::TextRun& run : runs) {
    if ((run.weight() >= 150.0f) != bold || run.italic() != runs[0].italic()
        || run.underline() != runs[0].underline()
        || run.strikethrough() != runs[0].strikethrough()
        || run.monospace() != runs[0].monospace()
        || run.small_caps() != runs[0].small_caps()
        || run.overline() != runs[0].overline()
        || script_for(run.escapement()) != script) {
      return;
    }
  }
  if (!bold && !runs[0].italic() && !runs[0].underline()
      && !runs[0].strikethrough() && !runs[0].monospace()
      && !runs[0].small_caps() && !runs[0].overline()
      && script == docv1::SCRIPT_UNSPECIFIED) {
    return;
  }
  docv1::Formatting* formatting = base->mutable_formatting();
  formatting->set_bold(bold);
  formatting->set_italic(runs[0].italic());
  formatting->set_underline(runs[0].underline());
  formatting->set_strikethrough(runs[0].strikethrough());
  formatting->set_monospace(runs[0].monospace());
  formatting->set_small_caps(runs[0].small_caps());
  formatting->set_overline(runs[0].overline());
  formatting->set_script(script);
}

void apply_run_hyperlinks(const TextRuns& runs, docv1::TextItemBase* base) {
  for (const officev1::TextRun& run : runs) {
    if (run.hyperlink_url().empty()) continue;
    base->set_hyperlink(run.hyperlink_url());
    return;
  }
}

void add_run_spans(const TextRuns& runs, InlineSpans* spans,
                   const std::string& language, const std::string& owner_ref,
                   long long base_offset, AnchorIndex* anchors) {
  long long local = base_offset;
  for (int index = 0; index < runs.size();) {
    const RunKey key = run_key(runs[index]);
    long long start = local;
    int end_index = index;
    // Adjacent runs agreeing on every attribute are one span: the office
    // core splits portions for reasons a reader never sees.
    while (end_index < runs.size() && run_key(runs[end_index]) == key) {
      local += runs[end_index].char_length();
      end_index++;
    }
    index = end_index;
    if (local <= start) continue;
    const std::string color = hex_color(key.color_rgb);
    const bool language_differs =
        !key.language.empty() && key.language != language;
    const bool formatted = key.bold || key.italic || key.underline
        || key.strikethrough || key.monospace || key.small_caps || key.overline
        || script_for(key.escapement) != docv1::SCRIPT_UNSPECIFIED;
    // -1 is the office core's transparent value: a run with no highlight.
    const std::string highlight = key.highlight_rgb >= 0
        ? hex_color_always(static_cast<uint32_t>(key.highlight_rgb))
        : std::string();
    if (!formatted && key.font.empty() && key.size_pt <= 0 && color.empty()
        && !language_differs && key.hyperlink.empty()
        && key.field_code.empty() && key.char_style.empty()
        && highlight.empty()) {
      // Nothing the item does not already say; a span here would be noise.
      continue;
    }
    docv1::InlineSpan* span = spans->Add();
    span->mutable_range()->set_start(clamp32(start));
    span->mutable_range()->set_end(clamp32(local));
    describe_span(key, highlight, color, formatted, language_differs, span);
    if (!key.field_target.empty() && !owner_ref.empty() && anchors != nullptr) {
      // The anchor the reference names may not have streamed yet, so the
      // target is filled in once the whole document has.
      anchors->add_reference(owner_ref, spans->size() - 1, key.field_target);
    }
  }
}

}  // namespace grparse::office_fold
