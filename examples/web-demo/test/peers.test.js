// Unit tests for the peer registry and the vendored proto trees. They
// never dial anything: everything loads from peer-protos/ or from a
// throwaway workspace built in a temp dir.
"use strict";

const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test } = require("node:test");

const peers = require("../peers");

const demoDir = path.resolve(__dirname, "..");
const syncScript = path.join(demoDir, "sync-peer-protos.sh");

test("every registry entry is complete and its proto sits under its include root", () => {
  for (const [name, known] of Object.entries(peers.KNOWN_UIS)) {
    for (const key of ["repo", "proto", "include", "service", "method"]) {
      assert.ok(known[key], `${name}.${key} is set`);
    }
    assert.ok(known.proto.startsWith(`${known.include}/`), `${name}: ${known.proto} is under ${known.include}`);
  }
});

test("includeTrees lists each (repo, include) pair once, in registry order", () => {
  const trees = peers.includeTrees();
  const keys = trees.map((tree) => `${tree.repo}/${tree.include}`);
  assert.equal(new Set(keys).size, keys.length);
  assert.equal(keys[0], "grpc-lol-html/proto");
  assert.ok(keys.includes("grPOIc/grpoic-api/src/main/proto"));
});

test("every vendored contract loads with its transitive imports and declares its service", () => {
  const rows = peers.checkProtos([peers.PEER_PROTOS_DIR]);
  const failures = rows.filter((row) => row.error);
  assert.deepEqual(failures, []);
  assert.equal(rows.length, Object.keys(peers.KNOWN_UIS).length);
  for (const row of rows) assert.ok(row.file.startsWith(peers.PEER_PROTOS_DIR));
});

test("a missing tree is reported by name, not thrown", () => {
  const rows = peers.checkProtos([path.join(os.tmpdir(), "no-such-workspace")]);
  assert.ok(rows.every((row) => row.error && row.error.includes("not found under")));
  const lines = [];
  assert.equal(peers.reportCheck(rows, (line) => lines.push(line)), 1);
  assert.ok(lines.some((line) => line.startsWith("FAIL lol-html:")));
});

test("vendored document.proto copies are identical to each other", () => {
  const copies = [];
  for (const tree of peers.includeTrees()) {
    const copy = path.join(peers.PEER_PROTOS_DIR, tree.repo, tree.include, "ai/pipestream/document/v1/document.proto");
    if (fs.existsSync(copy)) copies.push(fs.readFileSync(copy));
  }
  assert.ok(copies.length >= 8, "the Document-plane peers carry a copy");
  for (const copy of copies) assert.ok(copy.equals(copies[0]));
});

test("peers.js --list prints repo and include per line", () => {
  const result = spawnSync(process.execPath, [path.join(demoDir, "peers.js"), "--list"], { encoding: "utf8" });
  assert.equal(result.status, 0, result.stderr);
  const lines = result.stdout.trim().split("\n");
  assert.equal(lines.length, peers.includeTrees().length);
  assert.ok(lines.every((line) => line.split("\t").length === 2));
});

// Builds a fake workspace from the vendored trees themselves, so the sync
// script's check mode can be exercised without sibling checkouts.
function fakeWorkspace() {
  const ws = fs.mkdtempSync(path.join(os.tmpdir(), "peer-protos-ws-"));
  for (const tree of peers.includeTrees()) {
    fs.cpSync(path.join(peers.PEER_PROTOS_DIR, tree.repo), path.join(ws, tree.repo), { recursive: true });
  }
  return ws;
}

function runSync(ws, ...args) {
  return spawnSync("bash", [syncScript, "--workspace", ws, ...args], { encoding: "utf8" });
}

test("sync-peer-protos.sh --check passes against a workspace equal to the vendored copy", () => {
  const ws = fakeWorkspace();
  const result = runSync(ws, "--check");
  assert.equal(result.status, 0, result.stdout + result.stderr);
});

test("sync-peer-protos.sh --check fails and names a drifted file", () => {
  const ws = fakeWorkspace();
  const drifted = path.join(ws, "grpc-lol-html/proto/lolhtml/v1/types.proto");
  fs.appendFileSync(drifted, "\n// drift\n");
  const result = runSync(ws, "--check");
  assert.equal(result.status, 1);
  assert.match(result.stdout, /drift: .*lolhtml\/v1\/types\.proto/);
});

test("sync-peer-protos.sh --check fails when a sibling tree is missing", () => {
  const ws = fakeWorkspace();
  fs.rmSync(path.join(ws, "grpc-xml"), { recursive: true });
  const result = runSync(ws, "--check");
  assert.equal(result.status, 1);
  assert.match(result.stderr, /missing: .*grpc-xml\/proto/);
});
