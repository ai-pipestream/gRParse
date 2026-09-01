import type { Page } from "@playwright/test";

export interface ConsoleFinding {
  kind: "console.error" | "pageerror";
  text: string;
  url: string;
}

// Collects every console error and uncaught exception a page (and the
// iframes inside it) emits during a test. The `test` fixture in ./test.ts
// asserts the list is empty when the test ends, so a UI that logs an error
// fails its spec even when every functional assertion passed.
export class ConsoleGate {
  readonly findings: ConsoleFinding[] = [];

  constructor(private readonly page: Page) {
    page.on("console", (message) => {
      if (message.type() !== "error") return;
      this.findings.push({
        kind: "console.error",
        text: message.text(),
        url: message.location().url || page.url(),
      });
    });
    page.on("pageerror", (error) => {
      this.findings.push({ kind: "pageerror", text: error.message, url: page.url() });
    });
  }

  describe(): string {
    return this.findings
      .map((finding, index) => `${index + 1}. [${finding.kind}] ${finding.text}\n   at ${finding.url}`)
      .join("\n");
  }
}
