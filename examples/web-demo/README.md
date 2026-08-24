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
| `FASTWARC_TARGET` | `127.0.0.1:50060` | fastwarc-grpc endpoint backing the native FastWARC tab (see below) |
| `POIC_TARGET` | `127.0.0.1:50052` | grPOIc endpoint backing the native POI tab (see below) |
| `ASR_TARGET` | `127.0.0.1:50055` | grpc-asr endpoint backing the native ASR tab (see below) |
| `ENRICH_TARGET` | `127.0.0.1:50056` | grpc-enrich endpoint backing the native Enrich tab (see below) |
| `VLM_CONVERT_TARGET` | `127.0.0.1:50058` | grpc-vlm-convert endpoint backing the native VLM Convert tab (see below) |

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

## Native Document tab

The Document tab (`/document.html`, first native tab in the shell bar) is a
page-faithful viewer for the **complete merged document** — where the main
gRParse tab shows the live page stream, this one shows the final assembled
result. The page posts document bytes to `POST /api/document/parse` (same
500 MiB cap; `filename` and `contentType` as query params). The bridge runs
the unary `ConvertSource` RPC and relays NDJSON: `page` progress lines
(`pageNumber`, `totalPages`, `elapsedMs` — fed by a parallel
`StreamProcessDocument` call over the same bytes, purely so progress is
live; that leg's failures stay silent), then one `document` line carrying
the whole merged document as protobuf-JSON (camelCase field names, enum
value names as strings), or an `error` line. The tab dot follows
`GET /api/document/status`, a `Health` probe on the same channel
`/api/parse` uses, with the usual 1.5s deadline and 5s cache.

The viewer resolves the body tree against the item arenas
(`texts`/`tables`/`pictures`/`groups`/`key_value_items`/...) and renders one
card per page: the page image with an absolutely-positioned provenance box
per item on the left (colored by item label; the legend shows per-label
counts with checkboxes to hide labels), and the content in reading order on
the right — headings by level, nested lists from list groups, tables with
row/column spans and header cells, figures with their inline images,
captions, code with language badges, formulas. Hovering a box highlights
the matching content element and vice versa; clicking either scrolls its
counterpart into view. Items without page provenance (contributed by
out-of-process collectors) render in a trailing "unpaged content" section
grouped by collector, with confidence badges where the producer reported
one. Non-body layers (furniture, notes) stay hidden behind a toggle that
reveals them dimmed. Each item's metadata is available in a collapsible
drawer, and page cards build lazily as they scroll into view since page
images are large data URIs.

Items are deep-linkable: a URL fragment of `#item=<self_ref>` (optionally
`&cs=<start>-<end>` for a character range within that item's text) scrolls
both panes to the item and outlines it persistently until another anchor
lands or Escape is pressed; an unresolvable ref shows a small notice instead
of failing. Hovering a content element reveals a small copy-link button that
writes that fragment to the clipboard, including the current text
selection's range when the selection falls inside the hovered item.

## Native FastWARC tab

The standalone fastwarc-grpc server (`fastwarc.v1.WarcService`, default port
50060) has no web frontend of its own, so the shell carries it as a
**native tab**: in shell mode the tab bar always shows "FastWARC" next to
the registry tabs, pointing at the bridge's own `/fastwarc.html`, whether or
not the server is reachable (its status dot and the page's badge follow
`GET /api/fastwarc/status`, a `GetServiceInfo` probe with a
`grpc.health.v1.Health/Check` fallback, at the same 1.5s deadline and 5s
cache as the `/api/uis` probes). The page is also served standalone at
`/fastwarc.html` when shell mode is off.

The page posts archive bytes to `POST /api/fastwarc/parse` (same 500 MiB cap
as `/api/parse`; `parse_http`/`verify_digests` as query flags), the bridge
opens the bidirectional `ParseWarc` stream, uploads the body in 256 KiB
chunks, and relays the response stream as NDJSON: one `start` line per
record, a `preview` line with the first 4 KiB of payload when it reads as
text, an `end` line (with digest verification results when requested),
`error` lines for record failures, and a final `done` summary. The contract
resolves from the sibling `fastwarc-grpc` checkout through the same
`KNOWN_UIS` proto map as the other native tabs (the compose stack
bind-mounts it at `/fastwarc-grpc/proto`); the vendored
`collectors/warc*.proto` files speak the legacy chatnoir dialect and are
used only by the C++ collector's own client, never by this bridge.

## Native POI tab

grPOIc (`ai.pipestream.poi.v1.PoiParseService`, default port 50052) wraps
Apache POI: office document bytes in, typed structure events out. The shell
carries it as a **native tab** like FastWARC: in shell mode the tab bar
always shows "POI", pointing at the bridge's own `/poic.html`, whether or
not the server is reachable (its status dot and the page's badge follow
`GET /api/poic/status`, a `GetServiceInfo` probe with the same 1.5s deadline
and 5s cache as the other probes; the response also carries the service and
POI versions and the supported formats). The page is also served
standalone at `/poic.html` when shell mode is off.

The page posts document bytes (.docx/.xlsx/.pptx and the legacy OLE2 trio)
to `POST /api/poic/parse` (same 500 MiB cap as `/api/parse`; `filename` and
`contentType` as query params), the bridge opens the bidirectional
`ParseDocument` stream, uploads the body in 1 MiB chunks (identity fields on
the first, `complete` on the last), and relays the response stream as
NDJSON: a `start` line from `DocumentInfo` (detected format plus the
well-known metadata fields), one `preview` line per content element
(paragraph/table/sheet/slide/embedded object, text capped at 512
characters), an `end` line from the final `ParseStatus`, a `grpc-error`
line on stream failure, and a final `done` summary. The contract is
resolved from the sibling grPOIc checkout through the same
`KNOWN_UIS`/`resolveServiceProto` map the `/api/uis` probes use, so it works
in the plain workspace, a worktree, and the demo image's bind mounts.

## Native ASR, Enrich, and VLM Convert tabs

grpc-asr (`ai.pipestream.asr.v1.AsrService`, default port 50055), grpc-enrich
(`ai.pipestream.enrich.v1.EnrichService`, default port 50056), and
grpc-vlm-convert (`ai.pipestream.vlm.v1.VlmConvertService`, default port
50058) are headless pipeline services, so the shell carries each as a
**native tab** like FastWARC and POI: pages at `/asr.html`, `/enrich.html`,
and `/vlm-convert.html`, status dots and page badges following
`GET /api/<name>/status` (a `GetServiceInfo` probe with the same 1.5s
deadline and 5s cache as the other probes), contracts resolved from the
sibling repos through the same `KNOWN_UIS`/`resolveServiceProto` map. Each
page degrades to a "service unreachable" badge when its server is down.

- **ASR** posts audio/video bytes to `POST /api/asr/transcribe` (500 MiB
  cap; `model`/`language`/`task`/`word_timestamps` as query params — the
  model select is filled from the models `GetServiceInfo` reports, since
  `TranscribeOptions.model` is required and must be loaded). The bridge
  opens the bidirectional `Transcribe` stream (options first, then 1 MiB
  chunks) and relays NDJSON: a `media` line from `MediaInfo`, `partial` and
  `final` lines as the decoder commits segments (the page updates rows in
  place — finals replace partials by index), a `complete` trailer line,
  `grpc-error` on failure, `done` last.
- **Enrich** posts a JSON form to `POST /api/enrich/annotate`: pasted code
  and formula text plus an optional image, which the bridge folds into a
  minimal `ai.pipestream.document.v1.Document` (a `CodeItem`, a
  `FormulaItem`, a `PictureItem` with an inline data-URI `ImageRef`,
  labelled `CHART` when chart extraction is requested — that label is how
  the service selects pictures for the chart job). It then streams
  `EnrichDocument` with the document inline in the options message and
  relays NDJSON: `started` counts, one `annotation` or `skipped` line per
  item as its VLM call returns (the page groups rows by annotation kind),
  the `complete` trailer, `done` last.
- **VLM Convert** posts JSON (`preset`/`presetRaw`/`responseFormat`/
  `endpoint` plus base64 PNG pages with their pixel sizes, read
  client-side, one page per file ordered by filename) to
  `POST /api/vlm-convert/convert`. The bridge streams `ConvertPages` and
  relays NDJSON: `started` as pages enter the model queue, one `page` line
  per converted Document fragment in completion order (pages finish out of
  order, keyed by `pageNo`; the page renders one card per page), `raw`
  lines for mapping failures or endpoint errors, the `complete` trailer,
  `done` last.

Enrich and VLM Convert are clients of an **external VLM server**
(`ENRICH_VLM_URL` / `GRPC_VLM_ENDPOINT` on the service side). Their status
payloads carry `vlmConfigured`, and a reachable service with no configured
endpoint shows an **amber** dot and badge instead of green: enrichments
would all skip with `vlm-error` and conversions fail `FAILED_PRECONDITION`
until an endpoint is configured or the per-request override field on the
page is set.

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
