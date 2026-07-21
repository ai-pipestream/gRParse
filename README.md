# gRParse

gRParse is a C++ gRPC service that turns PDF pages and raster images into text with the maintained C++ [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) implementation. It targets NVIDIA CUDA through ONNX Runtime. The host was detected with an NVIDIA GeForce RTX 4080 SUPER; the included container exposes it with Compose's `gpus: all` setting.

## Run

1. Download the four model files listed in [models/README.md](models/README.md).
2. Build and start the service:

   ```bash
   docker compose up --build
   ```

The service listens on `localhost:50051` and implements `ai.docling.serve.v1.DoclingServeService` from the local `docling_serve.proto` contract. `ConvertSource` currently accepts one `FileSource` containing base64-encoded PDF, PNG, JPEG, or TIFF bytes. PDFs are rendered with Poppler at 200 DPI, then each page is processed on CUDA device 0.

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
`DocumentStreamEvent.complete`. A page event contains the supplied Docling
`PageItem` and the page's supplied `BaseTextItem` records. The original
`DoclingDocument` shape is unchanged: this is only a transport envelope for
incremental delivery. Each page event is allocated in its own
`google::protobuf::Arena`; the arena, page image, and event state are discarded
before the next page is rasterized. RapidOCR currently produces text, so `tables` and
`pictures` remain empty until dedicated extraction models are connected.

## Development

The container is the supported build environment because it runs Ubuntu 26.04 with CUDA 13.3, cuDNN 9, ONNX Runtime GPU 1.26.0 for CUDA 13, RapidOcrOnnx 1.2.3 C++ sources, and gRPC 1.82.1. It needs an NVIDIA Container Toolkit-enabled Docker installation. A CUDA-capable ONNX Runtime build is required for the provider to exist; a CPU-only runtime cannot activate the GPU.

```bash
docker compose build
```
