import type { FullConfig } from "@playwright/test";

// Readiness gate: the stack may still be booting when the runner starts
// (compose `up --wait` only knows a container is running, and gRParse loads
// its models after that). Poll the shell and the Document tab's status
// endpoint until both answer, bounded by E2E_READY_TIMEOUT_MS.
export default async function globalSetup(config: FullConfig) {
  const baseURL = config.projects[0]?.use?.baseURL;
  if (!baseURL) throw new Error("playwright.config.ts must set use.baseURL");
  const timeoutMs = Number(process.env.E2E_READY_TIMEOUT_MS ?? 180_000);
  const deadline = Date.now() + timeoutMs;
  let lastProblem = "not polled yet";

  while (Date.now() < deadline) {
    try {
      const uis = await fetch(new URL("/api/uis", baseURL), { signal: AbortSignal.timeout(5_000) });
      if (!uis.ok) throw new Error(`GET /api/uis -> HTTP ${uis.status}`);
      const status = await fetch(new URL("/api/document/status", baseURL), {
        signal: AbortSignal.timeout(5_000),
      });
      if (!status.ok) throw new Error(`GET /api/document/status -> HTTP ${status.status}`);
      const body = (await status.json()) as { reachable?: boolean; version?: string };
      if (!body.reachable) throw new Error(`gRParse not reachable yet: ${JSON.stringify(body)}`);
      console.log(`[e2e] stack ready at ${baseURL} (${body.version ?? "unknown version"})`);
      return;
    } catch (error) {
      lastProblem = error instanceof Error ? error.message : String(error);
      await new Promise((resolve) => setTimeout(resolve, 3_000));
    }
  }
  throw new Error(`[e2e] stack at ${baseURL} not ready after ${timeoutMs} ms: ${lastProblem}`);
}
