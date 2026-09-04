# PDF backend services: capability matrix and service plan

Status: design, 2026-09-05. Nothing here changes gRParse yet. The in-process
poppler path stays the baseline until a service backend passes the scorecard
and the S3 battery at parity; only then do we consider removing the linkage.

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
| outline / bookmarks | yes | not exposed | yes (`fpdf_doc`) |
| hyperlinks | yes | yes | yes |
| annotations | core/glib yes (cpp no) | partial (qpdf annots) | yes, rich (`fpdf_annot`) |
| form fields | core/glib yes (cpp no) | yes (widgets) | yes incl. interactive (`fpdf_formfill`) |
| attachments / embedded files | yes | via qpdf, not surfaced | yes (`fpdf_attachment`) |
| digital signatures | no | no | yes (`fpdf_signature`) |
| embedded JavaScript listing | core aware | no | yes (`fpdf_javascript`) |
| document metadata / XMP | yes | partial | yes (`FPDF_GetMetaText`) |
| encryption info / passwords | yes | yes (qpdf is strong here) | yes |
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

## Migration path to an Apache-only default stack

1. Land the proto contract (protos repo) and the capability matrix (this doc).
2. grpc-pdfium service + differential leg vs the in-process poppler path.
3. grpc-qparse service joins the differential.
4. grpc-poppler service (optional profile) so the differential covers all
   three even after step 6.
5. gRParse grows `GRPARSE_PDF_BACKEND`: `inprocess` (default, unchanged) or a
   backend target. Measure the service hop honestly: scorecard latency gate,
   plus a rasters-over-the-wire cost run at model DPI.
6. When a permissive backend holds parity on truth floors and the latency gate
   passes (or the regression is accepted), flip the default and remove the
   poppler linkage from gRParse. gRParse's shipped image is then Apache-only;
   grpc-poppler remains as a differential instrument.

Open questions

- Whether gRParse ultimately dials grpc-pdfium or links the MIT engine
  directly for text and keeps pdfium only for rasters. Direct linkage avoids
  the per-page raster hop; the differential data from steps 2-4 decides.
- Whether struct-tree reading order should feed the layout models as a hint
  channel. Nothing in the current pipeline consumes it; carrying it in the
  contract is cheap, wiring it into the fold is its own project.
