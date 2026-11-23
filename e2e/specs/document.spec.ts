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
import { expectNoGrpcError, linesOfType, postNdjson } from "../lib/bridges";
import { corpusFile } from "../lib/corpus";

// The protobuf-JSON shape of the few Document fields the chart derender
// case reads (examples/web-demo/server.js decodes with defaults on).
interface PictureJson {
  label?: string;
  image?: { uri?: string } | null;
  annotations?: { classification?: { predictedClasses?: { className?: string }[] } | null }[];
  meta?: { classification?: { predictions?: { className?: string }[] } | null; tabularChart?: unknown } | null;
}
interface DocumentJson {
  pictures?: PictureJson[];
  warnings?: { message?: string }[];
}

// gRParse's own rule for a picture the derender leg sends to grpc-enrich
// (chart_derender_candidates): a chart verdict and inline pixels.
const CHART_CLASSES = new Set(["bar_chart", "line_chart", "pie_chart"]);
function topClass(picture: PictureJson): string {
  for (const annotation of picture.annotations ?? []) {
    const predicted = annotation.classification?.predictedClasses ?? [];
    if (predicted.length > 0) return predicted[0].className ?? "";
  }
  return picture.meta?.classification?.predictions?.[0]?.className ?? "";
}
function chartVerdict(picture: PictureJson): boolean {
  return CHART_CLASSES.has(topClass(picture)) || picture.label === "DOC_ITEM_LABEL_CHART";
}

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

  // The chart derender leg: a raster chart goes through the CV path, and
  // gRParse sends the picture it classified as a chart to grpc-enrich over
  // EnrichDocument. The leg is advisory, so its outcome is either the
  // picture's tabular chart or a document warning saying why there is none;
  // a chart that produced neither never reached the leg.
  test("chart derender: bar_chart.png's chart goes from gRParse to grpc-enrich", async ({ request }) => {
    const label = "[document bridge | grparse CV + enrich | bar_chart.png]";
    const chart = corpusFile("bar_chart.png");
    const lines = await postNdjson(
      request,
      `/api/document/parse?filename=${encodeURIComponent(chart.name)}&contentType=${encodeURIComponent(chart.mimeType)}`,
      chart.buffer,
      label,
    );
    expectNoGrpcError(lines, label);
    const [line] = linesOfType(lines, "document");
    expect(line, `${label} no document line: ${JSON.stringify(lines).slice(0, 500)}`).toBeDefined();
    const doc = line.document as DocumentJson;
    const pictures = doc.pictures ?? [];
    const charts = pictures.filter(chartVerdict);
    test.skip(charts.length === 0, `no picture classified as a chart (top classes: ${JSON.stringify(pictures.map(topClass))}); the figure classifier is off in this stack, so the derender leg has nothing to send`);
    const sendable = charts.filter((picture) => (picture.image?.uri ?? "").startsWith("data:"));
    test.skip(sendable.length === 0, "the chart picture carries no inline image (GRPARSE_PICTURE_IMAGES off), so the derender leg has no pixels to send");
    const derendered = sendable.filter((picture) => Boolean(picture.meta?.tabularChart));
    const reported = (doc.warnings ?? []).filter((warning) => /chart derender|document enrich/.test(warning.message ?? ""));
    expect(
      derendered.length + reported.length,
      `${label} the chart came back with no table and no derender warning: warnings ${JSON.stringify(doc.warnings)}`,
    ).toBeGreaterThan(0);
  });
});
