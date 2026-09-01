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

### 2026-08-31 (night): three lanes, one ruler v2 (truth floors, layout, data)

- Program: same shape as the morning, three Fable agents with disjoint ownership
  and no commits of their own. Lane 1 (eval/, tests/golden/) worked in the main
  checkout; lanes 2 and 3 each in a worktree with its own build cache scope and a
  private CPU instance on the stack network, scored read-only with the main
  checkout's ruler. The coordinator committed per lane, merged from the main
  checkout, built with `grep 'Building CXX'` checks, deployed, scored twice, and
  re-recorded every intentional move with the commit and a reason.
- Lane 1, ruler v2 (4f450f2, 04b7f5b): truth files for 14 documents derived from the
  sources (docx/pptx/xlsx XML, pdftotext, tesseract), absolute metrics (headings,
  levels, reading-order anchors, table cells, figure anchors) with floors that
  ratchet in `_meta.json`; ten fixtures with deterministic generators (two-column
  PDF with footnotes and running header, rotated scan, form docx and pdf, docx with
  inline figures, pptx with notes, sixty-sheet workbook, three pdf-inspector
  samples); latency gate max(1.25x, +500 ms); `--repeat 2` stability; `EVAL_REQUIRE`.
  72 unit tests. Its first pass over the new fixtures produced the rows below.
- Lane 2, layout (4b954ac, merged cb3af81): geometry-only bodies re-ordered per page
  by XY-cut; heading hierarchy from numbering with the opening block promoted to
  TitleItem; run-in headings and form rows split; CV pictures deduplicated and
  anchored beside their paragraph in page order (deterministic); hyphen rejoin needs
  a two-letter tail word; running-furniture demotion judges the whole item and never
  demotes items over twelve words (it had removed every page paragraph from
  pdf-mixed and pdf-long-text); `page_rotation_vote` ready for the scheduler.
  Root cause on the paper: the two bad rejoins were line interleaving INSIDE single
  pdf-collector items, not body order; the paper is single-column, its floats came
  first on each page. 40 ctests.
- Lane 3, data (febea43, grpc-libreoffice cd8f667): office charts as one CHART
  picture with a bound TableItem and caption (an xlsx chart used to be two pictures,
  a pptx chart an empty text item); spreadsheets skip CV enrichment (the empty
  picture per sheet, 59 on sixty-sheets, and its nondeterministic page numbers);
  sheet header rows and spans; inline office pictures take their anchor paragraph's
  slot; HTML title as TITLE; deck title vs slide headings; mimetype sniffing with
  `origin.mimetype_evidence`; RSS and CPU gauges; `grparse_data_changes_total`.
  Collector side: Writer table grid from column separators (docx-sample3's table
  7x3 to 7x5) and chart replacement graphics re-encoded as PNG instead of
  StarView metafiles. 41 ctests after the merge.
- Live outcome (`final`, `--repeat 2`): 34 of 34 pass, every doc stable. Truth
  gains: paper headings 0.690 to 0.941 and levels 0.700 to 1.000; two-column
  headings 0.387 to 1.000; pptx-notes levels 0.250 to 1.000; pdf-form levels 0.500
  to 1.000; docx-sample3 and docx-figures figure anchors 0 to 1.000; html-entities
  headings 0 to 1.000; rotated-scan levels 0 to 0.500. Nothing fell. Counters over
  the day's runs: furniture_demoted 0, hyphens_rejoined 492, paragraphs_merged 6,
  cv_enrichment_skipped 38, sheet_header_rows 486, mimetypes_sniffed 234.
