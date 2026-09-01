#include <cstdlib>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/page_previews.h"
#include "support/check.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;

using grparse_test::require;

// A complete two-page PDF with a base-14 font, so nothing depends on the
// host's fonts.
std::string two_page_pdf() {
  const std::string content = "BT /F1 24 Tf 72 700 Td (Hello) Tj ET\n";
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> "
      ">> /Contents 5 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> "
      ">> /Contents 5 0 R >>",
  };
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  for (const size_t offset : offsets) {
    std::string entry = std::to_string(offset);
    entry.insert(entry.begin(), 10 - entry.size(), '0');
    pdf += entry + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

// Every page gets a PNG preview sized within the CV path's bound, keyed by
// page number; a page the document already described keeps what it said.
void verify_previews_attach_to_every_page() {
  docv1::Document document;
  auto& first = (*document.mutable_pages())[1];
  first.set_page_no(1);
  first.mutable_size()->set_width(612);
  first.mutable_size()->set_height(792);
  first.set_unit("pt");
  grparse::attach_page_previews(std::make_shared<const std::string>(two_page_pdf()), &document);
  require(document.pages_size() == 2, "both pages of the PDF carry a preview entry");
  for (const int page_no : {1, 2}) {
    const auto& page = document.pages().at(page_no);
    require(page.page_no() == page_no, "the entry names its page");
    require(page.has_image() && page.image().mimetype() == "image/png" &&
                page.image().uri().starts_with("data:image/png;base64,"),
            "the preview is an embedded PNG");
    const int longest = std::max(page.image().size().width(), page.image().size().height());
    require(longest > 0 && longest <= grparse::kPagePreviewMaxSide,
            "the preview is bounded like the CV path's: " + std::to_string(longest));
    const double ratio = page.image().size().width() / page.image().size().height();
    require(ratio > 0.75 && ratio < 0.79, "the preview keeps the page's aspect ratio");
  }
  require(document.pages().at(1).unit() == "pt" && document.pages().at(1).size().width() == 612,
          "the collector's own page description survives");
}

// Bytes that are not a PDF leave the document exactly as it was.
void verify_unopenable_bytes_change_nothing() {
  docv1::Document document;
  (*document.mutable_pages())[1].set_page_no(1);
  const std::string before = document.SerializeAsString();
  grparse::attach_page_previews(std::make_shared<const std::string>("not a pdf"), &document);
  require(document.SerializeAsString() == before, "an unopenable source is not a failure");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("page-previews-test", {
      verify_previews_attach_to_every_page,
      verify_unopenable_bytes_change_nothing,
  });
}
