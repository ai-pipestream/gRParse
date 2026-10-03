#include "grparse/document_collectors.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/epub_book.h"

namespace docv1 = ai::pipestream::document::v1;
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
                            const std::string& bytes, CollectorDeadline inbound_deadline,
                            CollectorCancelled cancelled, size_t byte_cap) {
  EpubStream result;
  auto stub = epubv1::EpubParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->ParseEpub(&context);

  epubv1::ParseEpubRequest request;
  request.mutable_options()->set_emit_document(true);
  request.mutable_options()->set_include_images(true);
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](epubv1::ParseEpubRequest& frame, size_t offset, size_t length, bool /*last*/) {
        frame.set_chunk(bytes.data() + offset, length);
      });

  bool trailer_seen = false;
  bool document_seen = false;
  // What the kept chapters and images hold together; past the cap the call
  // is cancelled and nothing more is kept.
  size_t buffered = 0;
  bool over_cap = false;
  const auto keep = [&](size_t size) {
    if (over_cap) return false;
    buffered += size;
    if (buffered <= byte_cap) return true;
    over_cap = true;
    context.TryCancel();
    return false;
  };
  epubv1::ParseEpubResponse event;
  while (stream->Read(&event)) {
    if (event.has_document()) {
      result.outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (event.has_chapter()) {
      auto* chapter = event.mutable_chapter();
      if (keep(chapter->content().size())) {
        result.chapters.push_back(EpubChapter{chapter->href(), chapter->media_type(),
                                              std::move(*chapter->mutable_content())});
      }
    } else if (event.has_resource()) {
      auto* resource = event.mutable_resource();
      if (resource->kind() == epubv1::RESOURCE_KIND_IMAGE && !resource->content().empty() &&
          keep(resource->content().size())) {
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
  upload.join();
  const grpc::Status status = stream->Finish();
  if (over_cap) {
    result.outcome.success = false;
    result.outcome.code = grpc::StatusCode::RESOURCE_EXHAUSTED;
    result.outcome.error = "epub collector: the book's chapters and images exceed " +
                           std::to_string(byte_cap) + " bytes once decompressed";
    result.chapters.clear();
    result.images.clear();
    return result;
  }
  result.outcome = finish_outcome("epub", status, trailer_seen, document_seen,
                                  std::move(result.outcome));
  return result;
}

std::string lowercase(std::string value) {
  for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// Whether a spine item is an image (a cover or a plate put straight in the
// spine, SVG included): a picture, not a text chapter.
bool image_chapter(const EpubChapter& chapter) {
  return lowercase(chapter.media_type).starts_with("image/");
}

// The chapter Document an image spine item stands for: one picture whose
// source names the item itself, so the fold places it in the chapter's
// group and inlines the bytes the chapter event carried.
docv1::Document image_chapter_document(const EpubChapter& chapter) {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->mutable_image()->set_mimetype(chapter.media_type);
  // A leading slash names the archive root, so the reference resolves to
  // the item's own path.
  picture->mutable_image()->set_uri("/" + chapter.href);
  picture->add_source()->mutable_collector()->set_collector("epub");
  document.mutable_body()->add_children()->set_ref("#/pictures/0");
  return document;
}

// Whether a spine item is something the markup collector reads as HTML.
// Spine items are XHTML by the EPUB specification.
bool html_chapter(const EpubChapter& chapter) {
  const std::string type = lowercase(chapter.media_type);
  if (type == "application/xhtml+xml" || type == "text/html") return true;
  const std::string href = lowercase(chapter.href);
  return href.ends_with(".xhtml") || href.ends_with(".html") || href.ends_with(".htm");
}

}  // namespace

CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes,
                                       CollectorDeadline inbound_deadline,
                                       CollectorCancelled cancelled) {
  return std::move(read_epub_stream(channel, bytes, inbound_deadline, std::move(cancelled),
                                    kEpubStreamByteCap)
                       .outcome);
}

CollectorOutcome collect_epub_book(const std::shared_ptr<grpc::Channel>& epub,
                                   const std::shared_ptr<grpc::Channel>& markup,
                                   const std::string& bytes,
                                   CollectorDeadline inbound_deadline,
                                   CollectorCancelled cancelled, size_t stream_byte_cap) {
  EpubStream stream = read_epub_stream(epub, bytes, inbound_deadline, cancelled, stream_byte_cap);
  CollectorOutcome& outcome = stream.outcome;
  if (!outcome.success) return std::move(outcome);
  if (markup == nullptr) {
    outcome.warnings.push_back(
        "chapters were not folded: the markup collector is not configured "
        "(GRPARSE_MARKUP_TARGET), so the chapter groups stay empty");
    return std::move(outcome);
  }

  // One ceiling for every chapter dial together: a fresh cap per chapter
  // would let a long book against a slow markup service run for hours.
  const CollectorDeadline chapters_deadline =
      capped_collector_deadline(inbound_deadline, kDeadline);
  std::vector<ParsedChapter> chapters;
  chapters.reserve(stream.chapters.size());
  for (auto& chapter : stream.chapters) {
    if ((cancelled && cancelled()) || std::chrono::system_clock::now() >= chapters_deadline) {
      outcome.warnings.push_back("chapter '" + chapter.href +
                                 "' and the ones after it were not parsed: the book's "
                                 "chapter deadline passed or the call was cancelled");
      break;
    }
    if (image_chapter(chapter)) {
      chapters.push_back(ParsedChapter{chapter.href, image_chapter_document(chapter)});
      const bool carried =
          std::ranges::any_of(stream.images, [&chapter](const EpubResource& image) {
            return image.href == chapter.href;
          });
      if (!carried && !chapter.content.empty()) {
        stream.images.push_back(
            EpubResource{chapter.href, chapter.media_type, std::move(chapter.content)});
      }
      continue;
    }
    if (!html_chapter(chapter)) {
      outcome.warnings.push_back("chapter '" + chapter.href + "' (" + chapter.media_type +
                                 ") is not XHTML; its group stays empty");
      continue;
    }
    CollectorOutcome parsed =
        collect_markup_document(markup, chapter.href, "application/xhtml+xml", chapter.content,
                                chapters_deadline, cancelled);
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
