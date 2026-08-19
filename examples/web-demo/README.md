# gRParse web demo

A small Node bridge plus a static page that shows the streaming parse live:
drop a PDF or image on the page and every `DocumentStreamEvent.page` is
painted the moment the server emits it — provenance boxes on a page-scaled
canvas (dashed for OCR text, solid for digital), reading-order text, table
grids, picture classifications, and decoded barcodes. When the service runs
with `GRPARSE_PAGE_IMAGES=on` (the compose file does), the boxes draw over
the real page render — the legend's "page image" checkbox flips between the
overlay and boxes-only views. Each page card carries
its arrival time relative to upload start (the visible proof pages stream
instead of waiting for the whole document), and once the stream completes the
full mapped result downloads as JSON (previews excluded; they are a rendering
aid, not parse data).

![The web demo after parsing the bundled sample](../../docs/images/web-demo.png)

The browser speaks NDJSON to the bridge; the bridge speaks the real
`ParseStreamingService/StreamProcessDocument` bidi stream to gRParse, chunking
the upload exactly like `grparse-stream-client` does. No gRPC-web, no build
step, no framework.

## Bundled sample

The "parse the bundled sample" button streams [`public/sample.pdf`](public/):
page 1 is digital text with a ruled table (native extraction, no OCR), page 2
is a raster page with no text layer, a bar-chart figure, and a QR code — so
OCR, layout, table structure, figure classification, and ZXing barcode
decoding all fire on one click. Regenerate it after changing the fixtures
with [`tools/make_sample.py`](tools/make_sample.py) (needs Pillow,
Ghostscript, and the DejaVu fonts):

```bash
python3 examples/web-demo/tools/make_sample.py
```

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
| `UI_BASE` | *(empty)* | Mount prefix the whole page is served under, e.g. `/ui/grparse` |

## Serving under a base path (`UI_BASE`)

When the demo sits behind a reverse proxy that forwards a path prefix (the
ai-pipestream shell app proxies `/ui/grparse/*` here), set `UI_BASE` to that
prefix:

```bash
UI_BASE=/ui/grparse GRPARSE_TARGET=localhost:50051 npm start
```

The page, its assets, `/api/health`, `/api/parse`, and the bundled
`sample.pdf` are then all served under `/ui/grparse/`, and the bridge injects
a `<meta name="ui-base">` tag into the entry page so the client-side code
prefixes its fetches to match. With `UI_BASE` unset the behavior is exactly
as before: everything lives at the server root.

The bridge loads the contract protos directly from the repository at startup,
so it never drifts from the service's actual schema.
