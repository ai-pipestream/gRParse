// S3 eval finding: a .csv's origin said text/plain from the bytes, because
// the text ladder's fallback outranked the name. "text/plain" is what the
// sniff says when it recognises nothing in particular; a name that names a
// specific text format knows more. Every other precedence stays as it was:
// a specific sniff beats the name, and a binary signature beats everything.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse/content_sniff.h"
#include "support/check.h"

namespace {

void require_resolution(const std::string& declared, const std::string& bytes, const std::string& name,
                        const std::string& mimetype, const std::string& evidence, const std::string& what) {
  const grparse::MimetypeResolution got = grparse::resolve_mimetype(declared, bytes, name);
  if (got.mimetype != mimetype || got.evidence != evidence) {
    throw std::runtime_error(what + ": expected " + mimetype + " (" + evidence + "), got " + got.mimetype +
                             " (" + got.evidence + ")");
  }
}

void verify_generic_text_yields_to_a_specific_name() {
  require_resolution("", "name,qty\nbolt,4\n", "parts.csv", "text/csv", "extension",
                     "a csv that sniffs as plain text is text/csv by its name");
  require_resolution("", "Just prose without markers.\n", "notes.md", "text/markdown", "extension",
                     "a markdown file without markers is text/markdown by its name");
  require_resolution("", "Just prose.\n", "notes.txt", "text/plain", "magic",
                     "plain text under a plain-text name rests on the bytes");
  require_resolution("", "Just prose.\n", "blob", "text/plain", "magic",
                     "plain text under a bare name rests on the bytes");
}

void verify_specific_sniffs_still_outrank_the_name() {
  require_resolution("", "# Heading\n\ntext\n", "notes.txt", "text/markdown", "magic",
                     "markdown markers in a .txt are the bytes' say");
  require_resolution("", "<!doctype html><html></html>", "page.txt", "text/html", "magic",
                     "html in a .txt is the bytes' say");
  require_resolution("", "%PDF-1.4\n", "report.csv", "application/pdf", "magic",
                     "a binary signature beats any name");
  require_resolution("text/csv; charset=utf-8", "anything", "x.bin", "text/csv", "declared",
                     "a declared type wins outright");
}

// The iWork rule mirrors the text one: "application/zip" is what the
// container scan says when it places nothing, and a .pages / .numbers name
// knows which app's document the archive is. A specific zip sniff (a docx)
// still outranks an iWork name, and a non-Apple name does not lift a plain
// zip.
void verify_a_plain_zip_yields_to_an_iwork_name() {
  const std::string plain_zip = std::string("PK\x03\x04") + std::string(26, '\0') + "Index/Document.iwa";
  require_resolution("", plain_zip, "report.pages", "application/vnd.apple.pages", "extension",
                     "a zip the scan cannot place is Pages by its name");
  require_resolution("", plain_zip, "budget.numbers", "application/vnd.apple.numbers", "extension",
                     "a zip the scan cannot place is Numbers by its name");
  require_resolution("", plain_zip, "bundle.zip", "application/zip", "magic",
                     "a plain zip under a zip name stays a zip");
  const std::string docx = std::string("PK\x03\x04") + std::string(26, '\0') + "word/document.xml";
  require_resolution("", docx, "misnamed.pages",
                     "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "magic",
                     "a docx sniff outranks an iWork name");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("content_sniff_precedence_test", "ok", {
      verify_generic_text_yields_to_a_specific_name,
      verify_specific_sniffs_still_outrank_the_name,
      verify_a_plain_zip_yields_to_an_iwork_name,
  });
}
