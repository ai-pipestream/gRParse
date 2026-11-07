// Fixtures and fakes for grparse-bench (#251). The streaming fakes are
// trimmed copies of the ones tests/streaming_service_test.cpp keeps in its
// anonymous namespace (FakeRecognizer, SlicingPdfInspector,
// PdfInspectorServer), sized for a 200-page document and without the gates
// those tests use to prove ordering. Header only, and test-only: nothing
// under src/ or include/ may include it.
#ifndef GRPARSE_TESTS_BENCH_BENCH_SUPPORT_H
#define GRPARSE_TESTS_BENCH_BENCH_SUPPORT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/pdf/v1/pdf_service.grpc.pb.h"
#include "grparse/ocr_engine.h"
#include "grparse/ocr_types.h"

namespace grparse_bench {

namespace docv1 = ai::pipestream::document::v1;
namespace pdfv1 = ai::pipestream::pdf::v1;

inline constexpr int kPages = 200;
inline constexpr int kLinesPerPage = 60;
// Items per page the fake inspector folds: one heading and this many
// paragraphs.
inline constexpr int kParagraphsPerPage = 20;

// The recognizer the fast path must never reach; it counts, so the bench
// can refuse to report a number for the wrong path.
class IdleRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    calls.fetch_add(1);
    return {};
  }
  std::atomic<int> calls{0};
};

inline std::string prose(int page, int index) {
  return "Paragraph " + std::to_string(index) + " of page " + std::to_string(page) +
         ": the committee reviewed the quarterly figures and recorded steady growth "
         "across every region it oversees.";
}

// Serves the inspector's page-document contract for a text PDF of kPages
// pages: the classification, then per page its markdown and its slice of
// the fold (a heading and kParagraphsPerPage paragraphs with boxes), then
// the whole fold and the status trailer. What SlicingPdfInspector serves
// for three pages, without the read gate.
class PagedPdfInspector final : public pdfv1::PdfParseService::Service {
 public:
  grpc::Status ParsePdf(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<pdfv1::ParsePdfResponse, pdfv1::ParsePdfRequest>* stream)
      override {
    pdfv1::ParsePdfRequest request;
    bool page_documents = false;
    while (stream->Read(&request)) {
      if (request.has_options()) page_documents = request.options().emit_page_documents();
    }
    pdfv1::ParsePdfResponse event;
    event.mutable_info()->set_pdf_type(pdfv1::PDF_TYPE_TEXT_BASED);
    event.mutable_info()->set_page_count(kPages);
    stream->Write(event);
    docv1::Document whole;
    whole.mutable_body()->set_self_ref("#/body");
    whole.mutable_furniture()->set_self_ref("#/furniture");
    int next_text = 0;
    for (int page_no = 1; page_no <= kPages; ++page_no) {
      docv1::Document slice;
      slice.mutable_body()->set_self_ref("#/body");
      slice.mutable_furniture()->set_self_ref("#/furniture");
      std::string markdown;
      for (int index = 0; index <= kParagraphsPerPage; ++index) {
        const bool heading = index == 0;
        const std::string ref = "#/texts/" + std::to_string(next_text++);
        auto* item = slice.add_texts();
        docv1::TextItemBase* base = heading ? item->mutable_section_header()->mutable_base()
                                            : item->mutable_text()->mutable_base();
        const std::string text =
            heading ? "Section " + std::to_string(page_no) : prose(page_no, index);
        base->set_self_ref(ref);
        base->mutable_parent()->set_ref("#/body");
        base->set_label(heading ? docv1::DOC_ITEM_LABEL_SECTION_HEADER
                                : docv1::DOC_ITEM_LABEL_TEXT);
        base->set_text(text);
        base->add_source()->mutable_collector()->set_collector("pdf");
        auto* prov = base->add_prov();
        prov->set_page_no(page_no);
        const double top = 760.0 - 34.0 * index;
        prov->mutable_bbox()->set_l(72.0);
        prov->mutable_bbox()->set_t(top);
        prov->mutable_bbox()->set_r(540.0);
        prov->mutable_bbox()->set_b(top - 24.0);
        prov->mutable_bbox()->set_coord_origin(docv1::COORD_ORIGIN_BOTTOMLEFT);
        slice.mutable_body()->add_children()->set_ref(ref);
        *whole.add_texts() = *item;
        whole.mutable_body()->add_children()->set_ref(ref);
        markdown += (heading ? "## " : "") + text + "\n\n";
      }
      auto& page = (*slice.mutable_pages())[page_no];
      page.set_page_no(page_no);
      page.mutable_size()->set_width(612);
      page.mutable_size()->set_height(792);
      (*whole.mutable_pages())[page_no] = page;
      event.Clear();
      event.mutable_page()->set_page_no(static_cast<uint32_t>(page_no));
      event.mutable_page()->set_markdown(markdown);
      stream->Write(event);
      if (page_documents) {
        event.Clear();
        event.mutable_page_document()->set_page_no(static_cast<uint32_t>(page_no));
        *event.mutable_page_document()->mutable_document() = std::move(slice);
        stream->Write(event);
      }
    }
    event.Clear();
    *event.mutable_document() = std::move(whole);
    stream->Write(event);
    event.Clear();
    event.mutable_status()->set_pages_extracted(kPages);
    stream->Write(event);
    return grpc::Status::OK;
  }
};

