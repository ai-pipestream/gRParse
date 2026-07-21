# gRParse

C++ gRPC document parse service: **diskless PDF/image → streamed page JSON** with boxes and stable offsets. All **ONNX** models run here on **ONNX Runtime** (NVIDIA CUDA and **Intel Arc / OpenVINO**, e.g. B70; OCR today; layout/tables/figures next). Java owns language decoration, office bridges, and UI-facing geometry.

- Architecture (runtime split, anti-seesaw pipeline, offset contract): [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Epics & tasks (C++ vs Java ownership, milestones): [docs/EPICS.md](docs/EPICS.md)

**Speed thesis:** pipelined pages, warm ORT session pools, selective OCR, early stream emit — keep CPU and GPU busy (no stage seesaw).

gRParse turns PDF pages and raster images into text with the maintained C++ [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) implementation. It targets NVIDIA CUDA through ONNX Runtime. The host was detected with an NVIDIA GeForce RTX 4080 SUPER; the included container exposes it with Compose's `gpus: all` setting.

## Run

1. Download the four model files listed in [models/README.md](models/README.md).
2. Build and start the service:

   ```bash
   docker compose up --build
   ```

The service listens on `localhost:50051` and implements `ai.docling.serve.v1.DoclingServeService` from the local `docling_serve.proto` contract. `ConvertSource` currently accepts one `FileSource` containing base64-encoded PDF, PNG, JPEG, or TIFF bytes. PDFs are rendered by Poppler's C++ API at 200 DPI and raster images are decoded with OpenCV, both directly from request memory. No input document or page image is written to disk.

`ConvertSource` returns the contract's `ConvertDocumentResponse`, populated with a native `DoclingDocument`. Each OCR line becomes a `TextItem`, with its page and bounding box in `provenance`; pages and the `#/body` to `#/texts/N` reference graph are also populated. It deliberately leaves tables, pictures, semantic headings, chunking, asynchronous jobs, and remote sources unimplemented until appropriate layout or extraction models are added.

The `Health` RPC reports readiness. The server intentionally fails at startup if a model is absent or CUDA initialization fails, instead of silently running CPU OCR.

To convert a PDF through the supplied unary Docling RPC, start the service and
run the reusable `grpcurl` client. It emits the complete
`ConvertSourceResponse` as JSON.

```bash
docker compose up -d
./scripts/parse_pdf.sh /path/to/document.pdf
```

## Page-streaming OCR

`ai.docling.serve.v1.DoclingStreamingService/StreamProcessDocument` accepts a
stream of `DocumentChunk` messages. Send the same `document_id`, filename, and
content type with the chunks, then set `complete = true` on the last one. The
server accepts PDFs and single raster images, up to 50 MiB.

It emits one `DocumentStreamEvent.page` per completed page, followed by one
`DocumentStreamEvent.complete`. Page events may arrive out of page-number order;
clients must use `PageData.page_number`. A page event contains the supplied Docling
`PageItem` and the page's supplied `BaseTextItem` records. The original
`DoclingDocument` shape is unchanged: this is only a transport envelope for
incremental delivery. Each page event is allocated in its own
`google::protobuf::Arena`; the arena, page image, and event state are discarded
after gRPC serializes the event. The server has a bounded pool of CUDA RapidOCR
sessions, with two page workers by default. Set `GRPARSE_PAGE_WORKERS` to tune
the concurrency for the available GPU memory. RapidOCR currently produces text,
so `tables` and `pictures` remain empty until dedicated extraction models are
connected.

The image also includes `grparse-stream-client`, a bidirectional gRPC client
that sends a PDF in chunks and prints each page event as it arrives:

```bash
docker run --rm --network host \
  -v /path/to/document.pdf:/input/document.pdf:ro \
  --entrypoint /usr/local/bin/grparse-stream-client \
  grparse-grparse /input/document.pdf localhost:50051
```

## Development

The container is the supported build environment because it runs Ubuntu 26.04 with CUDA 13.3, cuDNN 9, ONNX Runtime GPU 1.26.0 for CUDA 13, RapidOcrOnnx 1.2.3 C++ sources, and gRPC 1.82.1. It needs an NVIDIA Container Toolkit-enabled Docker installation. A CUDA-capable ONNX Runtime build is required for the provider to exist; a CPU-only runtime cannot activate the GPU.

```bash
docker compose build
```
