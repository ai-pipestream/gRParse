import { expect, type Page } from "@playwright/test";
import { corpusFile } from "./corpus";

// Structural expectations the Document tab must render for a fixture, on
// top of the baseline every row gets (viewer built, no error banner, at
// least one text-bearing item, the producing collector in the legend).
export type DocumentStructure = "headings" | "table" | "picture";

export interface DocumentFixture {
  // Short id used in test titles ("pdf", "docx", ...).
  id: string;
  file: string;
  // Which parser the routing table sends this format to, for the report.
  parser: string;
  // Collector name the viewer's legend must list (CollectorSource.collector).
  collector: string;
  structure: DocumentStructure[];
  // Upper bound for the parse; OCR rows get more.
  timeoutMs?: number;
}

// One row per parser the stack routes by format (README "Collector
// scatter-gather"). Adding a parser is one row.
export const DOCUMENT_FIXTURES: DocumentFixture[] = [
  { id: "pdf", file: "two-column.pdf", parser: "pdf-inspector", collector: "pdf", structure: ["headings"] },
  { id: "docx", file: "figures.docx", parser: "libreoffice", collector: "libreoffice", structure: ["picture"] },
  { id: "xlsx", file: "sample3.xlsx", parser: "libreoffice", collector: "libreoffice", structure: ["table"] },
  { id: "pptx", file: "notes.pptx", parser: "libreoffice", collector: "libreoffice", structure: ["headings", "table"] },
  // HTML with no selector routes to markup; lol-html is explicit-only.
  { id: "html", file: "entities.html", parser: "markup", collector: "markup", structure: ["headings"] },
  { id: "md", file: "streaming-markup.md", parser: "markup", collector: "markup", structure: ["headings", "table", "picture"] },
  { id: "xml", file: "jats-article.xml", parser: "xml", collector: "xml", structure: ["table", "picture"] },
  { id: "eml", file: "multipart_with_attachments.eml", parser: "email", collector: "email", structure: ["picture"] },
  // The epub skeleton is completed through markup, so the items carry the
  // markup collector tag (README, COLLECTOR_EPUB row).
  { id: "epub", file: "two-chapters.epub", parser: "epub + markup", collector: "markup", structure: ["headings", "picture"] },
  { id: "png", file: "report_page.png", parser: "grparse OCR", collector: "grparse", structure: ["headings", "table", "picture"], timeoutMs: 150_000 },
  { id: "scanned-pdf", file: "rotated-scan.pdf", parser: "grparse OCR", collector: "grparse", structure: ["headings"], timeoutMs: 150_000 },
];

export function describeFixture(fixture: DocumentFixture): string {
  return `[document tab | ${fixture.parser} | ${fixture.file}]`;
}

export async function openDocumentTab(page: Page) {
  await page.goto("/document.html", { waitUntil: "domcontentloaded" });
  // The header chip follows GET /api/document/status.
  await expect(page.locator("#health"), "Document tab status chip").toHaveClass(/\bok\b/);
}

// Uploads the fixture through the tab's file input and waits for the
// tab's own completion signals: the progress bar hides and the info bar
// appears once the `document` line lands. Page cards build lazily as they
// scroll into view, so every card is scrolled once before assertions run.
export async function uploadThroughDocumentTab(page: Page, fixture: DocumentFixture) {
  const label = describeFixture(fixture);
  const timeout = fixture.timeoutMs ?? 90_000;
  await page.locator("#file-input").setInputFiles(corpusFile(fixture.file));
  await expect(page.locator("#doc-info"), `${label} viewer never rendered`).toBeVisible({ timeout });
  // finishRun(): the dropzone drops `busy` and the progress bar is flagged
  // hidden once the NDJSON stream closes. The hidden flag is read as a DOM
  // property on purpose; see the "progress bar disappears" test in
  // document.spec.ts for why the bar is still painted.
  await expect(page.locator("#dropzone"), `${label} upload never finished`).not.toHaveClass(/\bbusy\b/, { timeout });
  await expect(page.locator("#progress"), `${label} progress bar not flagged hidden`).toHaveJSProperty("hidden", true);
  const errors = page.locator("#results .banner.error");
  await expect(errors, `${label} error banner: ${await errors.allTextContents()}`).toHaveCount(0);
  await revealAllPageCards(page);
}

export async function revealAllPageCards(page: Page) {
  const cards = page.locator("#results .dv-page-card");
  const count = await cards.count();
  for (let index = 0; index < count; index += 1) {
    const card = cards.nth(index);
    await card.scrollIntoViewIfNeeded();
    await expect(card.locator(".dv-page-content"), `page card ${index + 1} of ${count} never built`).toBeAttached();
  }
  await page.evaluate(() => window.scrollTo(0, 0));
}

// Rendered content elements. `.dv-item` covers every item kind the viewer
// draws (text, tables, figures, code, ...); the text filter keeps the
// baseline honest for spreadsheets, whose text lives in table cells.
export function contentItems(page: Page) {
  return page.locator("#results .dv-item");
}

export function textBearingItems(page: Page) {
  return contentItems(page).filter({ hasText: /\S/ });
}

export function headings(page: Page) {
  return page.locator("#results .dv-heading, #results .dv-title");
}

export function tables(page: Page) {
  return page.locator("#results table.dv-table");
}

export function pictures(page: Page) {
  return page.locator("#results figure.dv-figure");
}
