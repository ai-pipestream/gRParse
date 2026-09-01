import { test, expect } from "../lib/test";
import { corpusFile } from "../lib/corpus";

// Known shell bug, kept red on purpose (see e2e/README.md, "Known red
// tests"): after the shell proxies a streamed POST to a service frontend
// (/ui/<name>/api/parse), the very next request the shell forwards to that
// same frontend is answered 400 with an empty body. Hitting the frontend
// directly inside the compose network does not reproduce it, so the fault is
// in examples/web-demo/server.js proxyToFrontend (a reused upstream socket
// left in a bad state), not in the frontends. One request is lost per
// stream; the frontend's keep-alive timeout (about 5 s) clears it.
//
// service-uis.spec.ts drains that one request deliberately after every
// stream so the functional suite stays deterministic. When this test flips
// to an unexpected pass the bug is fixed: drop the test.fail() here and the
// drain in lib/service-ui-flows.ts.
test.describe("shell proxy", () => {
  test("the request after a proxied parse stream is served (not 400)", async ({ request }) => {
    test.fail(true, "shell proxyToFrontend: first request after a streamed POST to the same frontend gets HTTP 400");
    const fixture = corpusFile("streaming-markup.md");
    const parse = await request.post("/ui/markup/api/parse?format=MARKUP_FORMAT_MARKDOWN&delayMs=0&chunkBytes=1024", {
      data: fixture.buffer,
      headers: { "content-type": "application/octet-stream" },
    });
    expect(parse.status(), "proxied parse stream").toBe(200);
    await parse.body();
    const next = await request.get("/ui/markup/");
    expect(next.status(), "GET /ui/markup/ right after the stream").toBe(200);
  });
});
