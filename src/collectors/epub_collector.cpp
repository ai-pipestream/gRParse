#include "grparse/document_collectors.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ai/pipestream/epub/v1/epub_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/epub_book.h"

namespace docv1 = ai::pipestream::document::v1;
namespace epubv1 = ai::pipestream::epub::v1;

namespace grparse {

namespace {

constexpr size_t kMaxConcurrentChapterDials = 8;

// The decompressed bytes a book's stream holds at once. The reader charges
// every chapter and image as it arrives; a chapter the markup dials have
// finished with gives its bytes back, so the cap bounds bytes in flight
// rather than bytes seen. Only the reader charges, so only it can trip it.
class StreamBytes {
 public:
  explicit StreamBytes(size_t cap) : cap_(cap) {}

  // False once the charge passes the cap, and from then on.
  bool take(size_t size) {
    if (over_) return false;
    if (held_.fetch_add(size, std::memory_order_acq_rel) + size <= cap_) return true;
    over_ = true;
    return false;
  }

  void give_back(size_t size) { held_.fetch_sub(size, std::memory_order_acq_rel); }

  size_t cap() const { return cap_; }

 private:
  const size_t cap_;
  std::atomic<size_t> held_{0};
  bool over_ = false;
};

// The epub stream with its typed events kept: the skeleton Document as the
// outcome, plus every chapter and image resource in arrival order. A
// chapter goes to `on_chapter` instead when one is given, the moment its
// event arrives, so its dial can start while the rest of the book is still
// being inflated.
struct EpubStream {
  CollectorOutcome outcome;
  std::vector<EpubChapter> chapters;
  std::vector<EpubResource> images;
};

using ChapterSink = std::function<void(EpubChapter)>;

EpubStream read_epub_stream(const std::shared_ptr<grpc::Channel>& channel,
                            const std::string& bytes, CollectorDeadline inbound_deadline,
                            CollectorCancelled cancelled, StreamBytes& held,
                            const ChapterSink& on_chapter = {}) {
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
  // Past the cap the call is cancelled and nothing more is kept.
  bool over_cap = false;
  const auto keep = [&](size_t size) {
    if (over_cap) return false;
    if (held.take(size)) return true;
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
        EpubChapter kept{chapter->href(), chapter->media_type(),
                         std::move(*chapter->mutable_content())};
        if (on_chapter) {
          on_chapter(std::move(kept));
        } else {
          result.chapters.push_back(std::move(kept));
        }
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
                           std::to_string(held.cap()) + " bytes once decompressed";
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

// One chapter of the book, in spine (arrival) order, with what its dial
// made of it.
struct BookChapter {
  EpubChapter chapter;
  bool is_image = false;
  bool is_html = false;
  bool dialed = false;
  bool skipped_for_deadline = false;
  CollectorOutcome parsed;
};

// The markup dials of one book, run while its epub stream is still being
// read. The reader hands each chapter over as it arrives; an XHTML chapter
// joins a queue at most kMaxConcurrentChapterDials deep (the reader waits
// for room, which holds the epub stream back by flow control rather than
// buffering it) and at most kMaxConcurrentChapterDials workers, started as
// chapters need them, dial the queue in spine order. Each dial writes its
// chapter's own entry, so the fold reads the chapters in spine order
// whatever order their dials finished in.
class ChapterDials {
 public:
  ChapterDials(std::shared_ptr<grpc::Channel> markup, CollectorDeadline deadline,
               CollectorCancelled cancelled, StreamBytes& held)
      : markup_(std::move(markup)),
        deadline_(deadline),
        cancelled_(std::move(cancelled)),
        // The dials also end when the book is abandoned (the epub stream
        // failed or passed its cap), so a hung markup call never holds the
        // failed leg until its deadline.
        dial_cancelled_([this] { return abandoned_.load() || gone(); }),
        held_(held) {}

  ~ChapterDials() { finish(/*abandon=*/true); }
  ChapterDials(const ChapterDials&) = delete;
  ChapterDials& operator=(const ChapterDials&) = delete;

  // The reader's sink: keeps the chapter in spine order and queues its
  // dial when it is XHTML. Only the reader calls it.
  void add(EpubChapter chapter) {
    std::unique_lock<std::mutex> lock(mutex_);
    BookChapter& entry = chapters_.emplace_back();
    entry.chapter = std::move(chapter);
    entry.is_image = image_chapter(entry.chapter);
    entry.is_html = !entry.is_image && html_chapter(entry.chapter);
    if (!entry.is_html) return;
    room_.wait(lock, [this] { return queue_.size() < kMaxConcurrentChapterDials; });
    queue_.push_back(&entry);
    if (idle_ == 0 && workers_.size() < kMaxConcurrentChapterDials) {
      workers_.emplace_back([this] { run(); });
    } else {
      work_.notify_one();
    }
  }

  // Waits for every queued dial; `abandon` skips the ones not started and
  // cancels the ones in flight. Returns the chapters in spine order.
  std::deque<BookChapter>& finish(bool abandon) {
    if (abandon) abandoned_.store(true);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    work_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    return chapters_;
  }

 private:
  bool gone() const { return cancelled_ && cancelled_(); }

  void run() {
    for (;;) {
      BookChapter* entry = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ++idle_;
        work_.wait(lock, [this] { return !queue_.empty() || closing_; });
        --idle_;
        if (queue_.empty()) return;
        entry = queue_.front();
        queue_.pop_front();
      }
      room_.notify_one();
      dial(*entry);
    }
  }

  void dial(BookChapter& entry) {
    const size_t size = entry.chapter.content.size();
    // Once one chapter missed the deadline (or the call went away) every
    // chapter queued after it is skipped too: the book is cut at the first
    // chapter it could not parse, never left with holes.
    if (abandoned_.load() || deadline_tripped_.load() || gone() ||
        std::chrono::system_clock::now() >= deadline_) {
      deadline_tripped_.store(true);
      entry.skipped_for_deadline = true;
    } else {
      entry.parsed = collect_markup_document(markup_, entry.chapter.href,
                                             "application/xhtml+xml", entry.chapter.content,
                                             deadline_, dial_cancelled_);
      entry.dialed = true;
    }
    // The XHTML is done with either way; its bytes leave the stream's cap.
    std::string().swap(entry.chapter.content);
    held_.give_back(size);
  }

  const std::shared_ptr<grpc::Channel> markup_;
  const CollectorDeadline deadline_;
  const CollectorCancelled cancelled_;
  const CollectorCancelled dial_cancelled_;
  StreamBytes& held_;
  std::atomic<bool> abandoned_{false};
  std::atomic<bool> deadline_tripped_{false};
  std::mutex mutex_;
  std::condition_variable work_;
  std::condition_variable room_;
  // A deque, so a worker's entry stays put while the reader appends.
  std::deque<BookChapter> chapters_;
  std::deque<BookChapter*> queue_;
  std::vector<std::thread> workers_;
  size_t idle_ = 0;
  bool closing_ = false;
};

}  // namespace

CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes,
                                       CollectorDeadline inbound_deadline,
                                       CollectorCancelled cancelled) {
  StreamBytes held(kEpubStreamByteCap);
  return std::move(
      read_epub_stream(channel, bytes, inbound_deadline, std::move(cancelled), held).outcome);
}

CollectorOutcome collect_epub_book(const std::shared_ptr<grpc::Channel>& epub,
                                   const std::shared_ptr<grpc::Channel>& markup,
                                   const std::string& bytes,
                                   CollectorDeadline inbound_deadline,
                                   CollectorCancelled cancelled, size_t stream_byte_cap) {
  StreamBytes held(stream_byte_cap);
  if (markup == nullptr) {
    EpubStream stream = read_epub_stream(epub, bytes, inbound_deadline, cancelled, held);
    CollectorOutcome& outcome = stream.outcome;
    if (!outcome.success) return std::move(outcome);
    outcome.warnings.push_back(
        "chapters were not folded: the markup collector is not configured "
        "(GRPARSE_MARKUP_TARGET), so the chapter groups stay empty");
    return std::move(outcome);
  }

  // One ceiling for the whole book, epub stream and chapter dials together,
  // from when the epub call starts: a fresh cap per chapter would let a long
  // book against a slow markup service run for hours.
  const CollectorDeadline chapters_deadline =
      capped_collector_deadline(inbound_deadline, kDeadline);
  ChapterDials dials(markup, chapters_deadline, cancelled, held);
  EpubStream stream = read_epub_stream(epub, bytes, inbound_deadline, cancelled, held,
                                       [&dials](EpubChapter chapter) {
                                         dials.add(std::move(chapter));
                                       });
  CollectorOutcome& outcome = stream.outcome;
  std::deque<BookChapter>& book = dials.finish(/*abandon=*/!outcome.success);
  if (!outcome.success) return std::move(outcome);

  std::vector<ParsedChapter> chapters;
  chapters.reserve(book.size());
  for (BookChapter& slot : book) {
    auto& chapter = slot.chapter;
    if (slot.skipped_for_deadline ||
        (!slot.dialed &&
         ((cancelled && cancelled()) ||
          std::chrono::system_clock::now() >= chapters_deadline))) {
      outcome.warnings.push_back("chapter '" + chapter.href +
                                 "' and the ones after it were not parsed: the book's "
                                 "chapter deadline passed or the call was cancelled");
      break;
    }
    if (slot.is_image) {
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
    if (!slot.is_html) {
      outcome.warnings.push_back("chapter '" + chapter.href + "' (" + chapter.media_type +
                                 ") is not XHTML; its group stays empty");
      continue;
    }
    // The markup leg's warning keeps whatever it typed; only its words gain
    // the chapter they are about.
    for (Warning& warning : slot.parsed.warnings) {
      warning.message = chapter.href + ": " + warning.message;
      outcome.warnings.push_back(std::move(warning));
    }
    if (!slot.parsed.success) {
      outcome.warnings.push_back("chapter '" + chapter.href +
                                 "' could not be parsed by the markup collector: " +
                                 slot.parsed.error + "; its group stays empty");
      continue;
    }
    chapters.push_back(ParsedChapter{chapter.href, std::move(slot.parsed.document)});
  }
  fold_epub_book(std::move(chapters), stream.images, &outcome.document, &outcome.warnings);
  return std::move(outcome);
}
}  // namespace grparse
