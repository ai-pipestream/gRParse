import { expect, type APIRequestContext } from "@playwright/test";

// The shell's NDJSON bridges (examples/web-demo/server.js): one POST carries
// the upload, the answer is one JSON object per line as the service's
// bidirectional stream produces events. Every line has a `type`.
export interface BridgeLine {
  type: string;
  [key: string]: unknown;
}

export const BRIDGE_TIMEOUT_MS = 150_000;

// POSTs `body` (raw bytes, or an object sent as JSON) and returns every line
// of the NDJSON answer, failing with the HTTP status and body when the
// bridge refused the request outright.
export async function postNdjson(
  request: APIRequestContext,
  url: string,
  body: Buffer | object,
  label: string,
): Promise<BridgeLine[]> {
  const raw = Buffer.isBuffer(body);
  const response = await request.post(url, {
    data: body,
    headers: { "content-type": raw ? "application/octet-stream" : "application/json" },
    timeout: BRIDGE_TIMEOUT_MS,
  });
  const text = await response.text();
  expect(response.ok(), `${label} POST ${url} -> HTTP ${response.status()}: ${text.slice(0, 500)}`).toBe(true);
  return text
    .split("\n")
    .filter((line) => line.trim() !== "")
    .map((line) => JSON.parse(line) as BridgeLine);
}

export function linesOfType(lines: BridgeLine[], type: string): BridgeLine[] {
  return lines.filter((line) => line.type === type);
}

// The stream failed when the bridge relayed a gRPC error line.
export function expectNoGrpcError(lines: BridgeLine[], label: string) {
  const errors = lines.filter((line) => line.type === "grpc-error" || line.type === "error");
  expect(errors, `${label} stream relayed an error: ${JSON.stringify(errors)}`).toEqual([]);
}

// A PNG's pixel size from its IHDR chunk (bytes 16..23, big-endian).
export function pngSize(png: Buffer): { width: number; height: number } {
  expect(png.subarray(1, 4).toString("latin1"), "fixture is not a PNG").toBe("PNG");
  return { width: png.readUInt32BE(16), height: png.readUInt32BE(20) };
}

// The status payload a native tab's /api/<name>/status answers.
export interface BridgeStatus {
  reachable?: boolean;
  vlmConfigured?: boolean;
  models?: string[];
  error?: string;
}

export async function bridgeStatus(request: APIRequestContext, url: string): Promise<BridgeStatus> {
  const response = await request.get(url);
  expect(response.ok(), `GET ${url} -> HTTP ${response.status()}`).toBe(true);
  return (await response.json()) as BridgeStatus;
}
