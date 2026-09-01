import { test as base, expect } from "@playwright/test";
import { ConsoleGate } from "./console-gate";

// Every spec imports `test` from here. The auto fixture arms the console
// gate before the test body runs and asserts it is clean afterwards, so a
// spec cannot forget the no-console-errors check.
export const test = base.extend<{ consoleGate: ConsoleGate }>({
  consoleGate: [
    async ({ page }, use, testInfo) => {
      const gate = new ConsoleGate(page);
      await use(gate);
      expect(
        gate.findings,
        `console errors during "${testInfo.title}" (final page ${page.url()}):\n${gate.describe()}`,
      ).toEqual([]);
    },
    { auto: true },
  ],
});

export { expect };
