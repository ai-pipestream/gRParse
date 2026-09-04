# PDF backend services: capability matrix and service plan

Status 2026-09-04: M0 through M5 landed. The contract is released
(pipestream-protos v0.15.0, `pdf-backend` module), the three services are up
(grpc-pdfium tiers 0-2, grpc-qparse, grpc-poppler), the differential runs
three ways (`eval/pdf_diff`, RESULTS-2026-09-04), and gRParse has the
`GRPARSE_PDF_BACKEND` client mode with the scorecard run both ways
(SCORECARD-LEGS-2026-09-04) and the raster wire cost recorded
(RASTER-COST-2026-09-04). M6 remains evidence-gated: reading order on
long-text and two-column and the digital+OCR merge on the mixed rotated
scan are the open gaps, and the latency verdict awaits acceptance. The
in-process poppler path remains the default until then.

## Goal

Three interchangeable PDF backend containers behind one typed proto contract:

| service | engine | engine license | role |
|---|---|---|---|
| grpc-pdfium | PDFium (Chrome's PDF engine) | Apache-2.0/BSD-3 | the standard: fastest, most fuzz-hardened, permissive |
| grpc-qparse | the MIT qpdf-based cell parser (reference checkout under /work/main/reference-code/) | MIT (deps: qpdf Apache-2.0, blend2d Zlib, freetype, openjpeg BSD, lcms2 MIT) | model-ready text cells, deep graphics resources; may later be linked directly |
| grpc-poppler | Poppler | GPL-2-or-later | extraction quality reference and differential leg; the GPL stays inside this one container |

End state: the default shipped stack contains no GPL. gRParse returns to pure
Apache-2.0 once its PDF layer dials a backend service (or directly links one of
the two permissive engines). grpc-poppler remains an optional, clearly labeled
GPL container used for differential evaluation, off the default release path.

## What gRParse consumes today (the floor for the contract)

From `src/in_memory_document.cpp` (poppler-cpp):

- `document::load_from_raw_data` (in-memory bytes, diskless)
- per page: dimensions, orientation quarter-turn detection
- `page->text_list(text_list_include_font)`: text boxes with bbox and font info
- `page_renderer.render_page(dpi)`: BGR24 raster at a chosen DPI
- calls are serialized behind `PopplerGate` on arm64 (crash-driven), concurrent elsewhere

Any backend that can serve those four families at parity can replace the
in-process path. Everything else in the matrix is additional surface the
common shape should carry so no backend's data is dropped.

## Capability matrix

Legend: yes = exposed as typed data by the engine's public API; render = only
visible in rendered output; no = absent. "cpp" marks things poppler's narrow
C++ wrapper lacks even though poppler core/glib has them (the service would
use the wider API, not just poppler-cpp).

| data family | poppler | qparse | pdfium |
|---|---|---|---|
| load from memory | yes | yes | yes (`FPDF_LoadMemDocument`) |
| page size / rotation | yes | yes | yes |
| text with quads + fonts | yes (boxes + font; best layout heuristics) | yes (cells: axis bbox, rotated quad, direction, space width, rendering mode) | yes (`fpdf_text`: per-char quads, font, flags; no layout heuristics) |
| reading-order help | line/word assembly heuristics | cell sanitizers merge into reading-order cells (its differentiator) | tagged-PDF struct tree (`fpdf_structtree`), when the document has one |
| page raster | yes (splash/cairo, mature) | yes (blend2d renderer; new, maturity unproven) | yes (first class, fast) |
| embedded images as data | core yes (cpp no) | yes (bbox + decoded pixels; jpeg/jp2/jbig2) | yes (`fpdf_edit` page-object walk) |
| vector paths as data | no (render only) | yes (page shapes) | yes (`fpdf_edit` path objects) |
| fonts incl. embedded blobs | core partial | yes (font programs exposed) | yes |
| outline / bookmarks | yes | yes (TOC json: titles and nesting, no destinations) | yes (`fpdf_doc`) |
| hyperlinks | yes | yes | yes |
| annotations | core/glib yes (cpp no) | partial (qpdf annots) | yes, rich (`fpdf_annot`) |
| form fields | core/glib yes (cpp no) | yes (widgets) | yes incl. interactive (`fpdf_formfill`) |
| attachments / embedded files | yes | via qpdf, not surfaced | yes (`fpdf_attachment`) |
| digital signatures | no | no | yes (`fpdf_signature`) |
| embedded JavaScript listing | core aware | no | yes (`fpdf_javascript`) |
| document metadata / XMP | yes | XMP packet only | info keys only: no XMP, no custom keys (`FPDF_GetMetaText`) |
| encryption info / passwords | yes | qpdf is strong here but the public surface does not expose it yet | yes |
| linearized / progressive load | no | no | yes (`fpdf_dataavail`) |
| page thumbnails | no | no | yes (`fpdf_thumbnail`) |
| thread model | concurrent per document (arm64 gate) | concurrent per instance | none: one global lock, process-wide |
| build shape | CMake from tarball (current Dockerfile) | CMake, vendors its deps | GN/depot_tools from source, or sha-pinned prebuilt binaries |

Two matrix notes worth designing around:

- pdfium's struct-tree access is the only typed reading-order signal any of
  the three offers, and only tagged PDFs carry one. The contract should carry
  it as an optional tree, not fold it into cells.
- qparse is the only engine that types out the graphics resource world
  (colorspaces, shadings, patterns, xobjects). That belongs in the contract as
  an optional deep-resources block; the other two declare it absent.
  Implementation note (M3): the engine decodes these internally but its
  public surface does not serialize them yet, so grpc-qparse declares the
  family unsupported until an engine-side patch exposes it.

## The common contract

New proto package (lives in the protos repo per fleet convention, stubs from
`pipestream-protos-stubs`):

- `PdfBackend` service, three RPCs:
  - `Probe(DocumentBytes) -> BackendCapabilities`: what this backend will
    populate for THIS document (capabilities are per-backend AND per-document:
    struct tree only exists when tagged, signatures when signed).
  - `Parse(DocumentBytes) -> stream PdfParseChunk`: typed document tree,
    streamed page by page under the fleet message-size conventions.
  - `Render(RenderRequest) -> stream PageRaster`: page range at a DPI and
    pixel format; separate RPC because rasters dominate bytes and callers
    often want text without pixels.
- Shape rules follow the Document conventions: strongly typed messages per
  family (`TextCell`, `FontRef`, `EmbeddedFont`, `PlacedImage`, `VectorShape`,
  `Hyperlink`, `FormField`, `Annotation`, `OutlineNode`, `StructNode`,
  `AttachmentMeta`, `SignatureInfo`, `DocMeta`, `EncryptionInfo`), no keyed
  strings, every absent family declared absent in `BackendCapabilities`
  rather than silently empty. `TextCell` carries both the axis-aligned bbox
  and the rotated quad, direction, space width and a font table reference,
  which is the union of what the three engines emit.

## Service design

One repo per backend, same skeleton as the other single-format parsers
(tonic or C++ per engine, streaming server, health, reflection):

- **grpc-pdfium** (Apache-2.0, the standard). C++ against `public/fpdfview.h`
  et al. Engine from the sha-pinned prebuilt binary release (the pypdfium2
  precedent), not a source build; the depot_tools/GN path stays available for
  a later from-source hardening pass if we want our own compile. The engine's
  global-lock rule becomes a deployment shape: a small pool of single-threaded
  worker processes behind the gRPC front, page-range sharding across workers
  for large documents. V8/XFA stay out.
- **grpc-qparse** (our wrapper Apache-2.0; engine MIT). CMake, vendoring the
  engine's own dependency set the way its upstream build does. Thread-per-
  request with per-document parser instances. Because the engine is MIT it may
  ALSO later be linked directly into gRParse; the service exists for the
  differential program and contract symmetry, not for license isolation.
- **grpc-poppler** (GPL-3.0-or-later, and says so in its README). Uses the
  wider core/glib API, not just poppler-cpp, so annotations, forms and the
  struct tree reach the wire. Mirrors the arm64 serialization gate. Ships as
  an optional compose profile; excluded from default release artifacts so the
  default stack stays GPL-free once gRParse drops its own linkage.

Evaluation from day one: a differential leg beside eval/s3 and the scorecard
that runs the same PDF corpus through all available backends and tables the
divergences per data family (the same verdict discipline as the frontend
regression legs). The corpus starts from the scorecard PDFs plus
`~/parser-failed-docs`.

## Feature tiers

The contract defines every message up front (field numbers are forever); the
tiers say what a backend must FILL to pass each milestone, not what the proto
contains.

**Tier 0, the floor** (parity with what gRParse consumes today; a backend is
useless to the pipeline without all four):
1. Load from bytes, password optional; typed load errors.
2. Page inventory: count, per-page size, rotation.
3. Text cells: text, axis bbox, rotated quad, direction, space width,
   rendering mode, font table reference.
4. Page rasters: DPI + pixel format, streamed page range.

**Tier 1, standard extraction** (every backend can fill all of these):
5. Document metadata (info keys + XMP) and encryption/permissions info.
6. Font table with embedded font programs where present.
7. Placed images as typed data (bbox, decoded pixels or pass-through stream).
8. Hyperlinks.
9. Outline / bookmarks (qparse declares absent).

**Tier 2, rich families** (capability-gated; pdfium fills most, others some):
10. Annotations.
11. Form fields.
12. Attachments / embedded files.
13. Vector shapes (qparse, pdfium).
14. Tagged-PDF struct tree (pdfium only).
15. Signatures, JavaScript listing, thumbnails (pdfium only).
16. Deep graphics resources: colorspaces, shadings, patterns, xobjects
    (qparse only).

**Cross-cutting, present from the first commit:** `Probe` capabilities per
document, streamed `Parse` chunks under fleet size limits, `Render` as its own
RPC, typed absent-vs-empty discipline throughout.

## Landing order

Each milestone is one lane with its own gate; nothing depends on a later one.

- **M0: the contract.** Full proto (all tiers) in the protos repo, stubs
  released. Gate: buf lint/breaking, stub release consumed by a walking
  skeleton.
- **M1: grpc-pdfium at tier 0 + the differential harness.** Worker-process
  pool, pinned prebuilt engine, floor families only, and the differential
  runner that tables per-family divergence between a backend and the
  in-process poppler path over the scorecard PDFs + `~/parser-failed-docs`.
  Gate: differential report exists and text/raster parity is quantified.
  The harness lands here, not later: it is the acceptance instrument for
  every subsequent milestone.
- **M2: grpc-pdfium tiers 1-2.** Gate: every family it claims in `Probe` is
  exercised by a fixture test; families it cannot fill are declared absent,
  never empty.
- **M3: grpc-qparse, tier 0 then its uniques** (cells, shapes, embedded
  fonts, deep resources). Gate: joins the differential; its cell output is
  compared against gRParse's fold input expectations.
- **M4: grpc-poppler** (core/glib API, optional compose profile, GPL-3
  labeled). Gate: joins the differential; three-way divergence table runs.
- **M5: gRParse backend-client mode.** `GRPARSE_PDF_BACKEND` selects
  in-process or a service target; scorecard + latency gate run in both modes;
  the raster-over-the-wire cost at model DPI is measured and recorded.
- **M6: the flip (separate decision).** If a permissive backend holds the
  truth floors and the latency verdict is accepted: default flips, poppler
  linkage leaves gRParse, default stack is Apache-only. grpc-poppler stays
  as the differential instrument.

M1/M3/M4 are independent of each other after M0 and can run as parallel
lanes; M2 can trail M1 while M3 starts. M5 needs any one service at tier 0.
M6 is gated on evidence, not scheduled.

## Cross-engine consensus (added 2026-09-04)

Interchangeable backends made a new instrument possible: reading the same
document through several engines and reconciling. `eval/consensus` is the
measured prototype (truth-scored by the scorecard metrics) and
`src/consensus_page_source.cpp` the production form
(`GRPARSE_PDF_BACKEND` with a comma-separated target list; per-page
bigram-agreement vote). Measured on the truth documents: the vote finds
the truth-perfect reading order (in-process poppler alone: 0.800 on
two-column); the merged outline (embedded outlines + numbering + font
tiers) reads 1.000 on text, precision and levels; a table grid from the
qpdf-based service's ruled lines filled with pdfium word cells reads cell
F1 1.000 where the CV path reads 0.286; and a bidirectional word
alignment keeps consensus-offset to per-parser-offset maps with
deviations annotated (order breaks, missing words, same-place text
disagreements such as soft-hyphen artifacts). Winners are labeled
`protomolt`, per-parser values under their parser names, the shape the
claims/field_sources convention wants. Consensus sections drive chunk
boundaries, the path toward centroid-ready chunking. Next tuning target
from the measurements: the fold's own reading-order pass, which is where
two-column's residual 0.815 comes from once the source order is correct.

## License end state

The milestones above ARE the migration: after M6 the default stack ships no
GPL, gRParse's image is Apache-only, and grpc-poppler survives as an optional,
labeled differential instrument off the release path. Until M6, nothing in
gRParse changes and the in-process poppler path remains the baseline truth.

Open questions

- Whether gRParse ultimately dials grpc-pdfium or links the MIT engine
  directly for text and keeps pdfium only for rasters. Direct linkage avoids
  the per-page raster hop; the differential data from steps 2-4 decides.
- Whether struct-tree reading order should feed the layout models as a hint
  channel. Nothing in the current pipeline consumes it; carrying it in the
  contract is cheap, wiring it into the fold is its own project.
