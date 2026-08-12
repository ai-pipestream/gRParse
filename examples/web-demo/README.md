# gRParse web demo

A small Node bridge plus a static page that shows the streaming parse live:
drop a PDF or image on the page and every `DocumentStreamEvent.page` is
painted the moment the server emits it — provenance boxes on a page-scaled
canvas (dashed for OCR text, solid for digital), reading-order text, table
grids, picture classifications, and decoded barcodes.

The browser speaks NDJSON to the bridge; the bridge speaks the real
`ParseStreamingService/StreamProcessDocument` bidi stream to gRParse, chunking
the upload exactly like `grparse-stream-client` does. No gRPC-web, no build
step, no framework.

## Run with compose (service + demo together)

From the repository root:

```bash
docker compose --profile demo up --build
```

Then open <http://localhost:8080>. The `grparse` service itself still needs
the model files and a GPU as described in the top-level README.

## Run against an already-running service

```bash
cd examples/web-demo
npm install
GRPARSE_TARGET=localhost:50051 npm start
```

Environment:

| Variable | Default | Meaning |
|---|---|---|
| `GRPARSE_TARGET` | `localhost:50051` | gRParse gRPC endpoint |
| `PORT` | `8080` | HTTP port for the page |
| `GRPARSE_PROTO_DIR` | repo root | Directory holding the four contract `.proto` files |

The bridge loads the contract protos directly from the repository at startup,
so it never drifts from the service's actual schema.