- Rows the ruler still shows, by owner:
  - grpc-pdf-inspector: (0,0,0,0) boxes for most multi-line paragraphs in
    two-column.pdf (no geometric order possible; footnote and last paragraphs stay
    out of order); lines of side-by-side blocks interleaved inside one item on the
    paper ("probabil-/that/ity") and heading 3.2 missing entirely; Figures 2 and 3
    have no picture items; long-text.pdf's 110 furniture items are the collector's
    own; form.pdf's intro paragraph arrives as table row 0.
  - gRParse: wire `page_rotation_vote` in the scheduler (re-OCR at 90 and 270, keep
    the higher mean confidence); export the new repair and office-CV totals in the
    Prometheus exposition; docx-sample3 still shows four pictures for two drawings
    because the collector's picture boxes sit one image width right of the drawing;
    epub chapter h1 comes out as the title (`epub_book.cpp`); raster charts
    (bar_chart.png) carry the classifier verdict only, a table needs a VLM derender
    into `PictureTabularChartData`.
  - Scorecard: no chart fixture yet (charts_bound stayed 0 in corpus runs; the lane
    3 generator, openpyxl and python-pptx, is the template); `memory.metrics_url`
    resolves the stack container for a localhost target (`EVAL_METRICS_URL`
    overrides); paper Table 1 has no truth cells.
  - Proto (fleet sweep): typed slots for the last keyed strings,
    `CollectorClaim.warnings`, `TextItemBase.page_style_name`, `GroupItem.slide`,
    chart titles on `ChartMeta`.
  - Stack: VLM default stays Qwen on :8085 with North a per-request preset; no
    enrich-side eval exists to justify flipping it.
- Method notes: the shared-cache trap appeared once more inside a single scope (an
  object compiled after its header was edited kept the newer mtime), so acceptance
  builds use a fresh scope. The first scorecard run after a deploy fails latency on
  one or two docs from warm-up; the second run is the verdict.

### 2026-09-01: anti-drift battery, four lanes (collector fidelity, charts, rotation, browser e2e), krick-1

- Program: an Opus agent first pinned the data (15daa89): five offline ctests
  (assembly determinism across runs and input permutations, geometry order and
  picture anchoring over all permutations, repair fixed points, the chart and sheet
  contracts, the mimetype precedence and an exact allowlist for colon-keyed custom
  fields, `cell:?` only) and three scorecard modules (fixture manifest with sha256,
  size caps and a personal-document denylist; truth schema and floors never above
  their baseline; generators that regenerate byte-for-byte or pin their clock). Then
  three Fable lanes in worktrees with private rigs, as before, plus a fourth for the
  browser suite; the coordinator committed per lane, merged from the main checkout,
  built in a fresh scope, deployed, scored twice, re-recorded with reasons.
- Lane A, grpc-pdf-inspector (6606929 vendor, 0cd27b0 service): a two-column page
  arrives line by line across both columns, so blocks are now located as run chains
  (two-column.pdf: 0 body items without a box, was 10 of 16 on page 1); fused
  side-by-side blocks split back in column order; Form XObjects reach the wire as
  `SPAN_KIND_FORM` placements and become pictures (paper Figures 2 and 3; 17
  pictures, heading 3.2 present, headings 0.941 to 0.971, table cells 0.927 to
  0.978); table lines outside the vertical rules with one populated cell are
  dropped (form.pdf: intro is a paragraph, table cells 0.065 to 1.000); the
  parser's repeated-text verdict applies only to edge lines and text runs
  (long-text furniture 110 to 0). Vendor suite unchanged at 1015/14. 30 tests.
- Lane B, charts (33a3604): one bar annotation per series (series past the first
  were dropped), positional series names, blank corner from the sheet header;
  fixtures charts.xlsx and charts.pptx with truth (charts_bound 48 per run, table
  cells 1.000 on both, was unmeasured); paper Table 1 truth (table cells floor
  0.927 to 0.978); an opt-in enrich call (`GRPARSE_ENRICH_TARGET`, 5 s deadline,
  off unless set) that sends chart pictures with pixels to grpc-enrich and stores
  the returned table in `tabular_chart` with GenerationSource provenance;
  `eval/chart_derender/compare.py` measured both VLM endpoints through enrich: Qwen
  on :8085 answered 0 of 12 chart images (ragged CSV, one reply asking for the
  image: the request wiring, not the model), North on :8086 answered 12 of 12 (line
  chart cells 1.00, bar 0.73, pie 0.80, 350 to 500 ms, stable). The stack now sets
  the leg on with North named per request; the enrich default is unchanged.
