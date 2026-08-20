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
| `DEMO_UIS` | *(empty)* | Shell registry, `name=grpc_addr@ui_addr` comma-separated (see below) |
| `DEMO_PROTO_DIR` | *(empty)* | Override directory with one `<name>.proto` per registry entry |
| `FASTWARC_TARGET` | `127.0.0.1:50061` | fastwarc-grpc endpoint backing the native FastWARC tab (see below) |

## Demo shell mode (`DEMO_UIS`)

With `DEMO_UIS` set (and `UI_BASE` unset) the same process doubles as the
demo shell for the whole grpc-services family: the header grows a tab bar.
The first tab is this page's own gRParse demo; every other tab is a
registered service whose web frontend renders in an iframe below, proxied
same-origin under `/ui/<name>/` so no CSS or JS leaks between tabs. Each tab
carries a status dot: green when the service's gRPC info RPC answers, red
when it does not.

```bash
DEMO_UIS="lol-html=127.0.0.1:50057@127.0.0.1:8083,libreoffice=127.0.0.1:50053@127.0.0.1:8084" \
  GRPARSE_TARGET=localhost:50051 npm start
```

Registry syntax: comma-separated `name=grpc_addr@ui_addr` entries, where
`grpc_addr` is where the service's info RPC is called and `ui_addr` is the
HTTP frontend to proxy to (a full `http://` URL also works). `name` becomes
the shell path `/ui/<name>`; names match `[A-Za-z0-9][A-Za-z0-9_-]*`.

Three pieces make a tab work:

- `GET /api/uis` returns one `{name, title, path, description, reachable}`
  object per entry. `title`, `path`, and `description` come from a live call
  to the service's info RPC (the `UiInfo` block every ai-pipestream service
  advertises) with a 1.5s deadline; results are cached for 5 seconds so a
  refreshing tab bar doesn't hammer the services. If the call fails, or the
  service's proto can't be loaded, the entry comes back `reachable: false`
  with the registry name as a static fallback title.
- `/ui/<name>/*` is reverse-proxied to `http://<ui_addr>/ui/<name>/*`, path
  preserved — the frontends are started with `UI_BASE=/ui/<name>` and serve
  everything under that prefix. Proxying is raw `http` piping with no body
  buffering, so POST uploads and SSE/NDJSON streams flow through unchanged.
- Proto resolution: a per-name map in `server.js` points each known service
  at its sibling repo (`<repo>/proto/...`); the workspace keeps the repos
  side by side, and the resolver probes both the plain checkout depth and
  the one-level-deeper git worktree depth. Set `DEMO_PROTO_DIR` to a
  directory holding one `<name>.proto` per entry to override the map
  entirely — imports in those files resolve against the same directory. The
  compose demo service instead bind-mounts the sibling proto dirs at
  `/<repo>/proto`, which is where the resolver's probe lands in the image.

With `DEMO_UIS` empty there are no tabs and the served page is
byte-identical to the standalone demo. Shell mode and `UI_BASE` are
mutually exclusive: with `UI_BASE` set this app is a plain frontend serving
under its prefix, ready to be one tab inside another shell.

A new service gets a tab by (1) advertising a `UiInfo` block
(`title`/`path`/`description`) from its info RPC, (2) serving its web
frontend under a `UI_BASE` prefix, and (3) adding one `name=grpc@ui` entry
to `DEMO_UIS` — plus a proto map entry in `server.js` if it isn't one of
the known services.

## Native FastWARC tab

The chatnoir fastwarc-grpc server (`fastwarc.v1.WarcService`, default port
50061) has no web frontend and no info RPC, so it cannot be a `DEMO_UIS`
proxy tab. Instead the shell carries it as a **native tab**: in shell mode
the tab bar always shows "FastWARC" next to the registry tabs, pointing at
the bridge's own `/fastwarc.html`, whether or not the server is reachable
(its status dot and the page's badge follow `GET /api/fastwarc/status`, a
`grpc.health.v1.Health/Check` probe with the same 1.5s deadline and 5s cache
as the `/api/uis` probes). The page is also served standalone at
`/fastwarc.html` when shell mode is off.

The page posts archive bytes to `POST /api/fastwarc/parse` (same 500 MiB cap
as `/api/parse`; `parse_http`/`verify_digests`/`include_payload` as query
flags), the bridge opens the bidirectional `ParseWarc` stream, uploads the
body in 256 KiB chunks, and relays the response stream as NDJSON: one
`start` line per record, a `preview` line with the first 4 KiB of payload
when it reads as text, an `end` line, `error` lines for record failures, and
a final `done` summary. The vendored `collectors/warc*.proto` contracts are
staged into their `fastwarc/v1/` import layout under a temp dir at first
use, mirroring the boot-time staging of the gRParse contract protos.

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
