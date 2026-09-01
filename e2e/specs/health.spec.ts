import { test, expect } from "../lib/test";
import { NATIVE_TABS, REGISTRY_TABS, openShell, tabButton, type UiEntry } from "../lib/shell";

interface StatusPayload {
  reachable?: boolean;
  vlmConfigured?: boolean;
  version?: string;
  serviceVersion?: string;
}

test.describe("status endpoints", () => {
  test("gRParse bridge /api/health is ok", async ({ request }) => {
    const response = await request.get("/api/health");
    expect(response.ok(), `GET /api/health -> HTTP ${response.status()}`).toBe(true);
    const body = (await response.json()) as { ok?: boolean; error?: string; target?: string };
    expect(body.ok, `gRParse unhealthy: ${body.error ?? "no error text"}`).toBe(true);
  });

  for (const tab of NATIVE_TABS) {
    test(`native tab ${tab.title}: ${tab.status} reports reachable`, async ({ request }, testInfo) => {
      const response = await request.get(tab.status);
      expect(response.ok(), `GET ${tab.status} -> HTTP ${response.status()}`).toBe(true);
      const body = (await response.json()) as StatusPayload;
      expect(body.reachable, `${tab.name} status payload: ${JSON.stringify(body)}`).toBe(true);
      if (body.vlmConfigured === false) {
        // Reachable but degraded (amber dot): the service is up and its
        // external VLM endpoint is not. That is still a healthy stack.
        testInfo.annotations.push({ type: "degraded", description: `${tab.name}: no VLM endpoint configured` });
      }
    });
  }

  test("every registry tab in /api/uis reports reachable", async ({ request }) => {
    const response = await request.get("/api/uis");
    expect(response.ok(), `GET /api/uis -> HTTP ${response.status()}`).toBe(true);
    const { uis } = (await response.json()) as { uis: UiEntry[] };
    const unreachable = uis.filter((entry) => !entry.reachable).map((entry) => entry.name);
    expect(unreachable, "registry tabs whose info RPC did not answer").toEqual([]);
    expect(uis.map((entry) => entry.name)).toEqual(expect.arrayContaining(REGISTRY_TABS.map((tab) => tab.name)));
  });

  test("shell tab dots are green (or amber for a VLM tab without an endpoint)", async ({ page }) => {
    await openShell(page);
    for (const tab of NATIVE_TABS) {
      await expect(tabButton(page, tab.title).locator(".tab-dot"), `${tab.name} native tab dot`).toHaveClass(
        /\btab-dot (ok|warn)\b/,
      );
    }
    for (const tab of REGISTRY_TABS) {
      await expect(tabButton(page, tab.title).locator(".tab-dot"), `${tab.name} registry tab dot`).toHaveClass(
        /\btab-dot ok\b/,
      );
    }
  });
});