- Lane C, rotation and metrics (059c019): after the first recognition of a page
  with no text layer the angle vote picks 90/270, 180, or all three on a poor read;
  recognition runs again on the rotated raster, the best read is kept, and that
  raster replaces the original for layout, crops, previews and page size, so all
  geometry shares one upright frame (`PageItem.quality.rotation_degrees`, an
  existing typed slot). New fixtures rotated-scan-180 and rotated-scan-mixed; all
  three rotated docs at 1.000 on headings, levels, order and anchors (from
  0.800/0.500/0.333/0.600). Prometheus: `grparse_pages_rerecognized_total`,
  `grparse_rerecognition_passes_total`, `grparse_page_rotations_total{degrees}`,
  `grparse_repair_changes_total{kind}` (all nine, replacing `grparse_repairs_total`),
  `grparse_office_cv_total{kind}`, plus the derender kinds. Cost: the 90 degree
  scan went from 1.5 s to 4.8 s on the CUDA stack (three passes); recorded.
- Lane E, browser suite (7483ef9, 0079676): `e2e/` with 51 Playwright tests over the
  shell, the Document tab (one fixture per parser), the gRParse stream, each service
  UI, health, and one red marker; `compose.stack.e2e.yaml` (`e2e` profile) and
  `scripts/stack-e2e.sh`. Found and kept red: the shell proxy returns 400 on the
  request after a streamed POST to the same frontend (`examples/web-demo/server.js`
  `proxyToFrontend`; two parallel workers trip each other on it, so one worker is
  the default) and the Document tab progress bar stays visible after a parse.
- Local outcome (`final-3`, `--repeat 2`, CUDA stack): 38 scored, 38 pass, all
  stable; 49 ctests; 113 scorecard unit tests; e2e 50 passed, 1 skipped (no
  calamine tab). Re-records: three pdf docs for the collector, three rotated docs
  for orientation recovery, with the GPU latency budgets.
- Corpus note: `scanned-image.pdf` is three pages holding one 8x8 gray image each
  (a classification sample), so zero text items is the right answer; it is not an
  OCR fixture.
- krick-1 (Intel Arc, OpenVINO): the same merged state runs there from
  `~/parse-stack` (images by `docker save | zstd | ssh | docker load`, 24
  containers, `compose.stack.openvino.yaml` + `compose.stack.standalone.yaml`).
  Two host-specific faults found and fixed on the way: the OpenVINO GPU plugin
  compiles a kernel set per input size, so its cache filled the 256 MB `/tmp`
  tmpfs after a handful of documents and a truncated blob aborted the process
  (`clCreateProgramWithBinary`, exit 139; pages went missing before the abort);
  the cache now lives in a named volume (c919567; 847 MB after two corpus runs).
  And a host without sibling checkouts gets empty root-owned stubs for the
  shell's `../<repo>/proto` bind mounts, so every peer reported "proto
  unavailable"; the standalone overlay mounts them from `./protos`. Result on
  krick-1: scorecard 38 scored, 38 pass, all stable, twice (75 s and 56 s wall
  once the cache is warm; the 90 degree scan 5.0 s, on par with CUDA at 4.8 s),
  Playwright 50 passed, 1 skipped. Fourteen empty root-owned stub directories
  remain in krick-1's home (`~/grpc-*`, `~/fastwarc-grpc`, `~/grPOIc`) for the
  owner to remove.
- Follow-ons, by owner:
  - gRParse: the geometric re-cut moves a right-column item on two-column.pdf page 6
    now that all items carry boxes (`document_reading_order`, truth_order 0.978);
    orientation recovery could probe at reduced resolution before paying three full
    passes; OpenVINO compiles a kernel set per OCR crop width (2400 blobs after
    seven documents, 40 s per rotated doc cold), so crop widths want bucketing on
    that provider; the Dockerfiles could touch project sources before the build to
    end the stale-object trap for good; `page 10` reference item without a box.
  - grpc-enrich: `EnrichOptions` has no decoding controls (no greedy request);
    `ItemAnnotation.model` comes back empty; `ChartCsvParser` rejects pipe-separated
    tables and Qwen's output; the :8085 image-not-seen behaviour.
  - Shell: the proxy 400 after a streamed POST; the progress bar CSS.
  - Scorecard: the paper's Figure 2 anchor sits past the 60-char entry prefix; the
    e2e suite has no rows yet for the fastwarc, POI, ASR, Enrich and VLM Convert
    uploads; CI runs neither the e2e suite nor the scorecard.
  - Proto (fleet sweep): a typed multi-series bar slot, chart title on `ChartMeta`,
    `CollectorClaim.warnings`, `TextItemBase.page_style_name`, `GroupItem.slide`.

