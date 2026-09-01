import type { Locator, Page } from "@playwright/test";
import { test, expect } from "../lib/test";
import { corpusFile } from "../lib/corpus";
import { openShell } from "../lib/shell";

// True when at least one pixel of the overlay canvas was painted: every
// text, table and picture box is stroked onto it, so a blank canvas means
// the page event carried no boxes (or the renderer skipped them).
async function canvasPainted(canvas: Locator): Promise<boolean> {
  return canvas.evaluate((element) => {
    const context = (element as HTMLCanvasElement).getContext("2d");
    if (!context) return false;
    const { data } = context.getImageData(0, 0, (element as HTMLCanvasElement).width, (element as HTMLCanvasElement).height);
    for (let index = 3; index < data.length; index += 4) if (data[index] !== 0) return true;
    return false;
  });
}

async function expectTwoStreamedPages(page: Page, label: string) {
  const cards = page.locator("#results .page-card");
  await expect(cards, `${label} page cards`).toHaveCount(2, { timeout: 120_000 });
  // The collector-document banner shares the class; the terminal one
  // starts with "Complete".
  await expect(page.locator("#results .banner.done", { hasText: /^Complete/ }), `${label} completion banner`).toHaveText(
    "Complete: 2 page(s).",
    { timeout: 60_000 },
  );
  await expect(page.locator("#stat-pages"), `${label} pages stat`).toHaveText("2/2");
  await expect(page.locator("#stat-texts"), `${label} text items stat`).toHaveText(/^[1-9]\d*$/);
  await expect(page.locator("#results .banner.error"), `${label} stream error`).toHaveCount(0);
  for (let index = 0; index < 2; index += 1) {
    const card = cards.nth(index);
    await expect(card.locator("h2"), `${label} page ${index + 1} heading`).toContainText(`Page ${index + 1} of 2`);
    await expect(card.locator(".page-canvas canvas"), `${label} page ${index + 1} canvas`).toBeAttached();
    expect(await canvasPainted(card.locator(".page-canvas canvas")), `${label} page ${index + 1} drew no boxes`).toBe(true);
    // GRPARSE_PAGE_IMAGES=on in the stack: the preview sits under the boxes.
    await expect(card.locator(".page-canvas img"), `${label} page ${index + 1} preview`).toBeAttached();
  }
}

test.describe("gRParse page-stream tab", () => {
  test("service health chip is green", async ({ page }) => {
    await openShell(page);
    await expect(page.locator("#health")).toHaveClass(/\bok\b/);
    await expect(page.locator("#health")).toContainText("service healthy");
  });

  test("bundled two-page sample streams two page cards with boxes", async ({ page }) => {
    await openShell(page);
    await expect(page.locator("#health")).toHaveClass(/\bok\b/);
    await page.locator("#sample-button").click();
    await expectTwoStreamedPages(page, "[grparse tab | sample.pdf]");
    // Page 2 of the sample is a raster page: OCR must have contributed.
    await expect(page.locator("#stat-ocr")).toHaveText(/^[1-9]\d*$/);
  });

  test("corpus hello-text.pdf (two pages) streams through the upload input", async ({ page }) => {
    await openShell(page);
    await expect(page.locator("#health")).toHaveClass(/\bok\b/);
    await page.locator("#grparse-body #file-input").setInputFiles(corpusFile("hello-text.pdf"));
    await expectTwoStreamedPages(page, "[grparse tab | hello-text.pdf]");
  });
});
