# Oracle comparisons

Opt-in batteries that score gRParse against an external reader on a real
corpus. Nothing here runs in the CI gate; each script exits 77 (the CTest
skip code) when its inputs are not configured, so a green build never
means one of these ran.

## compare_vlm.py: an open vision-language model as oracle

Asks a running gRParse for markdown and an OpenAI-compatible vision
endpoint for markdown page by page, then scores agreement (letter
similarity, word recall/precision, heading and table-row counts) and
records timing and tokens per second. The endpoint is whatever serves the
model; `grpc-vlm-convert/serving/north-micro-vision/` serves Cohere Labs'
North Micro Vision Instruct (Apache 2.0) on NVIDIA, Intel XPU or CPU, so
the same corpus can be run once per accelerator with `EVAL_LABEL` naming
the leg.

```sh
uv run --with grpcio --with grpcio-tools \
  env GRPARSE_TARGET=localhost:50051 VLM_ENDPOINT=http://localhost:8086 \
      EVAL_CORPUS=/path/to/pdfs EVAL_LABEL=cuda \
  python eval/compare_vlm.py
```

Outputs land in `eval/out/<label>/`: the two markdowns per file,
`report.json`, and `report.md`. gRParse's gRPC must be reachable from the
host (`compose.stack.expose-grpc.yaml` publishes it); the Intel leg of
gRParse itself is `compose.stack.openvino.yaml`.

The oracle is not ground truth. High agreement says both read the page the
same way; low agreement says where to look, and the two markdowns are kept
side by side for exactly that.

## Scorecard: structural regression against a baseline, correctness against truth

`eval/scorecard/` scores what a running gRParse produces for a fixed corpus
two ways: against summaries committed under `eval/scorecard/baseline/` (drift
from the last recorded run) and against hand-written ground truth under
`eval/scorecard/truth/` (distance from what the source document says). The
corpus is `eval/scorecard/corpus.json`: fixtures under `tests/golden/corpus/`
(docx, xlsx, pptx, pdf, html, md, eml, xml, epub, png) plus external files
referenced by absolute path and never copied in (`EVAL_EXTERNAL_CORPUS=<dir>`
redirects where they are looked up). An external file that is missing is
reported as skipped, never as a pass.

```sh
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py             # score
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --repeat 2  # score + stability
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record \
    --reason "why the baseline moved"                                          # re-record summaries and gates
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record-floors \
    --repeat 2 --only <doc-id>                                                 # gates only, summaries untouched
EVAL_REQUIRE=1 uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py   # pre-release: no skips
uv run python eval/scorecard/tests/run_tests.py                                 # metric unit tests
```

`GRPARSE_TARGET` (default `localhost:50051`) and `EVAL_LABEL` (default
`live`) name the leg; outputs land in `eval/out/scorecard/<label>/`:
`report.md` (one row per document plus a truth table), `report.json`,
`summaries/<doc-id>.json` (the same projection the baseline holds) and
`markdown/<doc-id>.md`.

Exit codes: `0` every scored document is within tolerance, `1` at least one
regression (or, under `--require`, anything skipped), `77` (the CTest skip
code) when gRParse is unreachable, grpcio is not importable, or the corpus
resolves to nothing.

### Baseline metrics (drift)

Per document the summary keeps the ordered reading sequence (label, level,
normalized text and its hash), the heading hierarchy, every table grid with
spans, each picture with its parent group and preceding heading, the group
tree, counts, harness-derived warnings and a cross-collector agreement section
from `claims` and `field_sources`. Image bytes are reduced to a length and a
digest. The metrics and their tolerances are constants in
`eval/scorecard/scoring.py`, one rationale each: text similarity, reading-order
edit similarity, table cell F1 and structure match, heading score, picture
placement, new-warning count and field-winner agreement.

The rule: a regression blocks a change unless the baseline is shown to be
wrong. In that case the baseline is re-recorded in the same change
(`--record --reason "..."`), and the reason lands in the report notes and in
`baseline/_meta.json`.

### Truth metrics (correctness)

A baseline only says "different from yesterday". `eval/scorecard/truth/<doc-id>.json`
says what the document actually contains, derived from the source and never
from parser output, and `eval/scorecard/truth_metrics.py` scores the live
summary against it on an absolute scale where 1.0 means "matches the source":

| metric | what it measures |
|---|---|
| `truth_headings` | F1 of truth headings found among live headings (normalized prefix match either way) |
| `truth_heading_levels` | share of the matched headings whose level is exact |
| `truth_order` | longest in-order run over the anchors located in the full reading text |
| `truth_anchors_found` | share of anchors present anywhere in the reading text; the detail line adds how many still open a paragraph |
| `truth_table_cells` | F1 over the sampled cells (right text at the right row and column; the detail adds text-anywhere for a shifted grid) |
| `truth_figures` | share of figures whose picture sits after its anchor text and before the next anchor |

To author a truth file, open the source, not the output: docx/pptx/xlsx are
zip plus XML (`unzip -p`, or `uv run --with python-docx`, `--with python-pptx`,
`--with openpyxl`), PDFs give up their text with `pdftotext -layout` and their
pages with `pdftoppm` (`tesseract` for a scan), markup is read as is. Write:

