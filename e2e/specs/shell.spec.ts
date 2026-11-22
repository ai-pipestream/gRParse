import { test, expect } from "../lib/test";
import { bridgeStatus, expectNoGrpcError, linesOfType, postNdjson, pngSize } from "../lib/bridges";
import { corpusFile } from "../lib/corpus";
import {
  NATIVE_TABS,
  OPTIONAL_REGISTRY_TABS,
  REGISTRY_TABS,
  openShell,
  tabButton,
  type UiEntry,
} from "../lib/shell";

test.describe("demo shell", () => {
  test("loads with the gRParse tab active", async ({ page }) => {
    await openShell(page);
    await expect(page).toHaveTitle(/gRParse/);
    await expect(page.locator("nav.tab-strip")).toBeVisible();
    await expect(tabButton(page, "gRParse")).toHaveClass(/\bactive\b/);
    await expect(page.locator("#grparse-body #dropzone")).toBeVisible();
    // One button per tab: the gRParse pane, the native tabs, the registry,
    // plus any opt-in tab the running stack carries.
    const required = 1 + NATIVE_TABS.length + REGISTRY_TABS.length;
    const buttons = page.locator("nav.tab-strip button.tab");
    await expect.poll(() => buttons.count(), { message: "tab strip button count" }).toBeGreaterThanOrEqual(required);
    expect(await buttons.count(), "tab strip button count").toBeLessThanOrEqual(required + OPTIONAL_REGISTRY_TABS.length);
  });

  test("GET /api/uis lists the expected registry tabs", async ({ request }) => {
    const response = await request.get("/api/uis");
    expect(response.ok(), `GET /api/uis -> HTTP ${response.status()}`).toBe(true);
    const { uis } = (await response.json()) as { uis: UiEntry[] };
    const byName = new Map(uis.map((entry) => [entry.name, entry]));

    for (const tab of REGISTRY_TABS) {
      const entry = byName.get(tab.name);
      expect(entry, `registry tab ${tab.name} missing from /api/uis`).toBeDefined();
      expect(entry?.reachable, `registry tab ${tab.name} reports unreachable`).toBe(true);
      expect(entry?.path, `registry tab ${tab.name} path`).toBe(`/ui/${tab.name}`);
      expect(entry?.title, `registry tab ${tab.name} title`).toBe(tab.title);
    }
    const known = new Set([...REGISTRY_TABS, ...OPTIONAL_REGISTRY_TABS].map((tab) => tab.name));
    const unknown = uis.map((entry) => entry.name).filter((name) => !known.has(name));
    expect(unknown, "tabs in /api/uis that e2e/lib/shell.ts does not know; add a row").toEqual([]);
  });

  for (const tab of REGISTRY_TABS) {
    test(`registry tab ${tab.title}: iframe route /ui/${tab.name}/ renders its UI`, async ({ page }) => {
      await openShell(page);
      const button = tabButton(page, tab.title);
      await expect(button, `tab button "${tab.title}"`).toBeVisible();
      await button.click();
      await expect(button).toHaveClass(/\bactive\b/);
      const pane = page.locator(`iframe.tab-pane[title="${tab.title}"]`);
      await expect(pane, `iframe pane for ${tab.name}`).toBeVisible();
      await expect(pane).toHaveAttribute("src", `/ui/${tab.name}/`);
      await expect
        .poll(async () => page.frame({ url: new RegExp(`/ui/${tab.name}/`) })?.title(), {
          message: `${tab.name} iframe document title`,
        })
        .toMatch(tab.uiTitle);
    });
  }

  for (const tab of NATIVE_TABS) {
    test(`native tab ${tab.title}: iframe ${tab.page} renders its page`, async ({ page }) => {
      await openShell(page);
      const button = tabButton(page, tab.title);
      await button.click();
      const pane = page.locator(`iframe.tab-pane[title="${tab.title}"]`);
      await expect(pane, `iframe pane for ${tab.name}`).toBeVisible();
      await expect(pane).toHaveAttribute("src", tab.page);
      const frame = page.frameLocator(`iframe.tab-pane[title="${tab.title}"]`);
      await expect(frame.locator("header h1"), `${tab.name} page heading`).toHaveText(tab.title);
    });
  }
});