// Serves one inspector on a loopback port.
class InspectorServer {
 public:
  explicit InspectorServer(pdfv1::PdfParseService::Service* service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (!server_ || port_ == 0) throw std::runtime_error("fake pdf inspector failed to start");
  }
  ~InspectorServer() {
    if (server_) server_->Shutdown();
  }
  InspectorServer(const InspectorServer&) = delete;
  InspectorServer& operator=(const InspectorServer&) = delete;
  std::string target() const { return "127.0.0.1:" + std::to_string(port_); }

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

// One recognized page the way the CV path hands it to assembly: a
// 200 dpi Letter raster's coordinates, kLinesPerPage lines of 30 px with
// layout regions over them (a section header, four paragraphs, a five-item
// list), a running header that repeats on every page and a page number
// footer, both outside every region so the repair pass has work.
inline grparse::OcrPage synthetic_ocr_page(int page_number) {
  constexpr int kWidth = 1700;
  constexpr int kHeight = 2200;
  constexpr int kTop = 100;
  constexpr int kPitch = 34;
  constexpr int kLineHeight = 30;
  grparse::OcrPage page;
  page.width = kWidth;
  page.height = kHeight;
  page.layout_model = "bench-layout";
  const auto line_top = [](int line) { return kTop + kPitch * line; };
  for (int line = 0; line < kLinesPerPage; ++line) {
    std::string text;
    if (line == 0) {
      text = "Annual Report of the Regional Committee";
    } else if (line == 1) {
      text = "Section " + std::to_string(page_number);
    } else if (line >= 20 && line < 25) {
      text = "- point " + std::to_string(line - 19) + " of the agenda on page " +
             std::to_string(page_number);
    } else if (line == kLinesPerPage - 1) {
      text = std::to_string(page_number);
    } else {
      text = "Line " + std::to_string(line) + " of page " + std::to_string(page_number) +
             ": the committee reviewed the quarterly figures and noted growth";
    }
    const int top = line_top(line);
    page.lines.push_back(grparse::OcrLine{
        std::move(text),
        {{100, top}, {1600, top}, {1600, top + kLineHeight}, {100, top + kLineHeight}},
        0.93F,
        grparse::TextOrigin::kOcr});
  }
  const auto region = [&](std::string label, int first, int last) {
    return grparse::LayoutRegion{std::move(label), 0.9F, 90, line_top(first) - 2, 1610,
                                 line_top(last) + kLineHeight + 2};
  };
  page.regions.push_back(region("section_header", 1, 1));
  page.regions.push_back(region("text", 2, 10));
  page.regions.push_back(region("text", 11, 19));
  for (int line = 20; line < 25; ++line) page.regions.push_back(region("list_item", line, line));
  page.regions.push_back(region("text", 25, 40));
  page.regions.push_back(region("text", 41, 57));
  return page;
}

}  // namespace grparse_bench

#endif  // GRPARSE_TESTS_BENCH_BENCH_SUPPORT_H
