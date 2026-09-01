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

## Scorecard: structural regression against a committed baseline

`eval/scorecard/` scores what a running gRParse produces for a fixed corpus
against summaries committed under `eval/scorecard/baseline/`. The corpus is
`eval/scorecard/corpus.json`: small fixtures copied into
`tests/golden/corpus/` (docx, xlsx, pptx, pdf, html, md, eml, xml, epub, png)
plus external files referenced by absolute path and never copied in
(`EVAL_EXTERNAL_CORPUS=<dir>` redirects where they are looked up). An external
file that is missing is reported as skipped, never as a pass.

```sh
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py             # score
uv run --with grpcio --with grpcio-tools python eval/scorecard/run.py --record \
    --reason "why the baseline moved"                                          # re-record
uv run python eval/scorecard/tests/run_tests.py                                 # metric unit tests
```

`GRPARSE_TARGET` (default `localhost:50051`) and `EVAL_LABEL` (default
`live`) name the leg; outputs land in `eval/out/scorecard/<label>/`:
`report.md` (one row per document), `report.json`, `summaries/<doc-id>.json`
(the same projection the baseline holds) and `markdown/<doc-id>.md`.

Exit codes: `0` every scored document is within tolerance, `1` at least one
regression, `77` (the CTest skip code) when gRParse is unreachable, grpcio is
not importable, or the corpus resolves to nothing.

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
`baseline/_meta.json`. Metric and summary code lives outside the service; the
CTest registration (`LABELS eval`, `SKIP_RETURN_CODE 77`, like
`vlm-oracle-eval`) is a `CMakeLists.txt` change made alongside the runner.