// The shell's native bridges carry one corpus fixture each through the
// headless services' bidirectional RPCs: the upload goes up on the same
// stream the events come back on. A service whose model or VLM endpoint the
// stack does not provide is skipped by name, never passed silently.
test.describe("native tab bridges", () => {
  test("asr: formats/tone.wav streams through grpc-asr Transcribe", async ({ request }) => {
    const label = "[asr bridge | formats/tone.wav]";
    const status = await bridgeStatus(request, "/api/asr/status");
    expect(status.reachable, `${label} status: ${JSON.stringify(status)}`).toBe(true);
    const models = status.models ?? [];
    test.skip(models.length === 0, "grpc-asr reports no loaded whisper model (no ggml weights mounted); nothing can transcribe");
    const audio = corpusFile("formats/tone.wav");
    const lines = await postNdjson(
      request,
      `/api/asr/transcribe?model=${encodeURIComponent(models[0])}`,
      audio.buffer,
      label,
    );
    expectNoGrpcError(lines, label);
    const [media] = linesOfType(lines, "media");
    expect(media, `${label} no media line: ${JSON.stringify(lines)}`).toBeDefined();
    // One second of 440 Hz tone at 8 kHz mono (tests/golden/build_format_fixtures.py).
    expect(Number(media.durationMs), `${label} media duration`).toBeGreaterThan(500);
    expect(Number(media.durationMs), `${label} media duration`).toBeLessThan(1500);
    expect(linesOfType(lines, "complete"), `${label} no complete trailer`).toHaveLength(1);
    expect(lines.at(-1)?.type, `${label} last line`).toBe("done");
  });

  test("enrich: bar_chart.png chart extraction streams through grpc-enrich EnrichDocument", async ({ request }, testInfo) => {
    const label = "[enrich bridge | bar_chart.png]";
    const status = await bridgeStatus(request, "/api/enrich/status");
    expect(status.reachable, `${label} status: ${JSON.stringify(status)}`).toBe(true);
    if (status.vlmConfigured === false) {
      // Reachable without a VLM: the chart job is answered with a skip,
      // which is still one whole round trip on the stream.
      testInfo.annotations.push({ type: "degraded", description: "enrich: no VLM endpoint configured; the chart job skips" });
    }
    const chart = corpusFile("bar_chart.png");
    const lines = await postNdjson(
      request,
      "/api/enrich/annotate",
      { image: { base64: chart.buffer.toString("base64"), mime: chart.mimeType }, chart: true },
      label,
    );
    expectNoGrpcError(lines, label);
    const [started] = linesOfType(lines, "started");
    expect(started, `${label} no started line: ${JSON.stringify(lines)}`).toBeDefined();
    expect(started.chartExtractions, `${label} chart jobs planned`).toBe(1);
    const [complete] = linesOfType(lines, "complete");
    expect(complete, `${label} no complete trailer: ${JSON.stringify(lines)}`).toBeDefined();
    const answered = Number(complete.succeeded) + Number(complete.skipped) + Number(complete.failed);
    expect(answered, `${label} the chart job was never answered: ${JSON.stringify(complete)}`).toBe(1);
    const charts = linesOfType(lines, "annotation").filter((line) => line.kind === "chart");
    const skipped = linesOfType(lines, "skipped");
    expect(charts.length + skipped.length, `${label} neither a chart table nor a skip came back`).toBeGreaterThan(0);
  });

  test("vlm-convert: report_page.png streams through grpc-vlm-convert ConvertPages", async ({ request }) => {
    const label = "[vlm-convert bridge | report_page.png]";
    const status = await bridgeStatus(request, "/api/vlm-convert/status");
    expect(status.reachable, `${label} status: ${JSON.stringify(status)}`).toBe(true);
    test.skip(
      status.vlmConfigured !== true,
      "grpc-vlm-convert has no VLM endpoint (VLM_ENDPOINT unset in the stack); every conversion fails FAILED_PRECONDITION by contract",
    );
    const page = corpusFile("report_page.png");
    const { width, height } = pngSize(page.buffer);
    const lines = await postNdjson(
      request,
      "/api/vlm-convert/convert",
      { pages: [{ pageNo: 1, png: page.buffer.toString("base64"), width, height }] },
      label,
    );
    expectNoGrpcError(lines, label);
    const pages = linesOfType(lines, "page");
    expect(pages, `${label} page documents: ${JSON.stringify(lines)}`).toHaveLength(1);
    expect(Number(pages[0].texts) + Number(pages[0].tables), `${label} the page converted to nothing`).toBeGreaterThan(0);
    const [complete] = linesOfType(lines, "complete");
    expect(complete?.pagesOk, `${label} complete trailer: ${JSON.stringify(complete)}`).toBe(1);
  });
});
