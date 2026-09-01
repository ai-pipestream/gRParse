import { test, expect } from "../lib/test";
import {
  DOCUMENT_FIXTURES,
  describeFixture,
  headings,
  openDocumentTab,
  pictures,
  tables,
  textBearingItems,
  uploadThroughDocumentTab,
} from "../lib/document-tab";
import { openShell, tabButton } from "../lib/shell";

test.describe("Document tab", () => {
  test("status dot is green in the shell tab bar", async ({ page }) => {
    await openShell(page);
    await expect(tabButton(page, "Document").locator(".tab-dot")).toHaveClass(/\btab-dot ok\b/);
    await expect(tabButton(page, "Document").locator(".tab-dot")).toHaveAttribute("title", "service reachable");
  });

  // Known UI bug, kept red on purpose: style.css gives .dv-progress
  // `display: flex`, which outranks the `hidden` attribute finishRun() sets,
  // so the "parsing..." bar stays painted after every parse. This test flips
  // to an unexpected pass (and fails the run) once the stylesheet is fixed;
  // drop the test.fail() then.
  test("progress bar disappears once the parse finishes", async ({ page }) => {
    test.fail(true, "document.html: .dv-progress { display: flex } overrides [hidden]; the bar stays visible");
    const fixture = DOCUMENT_FIXTURES.find((candidate) => candidate.id === "html")!;
    await openDocumentTab(page);
    await uploadThroughDocumentTab(page, fixture);
    await expect(page.locator("#progress"), `${describeFixture(fixture)} progress bar still painted`).toBeHidden({ timeout: 5_000 });
  });

  for (const fixture of DOCUMENT_FIXTURES) {
    test(`${fixture.id} via ${fixture.parser}: ${fixture.file} renders the merged document`, async ({ page }) => {
      const label = describeFixture(fixture);
      await openDocumentTab(page);
      await uploadThroughDocumentTab(page, fixture);

      // Baseline: the info bar names the file, at least one item carries
      // text, and the legend attributes items to the expected collector.
      await expect(page.locator("#doc-info .dv-info-name"), `${label} info bar name`).toHaveText(fixture.file);
      const items = textBearingItems(page);
      expect(await items.count(), `${label} rendered no text-bearing item`).toBeGreaterThan(0);
      await expect(page.locator("#legend-collectors"), `${label} collector legend`).toContainText(fixture.collector);

      for (const structure of fixture.structure) {
        if (structure === "headings") {
          expect(await headings(page).count(), `${label} rendered no heading`).toBeGreaterThan(0);
        } else if (structure === "table") {
          expect(await tables(page).count(), `${label} rendered no table`).toBeGreaterThan(0);
          expect(
            await tables(page).locator("td, th").filter({ hasText: /\S/ }).count(),
            `${label} rendered tables with no cell text`,
          ).toBeGreaterThan(0);
        } else if (structure === "picture") {
          expect(await pictures(page).count(), `${label} rendered no picture`).toBeGreaterThan(0);
        }
      }
    });
  }
});
