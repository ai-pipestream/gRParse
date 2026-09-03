# Design: the grpc-chromium collector

**Status:** design, no implementation yet. The service does not exist; this
document is the agreement to build against.
**Updated:** 2026-09-03

## 1. Motivation

The fleet parses static HTML well: grpc-markup projects it through
html5ever, and grpc-lol-html answers targeted selector questions about it.
Neither runs JavaScript, and a growing share of the HTML the fleet actually
meets is a lie until JavaScript runs. The worst of it arrives through the
fastwarc collector: web archives store the bytes the server sent, not the
DOM the user saw, and for client-rendered pages the recorded payload is a
loader script and an empty `<div id="root">`. A static projection of that
produces a faithful document of nothing.

A headless browser closes that gap and buys three more things the fleet
already knows how to consume:

- **Print-to-PDF into the existing PDF pipeline.** Chromium's renderer is a
  paginating layout engine. Printing a rendered page to PDF and feeding
  those bytes into gRParse's own CV path (layout, OCR routing through
  grpc-pdf-inspector, the scorecard) reuses a pipeline that is already
  tested, scored, and gated, instead of inventing a second way to turn a
  page into structure.
- **PNG page previews for the demo shell.** Every other collector that
  touches paged output shows pages in its tab. Screenshots per print page
  give a future chromium tab the same shape with no new UI machinery.
- **The accessibility tree as a structure signal.** Chromium already
  computes a de-chromed semantic tree for assistive technology: landmarks,
  headings, lists, tables, and reading-order-ish grouping with navigation
  chrome and script scaffolding stripped by construction. It is not a
  replacement for markup parsing, but for pages whose DOM is adversarial
  soup it is a signal the fleet cannot get from bytes alone.

## 2. Non-goals

- **Not a replacement for grpc-markup's HTML projection.** Static HTML
  stays with markup; html5ever is faster, cheaper, and deterministic in a
  way a browser never is. Chromium is for pages that need execution.
- **Not a crawler.** One page per request. No link following, no sitemap
  walking, no crawl state. A request names one document; the service
  renders that document.
- **Not a general browser automation platform.** No scripting interface,
  no form filling, no session persistence beyond one render.
- **Not a screenshot service for arbitrary URLs on behalf of the public
  internet.** See the bytes-in rule below.

## 3. Wire contract sketch

Package `ai.pipestream.chromium.v1`, owned by the new repo and vendored
byte-identical into `gRParse/collectors/` by the usual rule.

```text
rpc RenderPage(stream RenderPageRequest) returns (stream RenderPageResponse);
rpc GetServiceInfo(GetServiceInfoRequest) returns (GetServiceInfoResponse);
```

First client message is options; subsequent messages are chunks of the
page bytes with a terminal `complete`, exactly the ParseEmail shape. The
family is diskless and its web content is WARC-sourced, so the primary
input is **bytes in**: the caller hands over the HTML payload (plus,
later, the sibling resources fastwarc can supply for the same capture).
A `url` field may exist for the demo shell, but fetching URLs server-side
is an explicit opt-in option, off by default: a parser that fetches the
network on behalf of whoever uploads bytes is an SSRF appliance, and the
collector posture is that bytes are the input.

Events, in order:

1. `RenderInfo`: final URL (after redirects and fragment navigation),
   title, console error count, render wall time, the Chromium version
   string. Facts about the render, before any payload.
2. `PagePng`: one event per print page, PNG bytes plus page index, for
   previews. Only when `emit_page_pngs` is set.
3. `PdfDocument`: the print-to-PDF bytes, once, when `emit_pdf` is set.
   Chunked if the page size ceiling requires it.
4. `AccessibilityTree`: the a11y tree as a typed node stream (role, name,
   level, bounding box where the renderer reports one), or a folded
   projection when `emit_document` is set instead.
5. `Document`: only when `emit_document`; see below.
6. `ParseStatus`: counts, warnings (mixed content, blocked subresources,
   console errors), the trailer.

### Where the Document fold lives

Two fold shapes exist in the family: libreoffice and lol-html emit typed
events and gRParse folds client-side; markup and epub project server-side
through `emit_document`. **Decision: server-side projection.** The a11y
tree is at its most informative inside the browser process, where the
fold can ask CDP follow-up questions (resolve a node's computed role,
drop display:none subtrees, correlate with the rendered text) while the
page is still alive. A client-side fold would have to serialize that
whole conversation onto the wire first, and every improvement to the fold
would be a gRParse release instead of a grpc-chromium release. Chromium
joins the emit_document majority.

## 4. Service shape

- **Tab lifecycle.** One tab per request, created on request start and
  destroyed on stream end, success or failure. Tabs are never pooled or
  reused: a tab is the unit of compromise and the unit of memory growth,
  and reuse leaks both across requests. A small pool of browser
  *processes* (not tabs) amortizes startup. A crashed renderer fails its
  RPC with `INTERNAL` and its browser process is replaced; a dead tab
  must never wedge the pool.
- **Memory ceilings.** Chromium does not respect container limits on its
  own. The service enforces: a per-render wall clock, a per-render output
  byte cap (PDF plus PNGs plus tree), and a hard process ceiling via
  cgroup-aware browser restarts (a browser process that balloons is
  killed and its in-flight renders fail). Defaults follow the fleet's
  byte-cap doctrine; the caller may lower, never raise.
