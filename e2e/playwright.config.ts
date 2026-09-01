import { defineConfig, devices } from "@playwright/test";

// One target, one browser. E2E_BASE_URL points at the stack's front door
// (the nginx proxy): the host-published port when running from a checkout,
// http://proxy:8080 inside the compose network (compose.stack.e2e.yaml).
const baseURL = process.env.E2E_BASE_URL ?? "http://127.0.0.1:18081";

// The parsers are shared by every worker, and OCR on a CPU-only host is
// slow, so the default parallelism is modest. E2E_WORKERS overrides it.
const workers = Number(process.env.E2E_WORKERS ?? 2);

export default defineConfig({
  testDir: "./specs",
  outputDir: "./test-results",
  globalSetup: "./lib/global-setup.ts",
  fullyParallel: false,
  forbidOnly: Boolean(process.env.CI),
  retries: 0,
  workers,
  // One upload through OCR on a CPU host can take well over 30 s; every
  // wait in the specs is bounded below this.
  timeout: 180_000,
  expect: { timeout: 20_000 },
  reporter: [
    ["list"],
    ["html", { outputFolder: "out/html", open: "never" }],
    ["junit", { outputFile: "out/junit.xml" }],
  ],
  use: {
    baseURL,
    trace: "off",
    video: "off",
    screenshot: "only-on-failure",
    actionTimeout: 20_000,
    navigationTimeout: 30_000,
  },
  projects: [
    {
      name: "chromium",
      use: { ...devices["Desktop Chrome"] },
    },
  ],
});
