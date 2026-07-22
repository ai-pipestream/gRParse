# gRParse

C++ gRPC document parse service: **diskless PDF/image to page-streamed protobuf** with boxes and stable offsets. RapidOCR runs through **ONNX Runtime CUDA** on NVIDIA GPUs. Intel Arc/OpenVINO, layout, tables, and figures are roadmap work, not current runtime capabilities.

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

Each PDF request opens one Poppler document directly from the request bytes. Full native-text pages skip raster OCR. Weak/partial digital layers keep their native boxes and still run OCR; geometry merge drops overlapping OCR duplicates so headers and scan body can coexist. Image-only pages render at 200 DPI into RapidOCR. Raster inputs decode with OpenCV from request memory. Nothing is written to disk on the hot path.

`ConvertSource` returns the contract's `ConvertDocumentResponse`, populated with a native `DoclingDocument`. Each OCR line becomes a `TextItem`, with its page and bounding box in `provenance`; pages and the `#/body` to `#/texts/N` reference graph are also populated. It deliberately leaves tables, pictures, semantic headings, chunking, asynchronous jobs, and remote sources unimplemented until appropriate layout or extraction models are added.

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
continue through the global queues.

The server has two CUDA RapidOCR sessions by default. Tune concurrency and
queue memory with `GRPARSE_PAGE_WORKERS`, `GRPARSE_RENDER_WORKERS`,
`GRPARSE_ASSEMBLY_WORKERS`, `GRPARSE_DOCUMENT_QUEUE`, `GRPARSE_RENDER_QUEUE`,
`GRPARSE_INFERENCE_QUEUE`, `GRPARSE_ASSEMBLY_QUEUE`, `GRPARSE_PAGE_WINDOW`, and
`GRPARSE_MAX_ACTIVE_DOCUMENTS`. Select the NVIDIA device with
`GRPARSE_CUDA_DEVICE`. Optional RapidOCR detect knobs:
`GRPARSE_OCR_PADDING`, `GRPARSE_OCR_MAX_SIDE`, `GRPARSE_OCR_BOX_SCORE`,
`GRPARSE_OCR_BOX_THRESH`, `GRPARSE_OCR_UNCLIP`. gRPC memory, thread,
and stream limits use `GRPARSE_GRPC_MEMORY_MIB`, `GRPARSE_GRPC_MAX_THREADS`,
and `GRPARSE_MAX_CONCURRENT_STREAMS`. RapidOCR currently produces text,
so `tables` and `pictures` remain empty until dedicated extraction models are
connected.

The server registers standard gRPC health checking and reflection in addition
to the Docling `Health` RPC. SIGINT and SIGTERM initiate a bounded graceful
shutdown.

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

The build runs assembly, geometry-merge, base64, scheduler (including partial
digital→OCR merge), unary, and streaming contract tests. Generated protobuf and
gRPC sources stay inside the build directory and are not committed.