- **Sandboxing.** The honest tradeoff: Chromium's own sandbox wants user
  namespaces and setuid helpers the hardened dhi.io runtime bases do not
  ship, so `--no-sandbox` is the tempting default and the wrong one.
  Ordering of preference: (1) the Chromium sandbox with a custom seccomp
  profile layered on the container, keeping user namespaces enabled in
  the runtime image; (2) `--no-sandbox` inside a container that is itself
  the sandbox (no capabilities, read-only root, no network beyond the
  gRPC listener, seccomp default-deny), documented as a deliberate
 downgrading of the browser-internal boundary rather than a silence. Option 1 is
  the target; option 2 is the acceptable interim and must be loud in the
  README. This is a real conflict with the hardened-image posture and the
  design does not pretend otherwise.
- **Version pinning and CVE cadence.** Chromium is the single largest
  attack surface in the fleet and the most actively exploited. The image
  pins an exact chrome-headless-shell build, and repinning is a
  first-class chore on a fixed cadence (weekly bump PR, renovate-style)
  rather than an event-driven scramble. A stale pin is a known,
  dashboard-visible state, not a surprise.
- **Why Chromium and not Firefox.** CDP is the mature, documented,
  tooling-rich control plane; the ecosystem (chrome-headless-shell as a
  minimal headless build, Playwright's driver protocol experience, the
  a11y tree's fidelity) is deeper. Firefox's WebDriver BiDi is converging
  but covers less of what this service needs, and print-to-PDF control is
  weaker.
- **amd64 only at first.** The C++ services already publish amd64-only
  because emulated arm64 builds are too slow; a Chromium-based image
  inherits that constraint and there is no reason to fight it in v1.

## 5. gRParse integration

- **Endpoint.** `GRPARSE_CHROMIUM_TARGET=<host:port>`; unset means the
  collector does not exist, same as every other collector.
- **Routing.** Explicit collector selection first: the caller names
  chromium, the way EBCDIC and lol-html are reached today. A default
  route is a later, separate decision, and the honest candidate is not
  "all HTML" (markup owns static HTML and should keep it) but HTML
  payloads that arrive from WARC captures, where "the bytes lie until
  JavaScript runs" is the common case rather than the exception. That
  routing change lands only after the collector has scorecard evidence
  that its output beats the static projection on the WARC corpus.
- **Ports.** Next free gRPC port is **50069**; demo HTTP port **8091**
  (8090 is taken by grpc-email's viewer). Per the workspace rule, the
  workspace `AGENTS.md` port tables are extended with the `grpc-chromium`
  rows when the service actually exists, not before.
- **UiInfo.** The info RPC carries the standard `UiInfo` block (title,
  suggested path `/ui/chromium`, one-line description), so the demo shell
  grows the tab the day the service joins the stack.
- **Merge.** The projected Document carries `CollectorSource{collector =
  "chromium", ...}` like every other collector and merges additively; the
  PDF leg needs no collector wiring at all, since print-to-PDF bytes can
  loop back through gRParse's own PDF path as an ordinary PDF parse.

## 6. Failure modes

- Renderer crash: the RPC fails `INTERNAL` naming the phase; the browser
  process is recycled; the service survives.
- Page that never quiesces: the wall clock fires, the render is cut off,
  and what completed (screenshots, partial tree) is still emitted with a
  `ParseStatus` warning. A timeout is a degraded render, not an error.
- Hostile page: sandbox escape is assumed possible; the container is the
  second boundary, network egress beyond the listener is denied, and no
  credentials exist in the environment to steal.
- Chromium CVE published against the pinned build: the pinned version is
  bumped on the cadence above; the service announces its build in
  `RenderInfo` and `GetServiceInfo` so staleness is observable.

## 7. Test plan

- Fixture pages authored in-memory and served from a test-local origin:
  a static page (must match markup's projection within tolerance), a
  client-rendered page (content only exists after script runs), a page
  with a slow timer that never settles (timeout path), a page that
  crashes the renderer on demand (crash recovery).
- Print-to-PDF golden: a fixed page renders to a PDF whose page count and
  extracted text are asserted, and the same bytes round-trip through the
  gRParse PDF path in an integration test.
- a11y tree: a page with known landmarks, headings, and a table asserts
  the projected Document's structure, not its exact bytes.
- Stream-liveness tests in the family shape: an event must arrive before
  the input is fully consumed; `ParseStatus` is a trailer.
- The merge-safety rules every Document-producing collector follows:
  densely numbered refs, `#/body` and `#/furniture` only, integrity
  check asserted empty in every fold test.

## 8. Milestones

- **M1: render and capture.** RenderPage with `emit_pdf` and
  `emit_page_pngs`: tab lifecycle, memory ceilings, sandboxing decision
  implemented, pinned Chromium build, print-to-PDF and screenshot events,
  tests for the happy path and the crash path. The PDF leg works through
  the existing gRParse PDF pipeline with no gRParse change.
- **M2: projection and wiring.** The a11y tree event, the `emit_document`
  fold, `GRPARSE_CHROMIUM_TARGET` support in gRParse with explicit
  selection, the demo tab, the workspace port-table rows.
- **M3: WARC integration.** fastwarc supplies the capture's HTML plus its
  recorded subresources, the render runs from those bytes without
  touching the network, and the WARC-routing default is evaluated against
  the scorecard before it ships.
