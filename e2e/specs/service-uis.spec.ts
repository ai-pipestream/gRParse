import type { Page } from "@playwright/test";
import { test } from "../lib/test";
import { SERVICE_UIS, uiPath } from "../lib/service-uis";
import { openUi, renderThroughUi, streamThroughUi } from "../lib/service-ui-flows";
import type { UiEntry } from "../lib/shell";

async function listedUis(page: Page): Promise<Set<string>> {
  const response = await page.request.get("/api/uis");
  const { uis } = (await response.json()) as { uis: UiEntry[] };
  return new Set(uis.map((entry) => entry.name));
}

test.describe("service UIs under /ui/<name>/", () => {
  for (const ui of SERVICE_UIS) {
    const verb = ui.flow === "load-only" ? "renders" : "parses a fixture";
    test(`${ui.name}: loads under ${uiPath(ui)} and ${verb}`, async ({ page }, testInfo) => {
      if (ui.optional) {
        const listed = await listedUis(page);
        test.skip(!listed.has(ui.name), `${ui.name} is not in /api/uis (opt-in compose profile not running)`);
      }
      await openUi(page, ui);
      if (ui.flow === "stream") await streamThroughUi(page, ui, testInfo);
      else if (ui.flow === "render") await renderThroughUi(page, ui, testInfo);
    });
  }
});
