# Backend client mode: scorecard both ways, 2026-09-04

Setup: the same locally built gRParse (CPU execution provider, models from
the repo mount) served twice, one process with the in-process poppler
path and one with `GRPARSE_PDF_BACKEND=localhost:51241` (grpc-pdfium,
word-level cells from loose char boxes). Scorecard `--only pdf --repeat 2`
against each. Baselines were recorded on the deployed OpenVINO/CUDA
stack, so verdict columns regress identically on both legs from the CPU
models; the measurement is the delta between the legs, not the verdicts.

## Leg vs leg (text / order per doc)

| doc | in-process | grpc-pdfium backend | delta |
|---|---|---|---|
| pdf-dummy | 1.000 / 1.000 | 1.000 / 1.000 | none |
| pdf-hello-text | 1.000 / 1.000 | 1.000 / 1.000 | none |
| pdf-long-text | 1.000 / 1.000 | 1.000 / 0.846 | order, one phantom heading |
| pdf-mixed | 1.000 / 1.000 | 1.000 / 1.000 | none |
| pdf-scanned-image | 1.000 / 1.000 | 1.000 / 1.000 | none (OCR path) |
| pdf-two-column | 0.983 / 0.902 | 0.900 / 0.815 | slightly behind |
| pdf-diffusion-paper | 0.826 / 0.017 | 0.792 / 0.028 | mixed, both far off the CUDA baseline |
| pdf-form | 0.955 / 0.444 | 0.944 / 0.556 | order ahead; table cells 0.917 vs 0.286 and structure 1.000 vs 0.000, both ahead |
| pdf-rotated-scan | 1.000 / 1.000 | 0.998 / 1.000 | raster nudge into OCR |
| pdf-rotated-scan-180 | 1.000 / 1.000 | 0.998 / 1.000 | raster nudge into OCR |
| pdf-rotated-scan-mixed | 0.994 / 0.976 | 0.938 / 0.684 | digital+OCR merge on the mixed page |

Wall clock for the 11-doc run: in-process 100.5s, backend 81.9s. The
backend leg is not slower; on this host the engine's rasterizer speed
covers the localhost hop.

## Raster over the wire at model DPI

From `raster-cost.md` (DPI 200, full-document Render, BGR8): grpc-pdfium
lands within a factor of two of the in-process poppler render on each
document and ahead of it on several, at ~10.7 MB per Letter page on the
wire; grpc-qparse ships RGBA8 at ~14.3 MB per page. Every request also
re-sends the document bytes, the price of the stateless contract.

## The cell-granularity lesson

Two grpc-pdfium revisions were measured on the way here:

1. Line-level cells (text-page rects): differential text parity looked
   fine, and the fold regressed (headings and order shifted on
   born-digital docs). The fold's heuristics are tuned to word boxes.
2. Word-level cells from tight ink rects: word counts matched poppler
   and the fold got worse; tight boxes give "page" a lower top than "1",
   and geometry sorts scrambled words within a line.
3. Word-level cells from loose (font-metric) char boxes: uniform vertical
   extent per line, and the fold snapped into agreement above.

The rule this leaves: a backend's `TextCell` geometry must be
font-metric boxes at word granularity, not glyph-ink unions. The
differential alone did not catch revisions 1 and 2; only the scorecard
leg did, which is why M5 runs both.

## Consensus mode (added later the same day)

Serving the same build with
`GRPARSE_PDF_BACKEND=localhost:51241,localhost:51242,localhost:51243`
(the consensus page source voting across all three backends per page)
ran the same subset in 96.9s. Results match the single grpc-pdfium
column on the gap documents and improve diffusion-paper text (0.812 vs
0.792), with some pages taken from other backends. The two-column
residue is now located: the source word order scores 1.000 on the truth
anchors (eval/consensus), so the remaining 0.815 comes from the fold's
own reading-order pass, which is where the next tuning work belongs.

## M6 evidence status

Held: load, inventory, fonts, scans, most born-digital docs, latency.
Not yet held: reading order on long-text (0.846) and two-column (0.815
vs 0.902), and the digital+OCR merge on rotated-scan-mixed. The flip
stays gated until those close; the form-document table win suggests some
gaps are fold tuning rather than engine quality.
