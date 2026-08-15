# gRParse

C++ gRPC document parse service: **diskless PDF/image to page-streamed protobuf** with boxes and stable offsets. RapidOCR and PicoDet layout run through **ONNX Runtime** on NVIDIA GPUs (CUDA) or Intel GPUs (OpenVINO). Layout labels, reading order, table items with geometry-derived cell grids, and picture items are live; model-based table spans and figure classification are roadmap work.

- Architecture (runtime split, anti-seesaw pipeline, offset contract): [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Epics & tasks (C++ vs Java ownership, milestones): [docs/EPICS.md](docs/EPICS.md)

**Speed thesis:** pipelined pages, warm ORT session pools, selective OCR, and early page emission keep CPU and GPU busy.

gRParse turns PDF pages and raster images into text with the maintained C++ [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) implementation. It targets NVIDIA CUDA through ONNX Runtime. The host was detected with an NVIDIA GeForce RTX 4080 SUPER; the included container exposes it with Compose's `gpus: all` setting.

## Run

1. Download the four model files listed in [models/README.md](models/README.md).
2. Build and start the service:

   ```bash
   docker compose up --build
   ```

The service listens on `localhost:50051` and implements `ai.pipestream.parse.v1.ParseService` from the local `parse.proto` contract. `ConvertSource` currently accepts one `FileSource` containing base64-encoded PDF, PNG, JPEG, or TIFF bytes. It renders every `OutputFormat` the wire declares from the merged document: TEXT, MARKDOWN, HTML, HTML_SPLIT_PAGE, JSON, YAML, DOCTAGS, DOCLANG, and VTT (an empty `to_formats` keeps the plain-text default alone), and returns `INVALID_ARGUMENT`, naming the offender, for populated options it does not implement and for unrenderable format values.

Each PDF request opens a small pool of Poppler documents directly from the request bytes, so render and digital-text extraction for different pages of the same document proceed in parallel. Recognition is selective by default: full native-text pages skip raster OCR, while weak/partial digital layers keep their native boxes and still run OCR, and geometry merge drops overlapping OCR duplicates so headers and scan body can coexist. Two `ConvertDocumentOptions` fields override the default per request: `do_ocr = false` disables recognition entirely, so only the embedded text layer is read and a page with no text layer yields no text; `force_ocr = true` recognizes every page at full-page scope and the recognized text replaces the embedded layer. `do_ocr = false` with `force_ocr = true` is contradictory and rejected by name. Pages rasterize at 200 DPI by default; `render_scale` sets a per-request scale in multiples of 72 DPI (accepted range [1.0, 8.0], rejected outside it by name), and all digital-line geometry scales with it so downstream boxes stay consistent. Raster inputs decode with OpenCV from request memory and are already pixels, so they ignore `render_scale`. Nothing is written to disk on the hot path.

`ConvertSource` returns the contract's `ConvertDocumentResponse`, populated with a native `Document`. Each OCR line becomes a `TextItem`, with its page and bounding box in `provenance`; pages, `TableItem`/`PictureItem` entries from layout, and the `#/body` reference graph are also populated. It deliberately leaves semantic chunking, asynchronous jobs, and remote sources unimplemented.

The `Health` RPC reports readiness. The server intentionally fails at startup if a model is absent or CUDA initialization fails, instead of silently running CPU OCR.

To stream a PDF with the supplied client, start the service and run:

```bash
docker compose up -d
./scripts/parse_pdf.sh /path/to/document.pdf
```

The helper invokes the compiled bidirectional-streaming client. It reads the
source and sends fixed-size chunks directly to gRPC; it does not base64-encode
the document or create temporary files.

### Examples: browser demo and other-language clients

[`examples/`](examples/README.md) holds working consumers of the streaming
contract: a browser demo that paints each page event live (boxes, reading-order
text, tables, picture classes, barcodes) plus terminal clients in Python and
Go. The demo runs with the service in one command:

```bash
docker compose --profile demo up --build   # service on :50051, demo on :8080
```

The page ships a bundled two-page sample (digital text + table on page 1, a
scanned-style OCR page with a classified figure and a decodable QR code on
page 2), so one click exercises every feature with no document at hand, and
the full parse result downloads as JSON when the stream completes:

![The web demo after parsing the bundled sample: streaming stats, provenance boxes, table grid, figure classes, and the decoded QR payload](docs/images/web-demo.png)

### Runtime image

The runtime stage is minimal-base compatible: it runs no package manager and
no ldconfig, ships its complete non-CUDA shared-library closure from the
build stage (cuDNN included), runs as the numeric non-root user 65532, and
asks the base only for glibc and the CUDA runtime libraries. The default base
is `nvidia/cuda:13.3.0-runtime-ubuntu26.04`; a hardened base such as a Docker
Hardened Images `nvidia-cuda` mirror drops in without a Dockerfile change:

```bash
docker build --build-arg GRPARSE_RUNTIME_IMAGE=docker.io/<org>/dhi-nvidia-cuda:<tag> .
```

The tag's CUDA major version must match the build stage (CUDA 13) and its
glibc must be at least ubuntu26.04's. The compose file runs the container
read-only with a tmpfs `/tmp`, all capabilities dropped, and privilege
escalation disabled.

## Page-streaming OCR

`ai.pipestream.parse.v1.ParseStreamingService/StreamProcessDocument` accepts a
stream of `DocumentChunk` messages. Send the same `document_id`, filename, and
content type with the chunks, then set `complete = true` on the last one. The
server accepts PDFs and single raster images, up to 50 MiB. The chunk fields
`do_ocr`, `force_ocr`, and `render_scale` carry the same recognition mode and
rasterization scale as the unary options, each resolved from the first chunk
that sets it (the same doctrine as `collectors`); an invalid value fails the
stream with `INVALID_ARGUMENT` naming the offender.

It emits one `DocumentStreamEvent.page` per page in page-number order, followed by one
`DocumentStreamEvent.complete`. A page event contains the supplied
`PageItem` and the page's supplied `BaseTextItem` records. `TextOffset` carries
append-only UTF offsets, source type, and OCR confidence when available. The original
`Document` shape is unchanged: this is only a transport envelope for
incremental delivery.

Each outbound event and its nested protobuf messages are allocated in a
short-lived `google::protobuf::Arena`. The arena stays alive until the
asynchronous gRPC write completes. Protobuf Arena does not own Poppler, OpenCV, or ONNX Runtime buffers;
those libraries release their own in-memory buffers at the page boundary. The
server never writes input documents, rendered pages, OCR intermediates, or
results to disk. It only reads the installed binaries and OCR model files. The
server has globally bounded document, render, inference, and assembly queues.
RapidOCR inference workers never perform gRPC writes. A stream that does not
consume events stops returning page credits to the scheduler, so that document
cannot advance beyond its configured page window. Other admitted documents can
continue through the global queues, and a stalled document keeps its scheduler
state until the client's credits return or the stream ends. The reactor's
in-flight write buffer is sized from `GRPARSE_PAGE_WINDOW`, so raising the page
window raises both bounds together.

The server has two CUDA RapidOCR sessions by default. Tune concurrency and
queue memory with `GRPARSE_PAGE_WORKERS`, `GRPARSE_RENDER_WORKERS`,
`GRPARSE_ASSEMBLY_WORKERS`, `GRPARSE_DOCUMENT_QUEUE`, `GRPARSE_RENDER_QUEUE`,
`GRPARSE_INFERENCE_QUEUE`, `GRPARSE_ASSEMBLY_QUEUE`, `GRPARSE_PAGE_WINDOW`,
`GRPARSE_PDF_PARSERS`, and `GRPARSE_MAX_ACTIVE_DOCUMENTS`.
`GRPARSE_PDF_PARSERS` sets how many Poppler documents a single PDF request may
open concurrently; it defaults to `GRPARSE_RENDER_WORKERS` and costs one parsed
document structure per slot. Select the NVIDIA device with
`GRPARSE_CUDA_DEVICE`. `GRPARSE_ORT_EP` picks the ONNX Runtime execution
provider: `cuda` (default, fails startup if CUDA cannot initialize),
`openvino` (Intel GPU/CPU/NPU through the OpenVINO build — see below), `cpu`
(explicit CPU inference), or `auto` (prefers CUDA, then OpenVINO, then CPU,
logging each fallback). Requesting a provider the linked ONNX Runtime does not
offer fails with the list that is actually available. An OCR session that
throws during inference is destroyed and rebuilt on next use instead of
staying in the pool poisoned. Optional RapidOCR detect knobs:
`GRPARSE_OCR_PADDING`, `GRPARSE_OCR_MAX_SIDE`, `GRPARSE_OCR_BOX_SCORE`,
`GRPARSE_OCR_BOX_THRESH`, `GRPARSE_OCR_UNCLIP`. These are read and range-checked
once at startup: a malformed or out-of-range value fails the server immediately
rather than being silently ignored per page. gRPC memory, thread,
and stream limits use `GRPARSE_GRPC_MEMORY_MIB`, `GRPARSE_GRPC_MAX_THREADS`,
and `GRPARSE_MAX_CONCURRENT_STREAMS`.

When `models/layout_publaynet.onnx` is present (see
[models/README.md](models/README.md)), every page also runs PicoDet layout
detection on the configured execution provider: text lines inside title and
list regions are labelled `TITLE`/`LIST_ITEM`, and table and figure regions
are emitted as `TableItem`/`PictureItem` entries with provenance boxes so
downstream table and picture extraction have crops to work from. Text streams
in reading order: a recursive XY-cut over layout regions (or the lines
themselves when no model is present) splits pages at the widest whitespace
gap, so multi-column pages read column by column instead of interleaving
rows, and UTF offsets follow that order. Control it
with `GRPARSE_LAYOUT=auto|on|off` (`auto`, the default, enables layout when
the model file exists and says so at startup; `on` fails startup if the model
is missing). Full-digital pages are still rasterized when layout is active,
but continue to skip OCR.

Every `TableItem` additionally carries cell structure in `data`. When
`models/slanet_plus.onnx` is present (`GRPARSE_TABLE_STRUCTURE=auto|on|off`,
same contract as layout), SLANet-plus runs on each detected table crop and
supplies the real grid: cell spans, `<thead>` rows as `column_header`, and
model cell boxes, with text lines bound to cells by box center. Without the
model, a geometry fallback clusters the table's text lines into row bands by
vertical overlap and columns by merging horizontal spans (a gap wider than
about half the median line height counts as a column gutter) with unit spans
and no header flags. Both paths give `num_rows`/`num_cols`, a rectangular
`grid`, and per-cell text with bounding boxes. Table interior text still
streams as ordinary `TEXT` items too, so UTF offsets stay contiguous for
clients that ignore tables.

When `models/figure_classifier.onnx` is present (`GRPARSE_FIGURE_CLASSES`,
same auto/on/off contract), each figure crop also runs the MIT-licensed
DocumentFigureClassifier (EfficientNet-B0, 16 classes such as bar_chart,
qr_code, signature, screenshot) and every `PictureItem` carries a
`classification` annotation with the full sorted class distribution, which
is what downstream policy hooks (signature routing, barcode triggers, icon
filtering) key on. The stream never blocks on classification: it is one
batch=1 device call per figure inside the inference stage.

Barcode and QR payloads decode without any model: ZXing is compiled in.
By default (`GRPARSE_BARCODES=auto`) a figure crop is decoded when the
classifier's top class is `bar_code` or `qr_code`; `on` decodes every figure
crop (needs only layout, no classifier); `off` disables it. Each decoded
payload rides on the `PictureItem` as a `misc` annotation with
`kind: "barcode"` and a struct holding `format` (the ZXing symbology name,
for example `QRCode` or `Code128`), `value`, and `provenance`. Decoding is
pure CPU inside the inference stage, after the device calls and before the
raster is released.

With `GRPARSE_PICTURE_IMAGES=on` (default off) each figure region's pixels
are cropped from the page raster in the inference stage, PNG-encoded, and
attached to its `PictureItem` as an `image/png` data URI with the pixel
size. The crop happens after OCR and before the raster is released, so
device work is never delayed and no raster outlives its page. Leave it off
for figure-heavy corpora where embedded images would inflate every page
event; picture bounding boxes are always present either way.

With `GRPARSE_PAGE_IMAGES=on` (default off) every page event additionally
carries a downscaled PNG preview of the page raster on its `PageItem`, so
clients can paint provenance boxes over the real page (the web demo does
exactly this). The preview encodes in the inference stage under the same
rule as figure crops — after the device calls, before the raster drops —
and full-digital pages are rasterized for it even when layout is off. The
preview's pixel size rides its `ImageRef`; the page size stays in the
page's own coordinate space (PDF points for digital pages), same aspect
ratio.

## Collector scatter-gather

gRParse is the coordinator of a set of parser collectors. The in-process CV
path above is one collector (`COLLECTOR_GRPARSE_CV`); the rest are remote
services, each configured with a `GRPARSE_<NAME>_TARGET=<host:port>`
environment variable and left unconfigured otherwise:

| Collector | Target env | Routed by default for |
|---|---|---|
| `COLLECTOR_LIBREOFFICE` | `GRPARSE_LIBREOFFICE_TARGET` | office formats (doc/x, xls/x, ppt/x, odf, rtf, csv, ...) |
| `COLLECTOR_ASR` | `GRPARSE_ASR_TARGET` (+ `GRPARSE_ASR_MODEL`, the whisper model name, required) | audio and video |
| `COLLECTOR_EMAIL` | `GRPARSE_EMAIL_TARGET` | `.eml`, `.msg`, `message/rfc822` |
| `COLLECTOR_XML` | `GRPARSE_XML_TARGET` | `.xml`, `.nxml`, `.xbrl`, `application/xml`, `text/xml` (never the `+xml` suffix family), plus the archive forms `.dclx` and `.tar.gz` (METS/GBS) |
| `COLLECTOR_EBCDIC` | `GRPARSE_EBCDIC_TARGET` | never routed; explicit selection with `ConvertDocumentOptions.ebcdic_layout_json` only |
| `COLLECTOR_EPUB` | `GRPARSE_EPUB_TARGET` | `.epub` |
| `COLLECTOR_MARKUP` | `GRPARSE_MARKUP_TARGET` | text markup: `.md`, `.html`/`.htm`/`.xhtml`, `.adoc`, `.tex`, `.vtt`, `.boxnote`, and `.json` (Docling JSON re-ingest); the dial carries a format hint from the filename, and the collector sniffs when none resolves |
| `COLLECTOR_LOL_HTML` | `GRPARSE_LOL_HTML_TARGET` | never routed; explicit selection with `ConvertDocumentOptions.lol_html_options_json` (the protobuf JSON of `lolhtml.v1.ExtractOptions`) only. Targeted CSS-selector extraction from HTML, not whole-document conversion: matches fold into a group per rule. HTML with no selection routes to `COLLECTOR_MARKUP` |

A request selects collectors explicitly (`ConvertDocumentOptions.collectors`,
or `DocumentChunk.collectors` on the streaming RPC); an empty selection
routes by format as above, with PDF and raster inputs staying on the CV
path. No code path converts office bytes to PDF in order to parse them.

The libreoffice collector streams typed events that gRParse folds into a
`Document` itself, and the lol-html collector's match stream is likewise
folded client-side (its forward-only wire deliberately has no document
event); every other remote collector projects its own typed stream into a
source-tagged `Document` server-side (their `emit_document` option), so
gRParse asks for the Document event, drains the typed events, and merges
what the collector itself attributed. The vendored wire
contracts live in `collectors/` (see its README); each collector repo owns
its contract.

The libreoffice leg is a hybrid: office text, tables, and typed content are
exact from the office core, so gRParse does not OCR office documents — but
the collector's page renders (the `PageImage` PNGs it streams anyway) run
through the same layout, figure-classification, and barcode engines the CV
path uses, sharing its session pools. Detected figures land as additional
source-tagged `PictureItem` entries with their class distributions and
decoded barcode payloads, boxes converted into the document's own
coordinate space — so a chart or QR code inside a DOCX is spotted and
decoded even though the office core cannot see it. The enrichment follows
the same knobs as the CV path: it needs the layout model, and barcode
decoding honors `GRPARSE_BARCODES`.

Every collector's output is an `ai.pipestream.document.v1.Document` whose items carry a
`CollectorSource` tag, and the coordinator merges them additively: item
references renumber, sources never overwrite each other, and choosing a
winner among sources is a downstream concern. On the streaming RPC each
out-of-process collector's document is emitted as a `CollectorDocument`
event the moment that collector finishes, while CV page events keep
streaming; the terminal event lists any `collector_failures`. A failed
collector degrades to an error entry (unary) or a failure entry (stream)
instead of failing the parse while any collector succeeds; the parse fails
only when every selected collector fails.

The server registers standard gRPC health checking and reflection in addition
to the contract's `Health` RPC. SIGINT and SIGTERM initiate a bounded graceful
shutdown.

Every `GRPARSE_METRICS_INTERVAL_SECONDS` (default 60, `0` disables) the server
prints one pipeline metrics line to stdout: document and page counters, queue
depths, per-stage busy percentages since the previous line, OCR session pool
acquire/discard/wait totals, and a page-latency histogram from schedule to
delivery. Render and inference busy percentages climbing together under load
is the pipeline overlap working; one stage pegged while its neighbor idles
identifies where to add workers.

The same counters are available in Prometheus text format: set
`GRPARSE_METRICS_PORT` (default `0`, off) and scrape
`http://<host>:<port>/metrics`. Counters and gauges map one to one with the
stdout line; the latency buckets become a `grparse_page_latency_seconds`
histogram, and per-stage busy time is exported as
`grparse_stage_busy_seconds_total` next to `grparse_stage_workers`, so
`rate(grparse_stage_busy_seconds_total[1m]) / grparse_stage_workers` is the
same busy fraction the stdout line prints. The listener is a minimal
in-process HTTP endpoint (no framework); a configured port that cannot be
bound fails startup loudly. The compose file wires it to `9464`.

## Intel GPUs (OpenVINO)

`Dockerfile.openvino` builds an Intel variant with no CUDA anywhere: ONNX
Runtime 1.24.1 with the OpenVINO execution provider (OpenVINO 2025.4.1 and its
Intel GPU/CPU/NPU plugins, from Intel's prebuilt distribution) plus the NEO
OpenCL compute runtime. It targets Arc discrete cards (Battlemage/Alchemist),
integrated Xe graphics, CPUs, and NPUs:

```bash
docker build -f Dockerfile.openvino -t grparse-openvino .
docker run --rm --device /dev/dri -v /path/to/models:/models:ro \
  -p 50051:50051 grparse-openvino
```

The image defaults to `GRPARSE_ORT_EP=openvino` with
`GRPARSE_OPENVINO_DEVICE=GPU`; set the device to `GPU.<n>`, `CPU`, `NPU`, or
an `AUTO:`/`HETERO:` list. Startup fails loudly if the device cannot
initialize — the host needs `/dev/dri` passed through and a kernel new enough
for the card. Provider selection is centralized in a small patch to the
RapidOcrOnnx session setup (`patches/rapidocr-session-ep.patch`); the server
refuses to start if a stale dependency cache produced an unpatched build, so
the configured provider can never silently degrade to CPU.

The image also includes `grparse-stream-client`, a bidirectional gRPC client
that sends a PDF in chunks and prints each page event as it arrives:

```bash
docker run --rm --network host \
  -v /path/to/document.pdf:/input/document.pdf:ro \
  --entrypoint /usr/local/bin/grparse-stream-client \
  grparse-grparse /input/document.pdf localhost:50051
```

## Development

The container is the supported build environment. It runs Ubuntu 26.04
with CUDA 13.3, cuDNN 9, ONNX Runtime GPU 1.28.0 for CUDA 13,
RapidOcrOnnx 1.2.3 C++ sources, and gRPC 1.83.0. These are the newest applicable
upstream versions as of 2026-08-11. RapidOCR 3.9.2 is the current Python package
release; its C++ entry point still directs users to RapidOcrOnnx, whose newest
C++ tag is 1.2.3. The container needs an NVIDIA Container Toolkit-enabled
Docker installation. A CUDA-capable ONNX Runtime build is required for the
provider to exist; a CPU-only runtime cannot activate the GPU.

```bash
docker compose build
```

CI builds both images (CUDA and OpenVINO) and then boot-proofs each runtime
stage with `scripts/smoke-test.sh`: library closure of the shipped binaries
plus a boot-to-main check that needs no GPU and no models. The publish
workflow runs the same gate before pushing any tag. With models present
locally, `scripts/smoke-test.sh <image> --full` additionally boots the server
on the CPU provider and streams a fixture through the bundled client.

Every push and PR also runs a short libFuzzer window over the two ingest
doors (Poppler PDF open/extract and OpenCV raster decode) — see
[fuzz/README.md](fuzz/README.md) for the standalone fuzz project and longer
campaigns. A weekly `sanitize.yml` workflow (also manually dispatchable)
builds the whole test battery with `-DGRPARSE_SANITIZE=address,undefined` on
the runner host — not inside `docker build`, whose seccomp profile breaks
LeakSanitizer — and runs it leak-checked under the `tests/lsan.supp`
suppressions.

The build compiles with `-DGRPARSE_WERROR=ON` and runs the full `grparse`-labelled
CTest set: barcode decoder (QR fixture payload, stride-safe region views),
base64, document assembly (offsets, layout label mapping, region
items), geometry merge (including overflow bounds), layout engine (golden
against the reference detector; skips without the model file), office CV
enrichment (figure boxes scaled into twips, class-gated barcode decode over
mapped page renders), scheduler (page
credits, backpressure, partial digital→OCR merge, layout labelling, page
previews), PDF page
source (Poppler text/raster geometry, `/Rotate`, concurrent access, two-column
reading order), Prometheus exporter (exact text rendering, cumulative
histogram, live loopback scrapes with the 404/405/500 doors), raster page
source (in-memory PNG/JPEG decode, BGR
normalization, decode-failure surfacing), reading order (XY-cut multi-column,
determinism), region geometry (center-containment binding, raster clipping,
zero-copy crops), resource pool, table structure (geometry grids, cell
binding, region crops), and streaming/unary contract tests. Third-party dependencies register their own CTest
suites, so the label filter is what keeps `ctest` scoped to this project:

```bash
ctest --test-dir /build --output-on-failure -L grparse
```

Two CMake options exist for local work: `-DGRPARSE_WERROR=OFF` relaxes the
warning gate, and `-DGRPARSE_SANITIZE=address` (or `thread`, `undefined`, or a
comma-separated list) instruments the gRParse targets. ThreadSanitizer cannot
start under `docker build`, which does not allow disabling ASLR; build the test
binaries there and run them with
`docker run --security-opt seccomp=unconfined`. The scheduler, resource pool,
and PDF page source tests are the concurrency-carrying ones and are expected to
be ThreadSanitizer-clean and, with
`LSAN_OPTIONS=suppressions=tests/lsan.supp`, AddressSanitizer- and
UndefinedBehaviorSanitizer-clean. The suppression file covers fontconfig's
one-time global config cache, which Poppler reaches when it substitutes a
base-14 font; it is not a per-page allocation. Generated protobuf and
gRPC sources stay inside the build directory and are not committed; the
document messages live in a single canonical `document.proto`.
