import { test, expect } from "../lib/test";
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
