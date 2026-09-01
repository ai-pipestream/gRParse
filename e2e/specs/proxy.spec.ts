import { test, expect } from "../lib/test";
import { corpusFile } from "../lib/corpus";

// Regression probe for a shell proxy fault that was live until the 2026-09-01
// dependency refresh: after the shell proxied a streamed POST to a service
// frontend (/ui/<name>/api/parse), the next request it forwarded to that
// frontend came back 400 with an empty body. Behind nginx 1.31 the follow-up
// is served on both stacks (five pairs out of five, twice); behind 1.27 it
// was refused in most pairs. The shell-side mechanism (server.js
// proxyToFrontend reusing an upstream socket) was never root-caused, so the
// probe stays, with several stream/request pairs because the fault was a
// race, and fails the run if the symptom returns.
const PAIRS = 5;
test.describe("shell proxy", () => {
  test("the request after a proxied parse stream is served (not 400)", async ({ request }) => {
    const fixture = corpusFile("streaming-markup.md");
    const followUps: number[] = [];
    for (let i = 0; i < PAIRS; i++) {
      const parse = await request.post("/ui/markup/api/parse?format=MARKUP_FORMAT_MARKDOWN&delayMs=0&chunkBytes=1024", {
        data: fixture.buffer,
        headers: { "content-type": "application/octet-stream" },
      });
      expect(parse.status(), `proxied parse stream ${i + 1}`).toBe(200);
      await parse.body();
      const next = await request.get("/ui/markup/");
      followUps.push(next.status());
      // Let the frontend's keep-alive window close so every pair starts clean.
      await new Promise((resolve) => setTimeout(resolve, 6000));
    }
    expect(followUps, `GET /ui/markup/ right after each of ${PAIRS} streams`).toEqual(Array(PAIRS).fill(200));
  });
});
