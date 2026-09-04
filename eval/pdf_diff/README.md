# PDF backend differential

Tables per-family divergence between a PdfBackend service (grpc-pdfium
first) and the in-process poppler path, over the scorecard PDFs plus
`~/parser-failed-docs` as load-status cases. This is the acceptance
instrument for the PDF backend milestones (see
`docs/pdf-backend-services.md`): every backend that wants into the fleet
joins this table.

## Legs

- **poppler**: `poppler_floor` (built here by `build_floor.sh` against the
  host's libpoppler-cpp) replays the exact poppler-cpp calls
  `src/in_memory_document.cpp` makes: `load_from_raw_data`,
  `text_list(text_list_include_font)`, `page_renderer` BGR24 at a DPI, and
  the quarter-turn orientation handling. It links GPL poppler and stays in
  `eval/`, off every release artifact. The host poppler version is recorded
  in the report; the gRParse image vendors its own, so treat raster deltas
  as indicative rather than exact.
- **service**: dials a running backend over `ai.pipestream.parse.pdf.v1`,
  with python stubs generated from the sha256-pinned pipestream-protos
  release tarball.

## Run

```bash
# terminal 1: the backend under test
GRPC_PDFIUM_PORT=51234 ./grpc_pdfium

# terminal 2
PDF_DIFF_TARGET=localhost:51234 uv run \
  --with grpcio --with grpcio-tools --with numpy \
  python eval/pdf_diff/runner.py --dpi 72
```

Output: `out/report.md` (the table) and `out/metrics.json` (everything).

## Reading the table

- **load / inventory**: AGREE means both legs load the document and agree
  on page count, per-page size (1% tolerance) and quarter-turn state.
- **text sim min/mean/bag**: per-page normalized text compared in order
  (SequenceMatcher) and as an order-insensitive character bag. High bag
  with low ordered similarity = the same text in a different reading order
  (verdict REORDERED), the known engine difference: poppler applies layout
  heuristics, pdfium emits content-stream order. Low bag = text is
  actually missing somewhere (DIVERGE).
- **cells**: granularity, not parity. poppler emits word boxes, pdfium
  line-level rects; the counts differ by design.
- **raster raw/aligned**: mean absolute pixel difference as-is and after
  the best one-pixel shift. Rotated pages land about a pixel apart between
  the renderers, so a raw ~20 that halves under alignment is a rounding
  offset plus resampling, not different content. Dense text at low DPI
  keeps a high mean from antialiasing differences alone.

Divergence is the diagnostic, not a failure: the report quantifies parity
and the milestone gates judge it.
