# Cross-engine consensus

Research instrument (python, like the scorecard it borrows its judges
from): with several typed parsers behind the PdfBackend contract, a second
and third reading of the same document plus light NLP corrects what any
single engine gets wrong. Judged end to end by the scorecard's own truth
metrics, so a win here is a win on the production gate.

## What it does

- **Reading order**: candidate word sequences from each backend (poppler
  emission order, pdfium content order, qparse sanitized order) plus
  `pdftotext -layout` as the simple text leg. Each candidate is scored by
  bigram agreement (how many of its adjacent word pairs the other legs
  also emit adjacently) with a sentence-continuity tiebreak; the winner is
  the consensus order. Measured: the vote picks the order that scores
  1.000 on the truth anchors for both truth documents, including
  two-column where the in-process path scores 0.800 (see
  RESULTS-2026-09-04.md).
- **Outline**: merges the embedded outlines of every backend with
  font-size-derived heading lines from the cell stream. Depth resolves by
  signal strength: section numbering in the title, then embedded outline
  nesting (anchored to the font tiers, since outline depth is relative and
  often starts below the document title), then the font tier. Measured:
  recall, precision, and levels all 1.000 on both truth documents.
- **Sections and chunks**: the consensus outline positions split the
  winning reading text into sections; sections chunk at word boundaries.
  This is the seam toward centroids: chunk boundaries come from consensus
  structure instead of raw text length.
- **Tables**: for ruled forms, a grid from qparse's vector shapes
  (horizontal and vertical rules, kept only where the two directions
  cross) filled with pdfium word cells, read line by line inside each
  cell, with empty grid positions emitted as real blank cells. Measured:
  cell F1 1.000 against the form truth, where the in-process CV path
  scores 0.286.
- **Annotation model**: each outline node carries the winning value under
  `protomolt` and every source's version under its own parser name. This
  is a prototype-JSON sketch of the direction the platform's typed
  claims/field_sources convention points; wiring these winners and losers
  into CollectorClaim/field_sources (typed, not keyed strings) is its own
  follow-up and has not been built.

## Run

```bash
# the three backends listening (grpc-pdfium, grpc-qparse, grpc-poppler)
uv run --with grpcio --with grpcio-tools python eval/consensus/prototype.py \
  --targets localhost:51241 localhost:51242 localhost:51243
```

Writes `out/report.md` and one annotated JSON per document.

## Production counterpart

The reading-order vote ships in C++ in the fold: `GRPARSE_PDF_BACKEND`
accepts a comma-separated target list, and with more than one target the
PDF layer fetches cells from every backend and returns the page whose
order wins the bigram vote (src/consensus_page_source.cpp). Outline,
section chunking, and the table grid are wired next, behind the same
measurements.

Embeddings stayed out of scope: geometric matching reconciled locations
on its own everywhere this corpus needed it.

## Bidirectional reconciliation

The correlations the vote computes are kept, not thrown away: for every
word in the consensus base stream, the alignment stores its consensus
index and character offset next to the matched index and offset in each
other word-granularity source (`out/<doc>.alignment.json` holds the
complete map, the per-document JSON the summary), so either side can look
up the other. Deviations are annotated where sources disagree: order
breaks, counted as regressions in the source's order so an inserted word
is not misread as reordering (85 on two-column, the column-order
divergence sites), missing words, and same-place text disagreements (92
on two-column), which are correction sites: soft-hyphen artifacts,
footnote markers fused to words, hyphenated-token splits. Each source's vote score rides along as its
weight, which is the extension point for further structure sources: a
markdown converter or a language model's perceived outline joins as one
more weighted source with no change to the model.
