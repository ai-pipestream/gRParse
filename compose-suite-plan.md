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

### 2026-08-30 (later still): the outline becomes navigation; chapters get names

- Ask: "how about the chapters and spine and stuff? we can do better with epub". The
  parse already carried the spine (chapter groups in order, epub.spine_index/linear),
  and the TOC (Document.outline with real titles targeting the chapter groups); the
  viewer dropped all of it on the floor.
- Landed (viewer only): a collapsible "Table of contents" card above the results renders
  Document.outline (indent by level; entries click through the existing #item= anchor
  to scroll+highlight the target, or to the page card for page-only entries like PDF
  bookmarks; unreachable entries disable). Chapter groups render a real header: TOC
  title (href fallback in a tooltip), "chapter N of M" from the spine index, an
  "auxiliary" badge for linear=false. Group containers now register in model.view so
  group refs are anchorable at all. Works for every outline-bearing format, not epub
  alone.
- Verified live: gatsby 25 TOC entries / 7 chapter heads, alice 16 entries with the
  Roman-numeral chapter titles; clicking entry 3 sets #item=, highlights the group in
  view; sample.pdf shows no TOC card, renders 2 page cards, zero console errors.
- Environment note, not a regression: the freshly rolled grparse container could not
  allocate CUDA workspaces (CUBLAS failure 3, card at 15.8/16.4 GiB) because the North
  Micro Vision CUDA server from the 08-29 eval still held ~5.7 GiB. Stopped
  north-micro-vision-north-cuda-1 (docker start brings it back, weights cached),
  restarted grparse, card at 8.9 GiB, PDF paths green again.

### 2026-08-30 (evening): North Micro Vision serves from krick-1 only

- Policy: the 4080 belongs to gRParse's CV path; VLM serving lives on krick-1. The
  local north-cuda container stays stopped (docker start north-micro-vision-north-cuda-1
  to revive); north-xpu on krick-1:8086 (Arc Pro B70) had been up 40h and healthy.
- Direct test: sample.pdf page 1 through krick-1:8086 chat completions: 275 tokens at
  60.4 tok/s, faithful markdown (heading + list + prose).
- The stack's vlm-convert image predated the north_micro_vision preset (43f3290);
  rebuilt from ../grpc-vlm-convert and rolled, status now lists north_micro_vision.
  End to end through the shell bridge (preset VLM_PRESET_NORTH_MICRO_VISION, endpoint
  http://krick-1.taild24b1c.ts.net:8086): 1 page in 4.3s, pagesOk=1, 9 structured items.
- ENRICH_VLM_URL / GRPC_VLM_ENDPOINT defaults stay on the Qwen server (krick-1:8085);
  North is a per-request preset+endpoint choice, both servers on the same box.

### 2026-08-31: three agents, one ruler (scorecard, repair pass, picture sources)

- Program: three concurrent agents with disjoint ownership and no commits of their own;
  the coordinator committed by path. Agent 1 (eval/, tests/golden/) built the ruler,
  agents 2 (src/, include/, tests/*.cpp) and 3 (grpc-markup) were measured with it.
- Scorecard (`eval/scorecard/`, 338a2f1 e076922 807db70 df5cac9): 24-document corpus
  (22 small fixtures copied from sister repos, 2 external by path), one ConvertSource
  per doc, a compact structural summary per Document committed as the baseline, pure
  metric functions (text, order keyed on label plus first six words, table cell F1 and
  structure, headings, picture placement, warnings, claims agreement) with explicit
  tolerances, a Changes section listing what moved per doc, exit 0/1/77. Registered as
  the opt-in `structural-scorecard` ctest (LABELS eval). Whole run: ~3-5 s.
- grpc-markup 8fce0e0: pictures from `<svg><image>` (href, else namespaced xlink:href),
  small vector `<svg>` as a data URI, `<picture>` via its `<img>`, `srcset` without
  `src`, image `<object>`/`<embed>`; decorative svgs produce nothing; a CAPTION adjacent
  to a picture binds into PictureItem.captions (also AsciiDoc/LaTeX). +22 tests, live
  probe returned the nested EPUB cover as `#/pictures/0` with `image.uri` intact.
- gRParse repair pass (e1fa639 02765bc f8818e7): post-merge, format-agnostic, default
  on (`GRPARSE_REPAIR=on|off|debug`), counters `grparse_repairs_total`. Running
  header/footer demotion to furniture, line-break hyphen rejoin, paragraph continuation
  across page/column breaks. The scorecard caught two false positives on the 11-page
  paper on the first live run ("2023." from a wrapped reference demoted as a page
  number; a figure sub-caption merged as a continuation) and a third after the first
  fix (the enumerator rule only covered same-page merges). Final live run: 16 hyphens
  rejoined, 0 demotions, 0 merges, 24/24 pass, paper back to 121 body items.
- Build trap found on the way (e79a43e): a local `act` CI run of the same Dockerfile
  shares the `/build` cache mount and left objects newer than the edited sources, so
  ninja skipped `document_repair.cpp` and a green in-build ctest verified the OLD
  binary. Always check the build log compiled the file you changed (`grep 'Building
  CXX'`); `--build-arg GRPARSE_BUILD_CACHE_SCOPE=ci` gives a second builder its own
  tree. `touch` does not help: BuildKit keys COPY on content, not mtime.
- Scorecard observations not yet acted on: docx pictures all appended at body end (CV
  figures in nondeterministic order), paper heading levels erratic (title split 1+2,
  ABSTRACT level 3), html/md origin.mimetype application/octet-stream (FileSource has no
  content type, only the filename hint), xlsx sheets carry an empty PictureItem, bar
  chart yields no data. Each is a row in the report to move, not an opinion.
- Rejoin precision on the paper: 14 of 16 right; two joined fragments the layout had
  already broken ("hyper- t−1", "probabil- that"). A tail fragment of at least two
  letters would catch the first; the second needs a lexicon. Left as rows to move.