### 2026-09-01 (later): dependency refresh, source stamp, S3 corpus eval

- Dependencies (52c9c18, 51c0320): gRPC 1.83.1, OpenCV 4.14.0, express 5.2,
  @grpc/grpc-js 1.14, TypeScript 7.0, node 26, nginx 1.31, the GitHub actions
  majors. Unchanged on purpose: ONNX Runtime 1.29.0 and onnxruntime-openvino
  1.24.1 (the newest each provider publishes), poppler 26.08, CUDA 13.3.1 on
  ubuntu 26.04, zxing, yaml-cpp, simdutf, miniz, Playwright. OpenCV 5 is
  deferred: it changes pixels under the detectors and would move baselines
  for no gain in output. All three images green, 49 ctests each, scorecard
  unchanged before and after.
- Source stamp (258cc86): `scripts/stamp-sources.sh` keeps a content manifest
  of every first-party source under the cache mount and touches whatever
  changed since the previous build, so ninja can no longer keep an object
  compiled from an older file. The stale-object trap that bit the OpenVINO
  build during the rotation lane (one file recompiled, link failed) and again
  during this lane is closed; a warm build logs `stamp: 0`, a fresh scope
  `stamp: 173`.
- S3 corpus eval (5577daa): `eval/s3/run.py` lists a bucket, reads each object
  into memory, parses it twice through the unary API (and a sample per
  extension once more under a name with no extension), and runs 25 named
  shape checks over the merged Document, grouped by (parser type x file type).
  Exit 1 on a failed check, 77 when the bucket or gRParse is unreachable; the
  report groups failures by cause and names the owner of the known ones
  (`eval/s3/owners.py`). `compose.stack.s3.yaml` adds RustFS under the `s3`
  profile with credentials from the environment only;
  `eval/s3/seed_from_workspace.py` seeds it from the sibling repositories'
  fixtures (113 objects, 19 extensions, 11 repositories; no txt, csv, jpg or
  audio fixtures exist anywhere in the family, so those routes are covered by
  ctests only). 58 unit tests cover the tool.
- What the first runs found on the stack as deployed: 92 of 1474 checks failed
  across 68 objects. Seven were gRParse bugs, each fixed with a ctest (49 to
  56): the merge attributed `created` field by field inside the Timestamp
  (`created.seconds`, two collectors could interleave seconds and nanos); a
  name with no extension routed by name alone, so docx, eml, md, html and xml
  bytes died on the CV path; the generic `text/plain` sniff outranked a
  specific text extension (`.csv`); Writer pictures without a paragraph slot
  trailed the body (a page-23 figure after page 208); empty and hidden sheets
  folded as 1 x 1 tables with no cells, and a header row over formula cells
  was not marked; the integrity walk called line-addressed provenance
  "page 0" and rejected `#/pages/N` targets; a leg failing with an empty
  status message read "markup collector: ". After the fixes: 33 of 1470
  checks red on 32 objects on the merged stack, twice, then 34 of 1483 on
  the same 32 objects once the 15 MB page parsed (its ragged grids), every
  one with an owner outside the lane (below). Nineteen scorecard baselines
  re-recorded with a reason (the agreement section now names `created` and
  `modified` whole).
- Outcome on the final image (3c563e9 and after): 57 ctests on both build
  stages, the stamp recompiling only the touched files; scorecard 38/38 on
  the local CUDA stack (twice on an idle stack; a run taken while the S3 pass
  was loading the same parsers regressed on latency alone) and 38/38 twice
  on krick-1; S3 eval 34/1483 as above; Playwright 50 passed, 1 skipped on
  krick-1 and locally; the shell proxy spec asserts for real on both.