- `headings`: `[{"text", "level"}]` in order. Level 0 is a title element
  (docx Title style, html/epub `<title>`, a deck's title slide); every other
  heading carries its nominal source level (`<h2>` and `## ` are 2, docx
  Heading 2 is 2, a numbered `1.1` is 2).
- `anchors`: 10 to 40 short unique snippets, the opening words of paragraphs,
  in the order they must be read; make them cross columns and pages so a
  column-major or interleaved read shows as a break. Furniture (running
  headers, page numbers, footnotes) is never an anchor.
- `tables`: `[{"table": <hint>, "cells": [[row, col, "text"], ...]}]`, a handful
  of cells per table, zero-based, a span named by its top-left cell, an empty
  string for a cell that must survive empty. The hint is documentation; each
  truth table is paired with the live table that hits most of its cells.
- `figures`: `[{"after": "<anchor of the paragraph the figure follows>", "caption": "..."}]`.
- `source` (one line naming the derivation) and `notes` (how each list was read).

Keep it small and certain: a fact you are not sure of is left out, not guessed.

### Gates: floors, latency, stability

`baseline/_meta.json` holds a `gates` row per document. Truth scores are
recorded there as floors the first time they are recorded (`--record`, or
`--record-floors` to leave the summaries alone); a scoring run fails a document
when any truth metric drops more than 0.02 below its floor. Floors are a
ratchet: a record raises a floor when live is higher and never lowers one
silently. Lowering needs `--reason`, and the change (before, after, reason)
is appended to the meta history. The report shows both columns, "vs baseline"
and "vs truth", and the truth table prints the absolute score next to its
floor so a reader sees how far from 1.0 each document still is.

Latency is scored by default: a document fails when its median `elapsed_ms`
exceeds `max(baseline * 1.25, baseline + 500 ms)`. The baseline is the median
of the runs at record time (one run unless `--repeat N`). `EVAL_LATENCY=off`
(or `--no-latency`) disables the gate; developers scoring a CPU-only dev
instance against a CUDA-recorded baseline run with it off.

Stability: `--repeat N` (default 1) converts each document N times and
compares the summaries; the report says `stable`/`unstable` with the first
differing field. Recording with `--repeat 2` stores the observed stability, so
a known-unstable document is a known row that a later fix flips to stable
(re-record to lock it), while a stable document going unstable is a regression.

Memory is informational only: when `EVAL_METRICS_URL` (or, for a local target,
the `parse-stack-grparse-1` container's `:9464/metrics`) exposes
`process_resident_memory_bytes`, the report notes the RSS before and after the
run; when it does not, the note says so.

### Zero-skip (pre-release) mode

`EVAL_REQUIRE=1` or `--require` turns every skip into a failure with a message
naming what was missing: the external corpus file, the unreachable service,
the missing grpcio. A release battery runs the scorecard in this mode so a
green result cannot be an empty one.

### Generated fixtures

`eval/scorecard/fixtures/build_all.sh` regenerates the authored fixtures into
`tests/golden/corpus/` (LibreOffice, poppler, ImageMagick and `uv` needed):

| fixture | doc id | generator | exercises |
|---|---|---|---|
| `two-column.pdf` | `pdf-two-column` | `two_column_pdf.py` (Flat ODT from `gatsby_excerpt.txt`, `soffice` to PDF) | two columns, two-line title, running header, page-number footer, two footnotes, numbered headings, hyphenated line ends |
| `rotated-scan.pdf` | `pdf-rotated-scan` | `rotated_scan.sh` (page 1 above, 150 dpi grayscale, rotated 90 degrees) | OCR of a rotated scan, no text layer |
| `notes.pptx` | `pptx-notes` | `pptx_notes.py` | title slide, two-level bullets, a table, speaker notes on three slides |
| `figures.docx` | `docx-figures` | `docx_figures.py` (PNGs drawn with `convert`) | two captioned figures anchored mid-body under numbered headings |
| `sixty-sheets.xlsx` | `xlsx-sixty-sheets` | `xlsx_sixty_sheets.py` | 60 sheets, a merged three-column title over a bold header row, a 40-column sheet |
| `form.docx`, `form.pdf` | `docx-form`, `pdf-form` | `docx_form.py` (docx, then `soffice` to PDF) | label/value table with empty cells and ballot-box checkboxes |
| `charts.xlsx` | `xlsx-charts` | `xlsx_charts.py` (data in `chart_data.py`; also paints the charts to `eval/chart_derender/renders/`) | three sheets, each a data table plus one chart: two-series column chart with title and axis titles, one-series line chart with a title, pie chart with no title |
| `charts.pptx` | `pptx-charts` | `pptx_charts.py` (data in `chart_data.py`) | title slide plus three titled chart slides: two-series column chart, pie chart with a title, line chart with no chart title |

`long-text.pdf`, `mixed.pdf` and `scanned-image.pdf` are copied from
`grpc-pdf-inspector/demos/sample-data/`, which generates them itself
(`node-client`, `npm run samples`). Gatsby prose is public domain (Project
Gutenberg transcription); every other fixture text was written for the
fixture. Fixtures stay under 2 MB each.

Metric and summary code lives outside the service; the CTest registration
(`LABELS eval`, `SKIP_RETURN_CODE 77`, like `vlm-oracle-eval`) is a
`CMakeLists.txt` change made alongside the runner.

The two chart fixtures and their truth files quote one data module, so the
truth cannot drift from the fixture; both generators pin document
properties and rewrite the package (and, for the deck, each chart's
embedded workbook) with fixed zip timestamps, and repeat runs are
byte-identical. `eval/chart_derender/` holds the enrich-side evidence for
the opt-in chart derender leg (`compare.py`, its README with the measured
numbers per VLM endpoint); it dials grpc-enrich, not gRParse.
