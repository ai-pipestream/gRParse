// Fuzzes the routing door every upload passes before a collector is chosen:
// resolve_mimetype (the declared type, the byte sniff, the extension map),
// sniff_mimetype (zip central-directory and mimetype-entry peeks, the
// binary signatures, the compound-file placement, the text, markup, mail,
// Markdown and CSV ladders), encrypted_office_document, and input_format_for
// over each resolved type.
//
// The whole input is the body, so a seed file is fuzzed as the bytes it is.
// The name rides beside it, picked from the extensions the two maps know by
// the input's last byte, and the first line of the input is also tried as a
// declared content type, so the precedence rules see every combination.
//
// The contract under test: any request ends in a resolution that names a
// type and its evidence, never a crash or sanitizer finding, and the sniff
// holds heap linear in the body.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

#include "grparse/content_sniff.h"
#include "grparse/input_format.h"
#include "heap_budget.h"

namespace {

// Names whose extension changes the resolution: the precedence exceptions
// (text names over a text/plain or CSV sniff, iWork names over a bare zip,
// OOXML names over an encrypted package, the storage suffix over the
// bytes), names only the extension routes, and names with nothing to say.
constexpr std::string_view kNames[] = {
    "upload",        "upload.bin",     "page.storage.xhtml", "page.confluence",
    "page.xhtml",    "doc.docx",       "book.xlsx",          "deck.pptx",
    "doc.DOCM",      "doc.doc",        "book.xls",           "deck.ppt",
    "doc.pages",     "sheet.numbers",  "talk.key",           "notes.txt",
    "notes.md",      "data.csv",       "subs.vtt",           "paper.tex",
    "page.html",     "feed.xml",       "article.nxml",       "report.xbrl",
    "mail.eml",      "mail.msg",       "archive.mht",        "book.epub",
    "crawl.warc",    "crawl.warc.gz",  "scan.pdf",           "photo.PNG",
    "doc.dclx",      "note.boxnote",   "guide.adoc",         "print.afp",
    "clip.mp3",      "data.json",      ".hidden",            "dir/",
};

void resolve(std::string_view declared, std::string_view body,
             const std::filesystem::path& name) {
  const grparse::MimetypeResolution resolution = grparse::resolve_mimetype(declared, body, name);
  if (resolution.mimetype.empty() || resolution.evidence.empty()) {
    __builtin_trap();  // every resolution names a type and what it rests on
  }
  (void)grparse::input_format_for(resolution.mimetype, name);
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  grparse_fuzz::install_heap_budget();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string body(reinterpret_cast<const char*>(data), size);
  const std::filesystem::path name(
      kNames[size == 0 ? 0 : data[size - 1] % std::size(kNames)]);
  const std::string_view first_line =
      std::string_view(body).substr(0, std::min(body.find('\n'), size_t{128}));

  const grparse_fuzz::HeapBudget budget(size, 16, size_t{4} << 20);
  resolve("", body, "upload");
  resolve("", body, name);
  resolve(first_line, body, name);
  (void)grparse::input_format_for(first_line, name);
  (void)grparse::sniff_mimetype(body);
  (void)grparse::encrypted_office_document(body);
  return 0;
}
