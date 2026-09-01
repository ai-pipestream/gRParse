#include "grparse/document_collectors.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/epub_book.h"

namespace epubv1 = ai::pipestream::epub::v1;

namespace grparse {

namespace {

// The epub stream with its typed events kept: the skeleton Document as the
// outcome, plus every chapter and image resource in arrival order.
struct EpubStream {
  CollectorOutcome outcome;
  std::vector<EpubChapter> chapters;
  std::vector<EpubResource> images;
};

EpubStream read_epub_stream(const std::shared_ptr<grpc::Channel>& channel,
                            const std::string& bytes, CollectorDeadline inbound_deadline) {
  EpubStream result;
  auto stub = epubv1::EpubParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseEpub(&context);

  epubv1::ParseEpubRequest request;
  request.mutable_options()->set_emit_document(true);
  request.mutable_options()->set_include_images(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](epubv1::ParseEpubRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  bool trailer_seen = false;
  bool document_seen = false;
  epubv1::ParseEpubResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      result.outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_chapter()) {
      auto* chapter = event.mutable_chapter();
      result.chapters.push_back(EpubChapter{chapter->href(), chapter->media_type(),
                                            std::move(*chapter->mutable_content())});
    } else if (event.has_resource()) {
      auto* resource = event.mutable_resource();
      if (resource->kind() == epubv1::RESOURCE_KIND_IMAGE && !resource->content().empty()) {
        result.images.push_back(EpubResource{resource->href(), resource->media_type(),
                                             std::move(*resource->mutable_content())});
      }
    } else if (event.has_status()) {
      for (const auto& warning : event.status().warnings()) {
        std::string text =
            epubv1::ParseWarningCode_Name(warning.code()) + ": " + warning.message();
        if (!warning.href().empty()) text += " (" + warning.href() + ")";
        result.outcome.warnings.push_back(std::move(text));
      }
      trailer_seen = true;
    }
    event.Clear();
  }
  result.outcome = finish_outcome("epub", stream->Finish(), trailer_seen, document_seen,
                                  std::move(result.outcome));
  return result;
}

std::string lowercase(std::string value) {
  for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// Whether a spine item is something the markup collector reads as HTML.
// Spine items are XHTML by the EPUB specification; an SVG-in-spine item is
// a picture, not a text chapter, and is left to its group.
bool html_chapter(const EpubChapter& chapter) {
  const std::string type = lowercase(chapter.media_type);
  if (type == "application/xhtml+xml" || type == "text/html") return true;
  const std::string href = lowercase(chapter.href);
  return href.ends_with(".xhtml") || href.ends_with(".html") || href.ends_with(".htm");
}

}  // namespace

CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes,
                                       CollectorDeadline inbound_deadline) {
  return std::move(read_epub_stream(channel, bytes, inbound_deadline).outcome);
}

CollectorOutcome collect_epub_book(const std::shared_ptr<grpc::Channel>& epub,
                                   const std::shared_ptr<grpc::Channel>& markup,
                                   const std::string& bytes,
                                   CollectorDeadline inbound_deadline) {
  EpubStream stream = read_epub_stream(epub, bytes, inbound_deadline);
  CollectorOutcome& outcome = stream.outcome;
  if (!outcome.success) return std::move(outcome);
  if (markup == nullptr) {
    outcome.warnings.push_back(
        "chapters were not folded: the markup collector is not configured "
        "(GRPARSE_MARKUP_TARGET), so the chapter groups stay empty");
    return std::move(outcome);
  }

  std::vector<ParsedChapter> chapters;
  chapters.reserve(stream.chapters.size());
  for (auto& chapter : stream.chapters) {
    if (!html_chapter(chapter)) {
      outcome.warnings.push_back("chapter '" + chapter.href + "' (" + chapter.media_type +
                                 ") is not XHTML; its group stays empty");
      continue;
    }
    CollectorOutcome parsed = collect_markup_document(
        markup, chapter.href, "application/xhtml+xml", chapter.content, inbound_deadline);
    for (auto& warning : parsed.warnings) {
      outcome.warnings.push_back(chapter.href + ": " + warning);
    }
    if (!parsed.success) {
      outcome.warnings.push_back("chapter '" + chapter.href +
                                 "' could not be parsed by the markup collector: " +
                                 parsed.error + "; its group stays empty");
      continue;
    }
    chapters.push_back(ParsedChapter{chapter.href, std::move(parsed.document)});
  }
  fold_epub_book(std::move(chapters), stream.images, &outcome.document, &outcome.warnings);
  return std::move(outcome);
}
}  // namespace grparse
