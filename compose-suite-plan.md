- gRParse shell (`examples/web-demo/server.js`, same day): the streaming tab drew
  bottom-left point boxes as top-left pixels, mirroring the fast-path overlay down
  the page; `mapBoundingBox` now flips by `coord_origin` against the page height.
  Verified with a zoomed headless capture of the repair receipt: every box on its text.

### 2026-08-30: epub books read as one Document (chapter fold + inline images)

- Finding: every epub came back with 0 texts, 0 tables, 0 pages, empty chapter groups
  and pictures by `epub:<href>` reference only, so the shell had nothing to draw. Not a
  rendering bug: grpc-epub emits a skeleton by contract ("the chapter groups are
  sockets the HTML collector's items are merged into downstream") and gRParse's
  `collect_epub_document` dropped the typed chapter/resource events and never dialed
  markup. The socket design was built; the plug was never written.
- Landed: `src/epub_book.cpp` (`resolve_epub_href`, `fold_epub_book`) and
  `collect_epub_book` in `document_collectors.cpp`. The epub stream's chapters and image
  bytes are kept; each XHTML chapter is dialed through markup with the HTML hint; the
  chapter's items are re-parented under the chapter group named by its href (spine
  order); chapter pictures resolve `src` against the chapter path, retire the
  skeleton's body-level duplicate (manifest facts + epub source ride along), and
  become `data:<media type>;base64,` URIs (16 MiB cap). Chapter-level identity (origin,
  source_meta, claims, meta_tags, ...) is stripped: a chapter's <title> is not the
  book's. `rewrite_references` exported from document_merge for the arena renumbering.
- Degradations are warnings, never failures: markup unconfigured (names
  GRPARSE_MARKUP_TARGET), a chapter markup rejects, a non-XHTML spine item, a missing or
  oversized image, a chapter with no group (joins the body's end).
- Tests: `tests/epub_book_test.cpp` (resolution matrix, placement, retirement +
  renumbering, inlining, degradations) and three `collect_epub_book` cases in
  `document_collectors_test.cpp` on a chapter-streaming fake epub + an HTML fake markup.
- Rejected: rendering EPUB to PDF pages for the CV path. The libreoffice image only has
  libepubgen (export), and paginating a reflowable book invents layout that the XHTML
  already states semantically. Figure classification on the now-carried bytes is the
  natural follow-on, without a page model.
- Live (parse-stack, rebuilt image): Alice 800 texts / 25 images inlined / 14 chapters, the
  paintings book 158 / 24 / 20, two Calibre samples 163 and 150 texts; 0 warnings on all
  four; Document tab renders 825 items with the illustrations inline, no console errors.
- Follow-on (grpc-markup): a cover page whose only content is `<svg><image xlink:href>`
  projects no picture, so that chapter group stays empty and the cover keeps its body-level
  seat (still inlined). Mapping SVG `<image>` to a PictureItem in markup closes it.

### 2026-08-30 (later): the main tab shows the union; markdown export in the shell

- Ask: "grparse should be able to show the union of the two joined together". The union
  already existed (epub skeleton + markup chapters merged into one Document); only the
  main page-stream tab had nothing to draw for a page-less format.
- Landed (shell only, no wire change): /api/parse's collector event now carries the
  collector's complete Document; when a parse completes with zero pages and a collector
  document arrived, index.html hands it to document.html via sessionStorage
  ("grparse-document-handoff") and embeds the viewer in an iframe below the stats
  (quota overflow degrades to a link). document.html?handoff=1 consumes the key, hides
  its dropzone, renders. .epub added to the picker accept list and content-type map.
- Markdown: /api/document/parse now requests OUTPUT_FORMAT_MARKDOWN (the wire field is
  exports.md, not exports.markdown - the first build read the wrong name) and the
  Document tab grows a "Download Markdown" button when the export rides along.
- Verified live: gatsby.epub on the main tab renders 1695 items with the image inline
  in the embedded viewer, dropzone hidden, no console errors; sample.pdf still renders
  2 page canvases with no iframe; markdown export 292 KB / 13 headings for gatsby,
  617 B for sample.html; markdown button shows for parses, never for JSON uploads.
- Known wart: the markdown serializer renders meta custom fields by reference parity
  ("append_custom_fields" in src/render/markdown_renderer.cpp), so epub/markup
  provenance keys (epub.idref, html.src, ...) surface as bare paragraphs near pictures
  and chapter heads. Parity-gated; fixing it means an options knob on render_markdown,
  not a fold change.
- The cwd trap struck again: a `cd ..` from examples/web-demo made the second shell
  rebuild silently a no-op (compose file not found), which looked like protobufjs
  dropping to_formats. Absolute paths for compose invocations, always.
