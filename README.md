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

The service listens on `localhost:50051` and implements `ai.docling.serve.v1.DoclingServeService` from the local `docling_serve.proto` contract. `ConvertSource` currently accepts one `FileSource` containing base64-encoded PDF, PNG, JPEG, or TIFF bytes. It accepts only TEXT output and returns `INVALID_ARGUMENT` for populated options it does not implement. It does not label plain text as Markdown.

Each PDF request opens a small pool of Poppler documents directly from the request bytes, so render and digital-text extraction for different pages of the same document proceed in parallel. Full native-text pages skip raster OCR. Weak/partial digital layers keep their native boxes and still run OCR; geometry merge drops overlapping OCR duplicates so headers and scan body can coexist. Image-only pages render at 200 DPI into RapidOCR. Raster inputs decode with OpenCV from request memory. Nothing is written to disk on the hot path.

`ConvertSource` returns the contract's `ConvertDocumentResponse`, populated with a native `DoclingDocument`. Each OCR line becomes a `TextItem`, with its page and bounding box in `provenance`; pages, `TableItem`/`PictureItem` entries from layout, and the `#/body` reference graph are also populated. It deliberately leaves semantic chunking, asynchronous jobs, and remote sources unimplemented.

The `Health` RPC reports readiness. The server intentionally fails at startup if a model is absent or CUDA initialization fails, instead of silently running CPU OCR.

To stream a PDF with the supplied client, start the service and run:

```bash
docker compose up -d
./scripts/parse_pdf.sh /path/to/document.pdf
```

The helper invokes the compiled bidirectional-streaming client. It reads the
source and sends fixed-size chunks directly to gRPC; it does not base64-encode
the document or create temporary files.

## Page-streaming OCR

`ai.docling.serve.v1.DoclingStreamingService/StreamProcessDocument` accepts a
stream of `DocumentChunk` messages. Send the same `document_id`, filename, and
content type with the chunks, then set `complete = true` on the last one. The
server accepts PDFs and single raster images, up to 50 MiB.

It emits one `DocumentStreamEvent.page` per page in page-number order, followed by one
`DocumentStreamEvent.complete`. A page event contains the supplied Docling
`PageItem` and the page's supplied `BaseTextItem` records. `TextOffset` carries
append-only UTF offsets, source type, and OCR confidence when available. The original
`DoclingDocument` shape is unchanged: this is only a transport envelope for
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

With `GRPARSE_PICTURE_IMAGES=on` (default off) each figure region's pixels
are cropped from the page raster in the inference stage, PNG-encoded, and
attached to its `PictureItem` as an `image/png` data URI with the pixel
size. The crop happens after OCR and before the raster is released, so
device work is never delayed and no raster outlives its page. Leave it off
for figure-heavy corpora where embedded images would inflate every page
event; picture bounding boxes are always present either way.

The server registers standard gRPC health checking and reflection in addition
to the Docling `Health` RPC. SIGINT and SIGTERM initiate a bounded graceful
shutdown.

Every `GRPARSE_METRICS_INTERVAL_SECONDS` (default 60, `0` disables) the server
prints one pipeline metrics line to stdout: document and page counters, queue
depths, per-stage busy percentages since the previous line, OCR session pool
acquire/discard/wait totals, and a page-latency histogram from schedule to
delivery. Render and inference busy percentages climbing together under load
is the pipeline overlap working; one stage pegged while its neighbor idles
identifies where to add workers.

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
with CUDA 13.3, cuDNN 9, ONNX Runtime GPU 1.27.1 for CUDA 13,
RapidOcrOnnx 1.2.3 C++ sources, and gRPC 1.82.1. These are the newest applicable
upstream versions as of 2026-07-21. RapidOCR 3.9.2 is the current Python package
release; its C++ entry point still directs users to RapidOcrOnnx, whose newest
C++ tag is 1.2.3. The container needs an NVIDIA Container Toolkit-enabled
Docker installation. A CUDA-capable ONNX Runtime build is required for the
provider to exist; a CPU-only runtime cannot activate the GPU.

```bash
docker compose build
```

The build compiles with `-DGRPARSE_WERROR=ON` and runs the full `grparse`-labelled
CTest set: base64, document assembly (offsets, layout label mapping, region
items), geometry merge (including overflow bounds), layout engine (golden
against the reference detector; skips without the model file), scheduler (page
credits, backpressure, partial digital→OCR merge, layout labelling), PDF page
source (Poppler text/raster geometry, `/Rotate`, concurrent access, two-column
reading order), reading order (XY-cut multi-column, determinism), resource
pool, table structure (geometry grids, cell binding, region crops), and
streaming/unary contract tests. Third-party dependencies register their own CTest
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
`docling_document` messages live in a single canonical `docling_document.proto`.