- Two more from running it on the merged stack: with the status text fixed, the
  15 MB WHATWG page's markup leg turned out to fail with RESOURCE_EXHAUSTED
  ("Received message larger than max"): the collector channels kept gRPC's
  4 MB default receive limit while the server accepts 520 MB. One constant
  (`kMaxMessageBytes`) now sets both sides and every collector channel; ctest
  `collector-channel-limits-test` pins it (57 ctests). And a parse that ran
  past gRParse's own budget (calamine's far-corner sheet, about ten minutes)
  was parsed twice more by the runner; `EVAL_S3_CONVERT_TIMEOUT` (600 s)
  caps a conversion and a timed-out object is parsed once.
- The shell proxy 400 after a streamed POST does not reproduce behind nginx
  1.31 (the deps bump), on either stack, in five stream/request pairs; it did
  behind 1.27. `e2e/specs/proxy.spec.ts` is a live assertion now and the
  workaround request in the UI flows is gone. Seen once under load: grpc-markup
  answered a UI parse of streaming-markup.md with no blocks while the S3 eval
  was driving it (passes alone); the suite stays at one worker.
- Kept red on purpose (the report names them): collector warnings ride
  `custom_fields[collector_warnings:<name>]` because the Document has no typed
  warnings slot (schema sweep); every WARC object fails because the stack
  leaves `GRPARSE_FASTWARC_TARGET` unset (wire dialect); calamine's
  `corners.xlsx` runs the office render past its timeout; grpc-markup emits
  ragged grids for spanning header cells (`streaming-markup.html`, and 23
  tables of the 15 MB WHATWG page once it parsed). The eval's own HTML
  heading reader was a regex and took a `<h6>` quoted inside a `title`
  attribute of that page for a heading; it is the stdlib tokenizer now.
- Follow-ons, by owner:
  - gRParse: typed `Document.warnings` to retire the keyed warnings; a way to
    judge blank rasters without the page preview; the e2e suite still has no
    S3 row.
  - fastwarc-grpc: reconcile the vendored `fastwarc.v1` dialect with the image.
  - grpc-libreoffice: far-corner sparse sheets; grpc-markup: the 15 MB page and
    spanning header cells in the grid.
  - Corpus: add txt, csv, jpg and audio fixtures somewhere the seed can find them.

### 2026-09-01 (night): cleanup and decomposition, four lanes, same output

- Survey first: 21k lines of C++ across src/ and include/, 22k of tests. Smell
  debt was near zero (no dead code, no TODOs, no stream flushes), so the work
  was structural: four files had grown into everything-files (docling_map.cpp
  3057 lines, document_parser_service.cpp 1752, markdown_renderer.cpp 1747,
  confluence_storage.cpp 1401, then document_collectors.cpp 1174 and a
  281-line main()), 56 of 60 test files repeated the same assertion helper,
  and fourteen units had no test naming them. The Python under eval/ got a
  ruff pass (imports, deprecated typing, ambiguous names) with the config
  pinned in ruff.toml.
- Four lanes in worktrees with disjoint ownership, none committing, each
  building in its own cache scope and each holding the same identity gate:
  every ctest, then the scorecard against a private CPU instance built from
  its tree with the Changes section identical before and after (a pure
  refactor moves nothing; nobody re-records). Results:
  - Mapper: DoclingMapper is an event router over grparse::office_fold, one
    unit per plane with its own pending state, an arena, an anchor index and
    the integrity walk; 36 files, largest 484 lines, no translation-unit
    fallback.
  - Service: five units for the parser service (endpoints, shared support,
    per-source pipeline, unary, streaming), one collector leg per file under
    src/collectors, main() 281 to 60 lines with every environment read in
    server_config (names, defaults, messages and the startup log
    byte-identical against the previous image), the sniff rules a flat table.
  - Render: markdown as per-item emission over a structural walk with the
    value, meta, label, table and text helpers in units of their own;
    Confluence storage as six units; canonical JSON judged one flat
    serialiser and split by layer only. Every renderer's output over the 36
    golden documents identical before and after (180 files).
  - Tests: tests/support/check.h and document_builder.h replace the repeated
    helpers; 12 new test files (geometry, display width, load normalisation,
    the html, doctags, doclang and vtt renderers, renderer_base, the sniff
    table, target_step, determinism); 57 to 69 ctests.
