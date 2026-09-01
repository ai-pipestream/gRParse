import { expect, type Page, type TestInfo } from "@playwright/test";
import { corpusFile } from "./corpus";
import { describeUi, uiPath, type ServiceUi } from "./service-uis";

export const STREAM_TIMEOUT_MS = 90_000;

export async function openUi(page: Page, ui: ServiceUi) {
  await page.goto(uiPath(ui), { waitUntil: "domcontentloaded" });
  await expect(page, `${describeUi(ui)} title`).toHaveTitle(ui.title);
}

// The shared demo page: pick a file (or keep the bundled sample), press
// run, wait for the button to come back, read the counters.
export async function streamThroughUi(page: Page, ui: ServiceUi, testInfo: TestInfo) {
  const label = describeUi(ui);
  const run = page.locator("#run");
  await expect(run, `${label} run button`).toBeEnabled();
  if (ui.fixture) await page.locator("#pick").setInputFiles(corpusFile(ui.fixture));
  else await expect(page.locator("#sample option"), `${label} bundled samples`).not.toHaveCount(0);
  await run.click();
  await expect(run, `${label} stream never finished`).toBeEnabled({ timeout: STREAM_TIMEOUT_MS });
  const verdict = page.locator("#verdict");
  await expect(verdict, `${label} verdict stayed idle: "${await verdict.textContent()}"`).not.toHaveClass(/\bidle\b/);
  await expect(page.locator(ui.stat!), `${label} ${ui.statLabel} counter stayed at zero`).toHaveText(/[1-9]/);
  await drainPoisonedProxyRequest(page, ui, testInfo);
}

// The libreoffice frontend: selecting a file starts the render.
export async function renderThroughUi(page: Page, ui: ServiceUi, testInfo: TestInfo) {
  const label = describeUi(ui);
  await page.locator("#file-input").setInputFiles(corpusFile(ui.fixture!));
  await expect(page.locator("#doc-card"), `${label} document card never appeared`).not.toHaveClass(/\bhidden\b/, {
    timeout: STREAM_TIMEOUT_MS,
  });
  await expect(page.locator("#drop-zone"), `${label} render never finished`).not.toHaveClass(/\bbusy\b/, {
    timeout: STREAM_TIMEOUT_MS,
  });
  const errorBanner = page.locator("#error-banner");
  await expect(errorBanner, `${label} error banner: "${await errorBanner.textContent()}"`).toHaveClass(/\bhidden\b/);
  await expect(page.locator("#page-grid .page-cell img").first(), `${label} rendered no page image`).toBeAttached();
  await expect(page.locator("#doc-pages"), `${label} page count`).toContainText(/[1-9]/);
  await drainPoisonedProxyRequest(page, ui, testInfo);
}

// Workaround for the shell bug proxy.spec.ts keeps red: the first request
// the shell forwards to a frontend after a streamed POST is lost (HTTP 400).
// Spending that request here, outside the browser, keeps the next test's
// page load (or the shell's iframe) from being the victim. The outcome is
// recorded as an annotation so the report shows the bug is still live.
// Remove together with the test.fail() in proxy.spec.ts.
async function drainPoisonedProxyRequest(page: Page, ui: ServiceUi, testInfo: TestInfo) {
  const response = await page.request.get(uiPath(ui));
  testInfo.annotations.push({
    type: "proxy-drain",
    description: `GET ${uiPath(ui)} after the stream -> HTTP ${response.status()} (400 = shell proxy bug still present)`,
  });
}
