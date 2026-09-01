# End-to-end suite for the parser stack

Playwright specs that drive the demo stack (`compose.stack.yaml`) through
its front door, the nginx proxy: the demo shell, every native tab, every
proxied service UI, and the parsers behind them, using real fixtures from
`tests/golden/corpus`. Chromium only, no retries; the exit code is the
verdict.

## Layout

| Path | What it holds |
|---|---|
| `playwright.config.ts` | one project (chromium), `E2E_BASE_URL` (default `http://127.0.0.1:18081`), list + HTML + JUnit reporters under `out/` |
| `lib/test.ts` | the `test` every spec imports: an auto fixture collects console errors and uncaught exceptions (iframes included) and fails the test if any were logged |
| `lib/corpus.ts` | corpus lookup (`E2E_CORPUS_DIR` or `../tests/golden/corpus`) and the MIME table |
| `lib/shell.ts` | the tab model: registry tabs, optional tabs, native tabs, tab-strip locators |
| `lib/document-tab.ts` | the Document tab fixture table and its upload/completion helper |
| `lib/service-uis.ts`, `lib/service-ui-flows.ts` | the `/ui/<name>/` table and the two upload flows the frontends share |
| `lib/global-setup.ts` | readiness loop: waits for `/api/uis` and `/api/document/status` (`E2E_READY_TIMEOUT_MS`) |
| `specs/*.spec.ts` | one file per surface, see below |

Every spec navigates on `domcontentloaded` (the pages hold status polls and
parse streams open, so `networkidle` never settles) and waits on the tab's
own progress signals with bounded timeouts rather than sleeping. Failure
messages name the service and the fixture.

| Spec | Covers |
|---|---|
| `shell.spec.ts` | the shell loads, `GET /api/uis` lists exactly the expected registry tabs, each registry tab's iframe (`/ui/<name>/`) renders its UI title, each native tab's iframe renders its page |
| `document.spec.ts` | one corpus fixture per parser through the Document tab (`POST /api/document/parse`): viewer renders, text items present, collector legend names the producer, expected structure (headings / table / picture); plus the tab's status dot |
| `grparse.spec.ts` | the gRParse page-stream tab: the bundled two-page sample and the corpus `hello-text.pdf` stream two page cards with painted box overlays and a completion banner |
| `service-uis.spec.ts` | every `*-ui` frontend under its `/ui/<name>/` prefix loads and parses one fixture through its own upload control (ebcdic streams its bundled sample; calamine is load-only and skipped unless its profile is up) |
| `health.spec.ts` | `/api/health`, every native tab's `/api/<name>/status` reports reachable, every registry entry reachable, tab dots green (amber allowed for a VLM tab without an endpoint) |
| `proxy.spec.ts` | a documented red test for a shell proxy bug (below) |

Adding a parser: one row in `lib/shell.ts` (its tab), one in
`lib/document-tab.ts` (a corpus fixture and what it must render), one in
`lib/service-uis.ts` (its frontend and the counter its upload flow bumps).

## Running against an already-running stack

```bash
cd e2e
npm ci
E2E_BASE_URL=http://127.0.0.1:18081 npx playwright test
npx playwright show-report out/html      # optional
```

`npm ci` installs the pinned `@playwright/test`; if Chromium is not cached
yet, `npx playwright install chromium` fetches it once. Reports land in
`out/html/` and `out/junit.xml`; screenshots of failures in `test-results/`.
`E2E_WORKERS` (default 2) bounds the load on the shared parsers. Run a
single surface with `npx playwright test specs/document.spec.ts`.

## Running as a compose profile

`compose.stack.e2e.yaml` adds a `playwright` service (the official
`mcr.microsoft.com/playwright` image pinned to the same version as
`package.json`) under the `e2e` profile. It mounts this directory and the
corpus, targets `http://proxy:8080` inside the compose network, runs
`npm ci` then `npx playwright test`, and writes the same `out/` reports.

```bash
scripts/stack-e2e.sh                 # up -d --wait (parsers + heavy), then run --rm playwright
NO_GPU=1 scripts/stack-e2e.sh        # layer compose.stack.cpu.yaml
INTEL=1 scripts/stack-e2e.sh         # layer compose.stack.openvino.yaml
E2E_DOWN=1 scripts/stack-e2e.sh      # tear the project down afterwards
```

The script exits with Playwright's exit code. `E2E_PROJECT` names the
compose project (default `parse-stack`, the name the stack file declares).
To point the containerised runner at a stack that is already up somewhere
else, run only the service:

```bash
docker compose -p e2e-check -f compose.stack.yaml -f compose.stack.e2e.yaml \
  run --rm --no-deps -e E2E_BASE_URL=http://host.docker.internal:18081 playwright
docker compose -p e2e-check -f compose.stack.yaml -f compose.stack.e2e.yaml down --remove-orphans
```

## Known red tests

Two tests carry `test.fail()` because they pin bugs found while writing the
suite. They pass (as expected failures) today; when the bug is fixed they
flip to an unexpected pass and fail the run, which is the signal to delete
the marker.

- `proxy.spec.ts`: after the shell proxies a streamed POST to a service
  frontend, the next request it forwards to that frontend is answered
  `400` with an empty body (`examples/web-demo/server.js`,
  `proxyToFrontend`; the frontends answer `200` when reached directly).
  `lib/service-ui-flows.ts` spends that request deliberately after every
  stream so the rest of the suite is deterministic; remove the drain with
  the marker.
- `document.spec.ts` "progress bar disappears": `.dv-progress` has
  `display: flex` in `style.css`, which outranks the `hidden` attribute the
  Document tab sets when a parse finishes, so the "parsing..." bar stays
  painted.

## Rules

- Corpus fixtures only; nothing outside `tests/golden/corpus` is uploaded.
- No `networkidle`, no fixed sleeps, no retries, no console-error allowlist.
- Shared helpers live in `lib/`; specs stay small and data-driven.