- What the new tests found: the protobuf JSON export (and the YAML that
  re-emits it) depended on map iteration order, so two equal Documents could
  render different bytes. Fixed with render/json_key_order (map objects sorted
  by content, everything else copied through); the red test is a regular one,
  70 ctests. Two smaller facts pinned as passing tests: `.vtt` is not in the
  extension table although a comment says it is, and a bare "BM" is text.
- Merge order mapper, render, tests, service; no conflicts. Outcome on the
  merged image: 70 ctests on both build stages; scorecard 38/38 twice on the
  local CUDA stack with the Changes section identical to the pre-refactor run
  (the pdf-two-column committed-summary drift from the S3 lane); krick-1
  38/38 twice and Playwright 50 passed, 1 skipped on the OpenVINO image; the
  S3 eval on the refactored stack 34 of 1483 checks red on the same 32
  objects with the same findings as before the refactor.
- Playwright locally: a full run taken right after the S3 passes failed
  figures.docx ("upload never finished": grpc-libreoffice was still rendering
  corners.xlsx server-side after the eval's client deadline) and the markup
  UI ("Parsed with no content blocks"); on the quiet stack the document specs
  pass, three repeats of the service-UI sequence pass 24/24, and the markup
  parse API answers identically 10 of 10 through the proxy and direct. The
  markup UI symptom is an intermittent flake of that UI's browser flow (seen
  three times today, always after heavy load, never on demand); owner
  grpc-markup, next step is a trace of the failing run's network log.
- Follow-ons: document_repair.cpp (966) and document_assembly.cpp (888) are
  the next largest units and were not touched; `.vtt` in the extension table;
  the four bespoke test mains (chunking, the three model-gated tests) keep
  their own shape; the markup UI flake above.

### 2026-09-02: the open dependency PRs on Forgejo, reviewed and merged

- Twenty Renovate PRs were open across thirteen parser repos. Seventeen are
  merged, each on a green run of its repo's own gate against the merged
  result (Gradle builds, cargo test plus clippy and fmt, Docker image
  builds; grpc-opennlp-analysis additionally nativeCompile for the GraalVM
  plugin bump). Renovate does not refresh Cargo.lock here, so each crate
  bump needed a lock refresh on the branch; grpc-xml and grpc-epub needed
  the quick-xml 0.42 string-API migration, grpc-ebcdic and grpc-xml a
  libprotobuf-dev line in the Dockerfile (a baseline failure, not the
  bump's).
- Three stay open on purpose, each with the evidence in a PR comment or the
  branch: grpc-pdf-inspector lopdf 0.44 and pyo3 0.29 both edit the
  vendored parser tree whose version moves are the re-vendoring procedure
  (and lopdf 0.44 turns a page decode failure into silently blank text);
  calamine quick-xml 0.42 is 171 compile errors in upstream parsing code
  that upstream itself has not migrated. The central Renovate config now
  ignores vendor/** so the two vendored-tree PRs stop regenerating.
- Two findings for the owner:
  - grpc-ebcdic, grpc-xml and grpc-epub have Forgejo default branch
    `development` that is a stale strict ancestor of `main` (which GitHub
    mirrors and which sits 15, 35 and 16 commits ahead), so Renovate and
    these merges land on a dead line. On `main` the rust 1.98 and
    libprotobuf fixes already exist and grpc-xml is already on quick-xml
    0.42; grpc-epub's quick-xml 0.42 migration is the one real piece not on
    `main`. Repoint Renovate or retire `development`.
  - calamine `master` had a policy of staying byte-identical to upstream
    (the 2026-08-15 revert of the dependency rollup says so); merging the
    criterion and sha2 bumps departs from it. If the policy stands, revert
    c15bda98 and 32d45e50 and add a Renovate ignore for this repo.
- Forgejo push-mirrors cover every repo checked (grpc-email, grpc-markup,
  grpc-pdf-inspector, calamine verified in sync after the merges), so no
  manual GitHub pushes were needed. grpc-epub has a pre-existing flaky
  streaming test (fast hosts defeat its fewer-than-all-chapters assertion).
