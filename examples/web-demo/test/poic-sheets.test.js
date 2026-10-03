// Unit tests for the grPOIc sheet-batch folder the /api/poic/parse relay
// uses. Events are shaped as proto-loader decodes them (camelCase fields).
"use strict";

const assert = require("node:assert/strict");
const { test } = require("node:test");

const { createSheetFolder } = require("../poic-sheets");

const row = (...values) => ({ cells: values.map((formatted) => ({ formatted })) });
const sheet = (index, name, rows, moreRows = false, extra = {}) => ({ index, name, rows, moreRows, ...extra });
const identity = (text) => text;

test("a sheet that fits one batch is one preview", () => {
  const folder = createSheetFolder(identity);
  const out = folder.add(sheet(0, "Totals", [row("a", "b"), row("c", "d")]));
  assert.deepEqual(out, [
    { kind: "sheet", index: 0, name: "Totals", rows: 2, batches: 1, text: "a | b" },
  ]);
  assert.equal(folder.flush(), null);
});

test("batches of one sheet fold into one preview on the last batch", () => {
  const folder = createSheetFolder(identity);
  assert.deepEqual(folder.add(sheet(1, "Ledger", [row("head", "er"), row("1", "2")], true)), []);
  assert.deepEqual(folder.add(sheet(1, "Ledger", [row("3", "4")], true)), []);
  const out = folder.add(sheet(1, "Ledger", [row("5", "6"), row("7", "8")], false, { hidden: true }));
  assert.deepEqual(out, [
    { kind: "sheet", index: 1, name: "Ledger", rows: 5, batches: 3, hidden: true, text: "head | er" },
  ]);
  assert.equal(folder.flush(), null);
});

test("the first row comes from the first batch that carries rows", () => {
  const folder = createSheetFolder(identity);
  folder.add(sheet(0, "Sparse", [], true));
  const [preview] = folder.add(sheet(0, "Sparse", [row("x")]));
  assert.equal(preview.text, "x");
  assert.equal(preview.batches, 2);
});

test("the preview text goes through the cap", () => {
  const folder = createSheetFolder((text) => text.slice(0, 3));
  const [preview] = folder.add(sheet(0, "Wide", [row("abcdef")]));
  assert.equal(preview.text, "abc");
});

test("a sheet left waiting for its last batch flushes as incomplete", () => {
  const folder = createSheetFolder(identity);
  folder.add(sheet(2, "Cut", [row("p"), row("q")], true));
  assert.deepEqual(folder.flush(), {
    kind: "sheet", index: 2, name: "Cut", rows: 2, batches: 1, incomplete: true, text: "p",
  });
  assert.equal(folder.flush(), null);
});

test("a different sheet interrupting a batched one closes it as incomplete first", () => {
  const folder = createSheetFolder(identity);
  folder.add(sheet(0, "First", [row("1")], true));
  const out = folder.add(sheet(1, "Second", [row("2")]));
  assert.deepEqual(out.map((preview) => [preview.index, preview.rows, Boolean(preview.incomplete)]), [
    [0, 1, true],
    [1, 1, false],
  ]);
});
