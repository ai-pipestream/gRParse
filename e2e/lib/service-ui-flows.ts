import { expect, type Page } from "@playwright/test";
import { corpusFile } from "./corpus";
import { describeUi, uiPath, type ServiceUi } from "./service-uis";

export const STREAM_TIMEOUT_MS = 90_000;

export async function openUi(page: Page, ui: ServiceUi) {
  await page.goto(uiPath(ui), { waitUntil: "domcontentloaded" });
  await expect(page, `${describeUi(ui)} title`).toHaveTitle(ui.title);
}

// The shared demo page: pick a file (or keep the bundled sample), press
// run, wait for the button to come back, read the counters.
export async function streamThroughUi(page: Page, ui: ServiceUi) {
  const label = describeUi(ui);
  const run = page.locator("#run");
  await expect(run, `${label} run button`).toBeEnabled();
  if (ui.layout !== undefined) await setEbcdicOptions(page, ui);
  if (ui.fixture) await page.locator("#pick").setInputFiles(corpusFile(ui.fixture));
  else await expect(page.locator("#sample option"), `${label} bundled samples`).not.toHaveCount(0);
  await run.click();
  await expect(run, `${label} stream never finished`).toBeEnabled({ timeout: STREAM_TIMEOUT_MS });
  const verdict = page.locator("#verdict");
  await expect(verdict, `${label} verdict stayed idle: "${await verdict.textContent()}"`).not.toHaveClass(/\bidle\b/);
  await expect(page.locator(ui.stat!), `${label} ${ui.statLabel} counter stayed at zero`).toHaveText(/[1-9]/);
  if (ui.emitDocument) {
    await expect(page.locator("#log"), `${label} no folded Document came back`).toContainText("folded Document received");
  }
}

// The ebcdic frontend loads its first bundled sample's companion layout into
// `#layout` once the page is up; the row's own layout is typed over it only
// after that, so the late load cannot overwrite it.
async function setEbcdicOptions(page: Page, ui: ServiceUi) {
  const label = describeUi(ui);
  await expect(page.locator("#sample option"), `${label} bundled samples`).not.toHaveCount(0);
  await expect(page.locator("#layout"), `${label} companion layout never loaded`).not.toHaveValue("");
  await page.locator("#layout").fill(ui.layout!);
  await expect(page.locator("#layout"), `${label} layout box`).toHaveValue(ui.layout!);
  if (ui.encoding) {
    const options = await page.locator("#encoding option").allTextContents();
    const encoding = options.find((option) => ui.encoding!.test(option));
    expect(encoding, `${label} code page ${ui.encoding} not offered: ${JSON.stringify(options)}`).toBeDefined();
    await page.locator("#encoding").selectOption(encoding!);
  }
  if (ui.emitDocument) await page.locator("#emitDoc").check();
}

// The libreoffice frontend: selecting a file starts the render.
export async function renderThroughUi(page: Page, ui: ServiceUi) {
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
}

