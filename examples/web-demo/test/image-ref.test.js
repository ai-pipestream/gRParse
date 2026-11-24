// Unit tests for how the demo reads an ImageRef: bytes held in `data` are
// shown like a data: URI, and no Buffer reaches the page.
"use strict";

const assert = require("node:assert/strict");
const { test } = require("node:test");

const { imageUri, withShowableImages } = require("../image-ref");

const png = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

test("a uri is given as it is", () => {
  assert.equal(imageUri({ uri: "https://example.test/a.png", data: png }), "https://example.test/a.png");
  assert.equal(imageUri({ uri: "data:image/png;base64,AA==" }), "data:image/png;base64,AA==");
});

test("bytes in the data field become a data: URI under their declared type", () => {
  assert.equal(imageUri({ mimetype: " Image/PNG; x=y", uri: "", data: png }),
    "data:image/png;base64,iVBORw0KGgo=");
  assert.equal(imageUri({ uri: "", data: png }), "data:application/octet-stream;base64,iVBORw0KGgo=");
});

test("a ref that holds nothing has no uri", () => {
  assert.equal(imageUri(undefined), null);
  assert.equal(imageUri({ uri: "", data: Buffer.alloc(0) }), null);
  assert.equal(imageUri({ mimetype: "image/png" }), null);
});

test("a document's held images are showable and carry no Buffer", () => {
  const doc = {
    pages: { 1: { image: { mimetype: "image/png", uri: "", data: png } }, 2: {} },
    pictures: [
      { image: { mimetype: "image/png", uri: "", data: png } },
      { image: { uri: "figs/a.png", data: Buffer.alloc(0) } },
      {},
    ],
    texts: [{ code: { image: { mimetype: "image/png", data: png } } }, { text: {} }],
  };
  const shown = withShowableImages(doc);
  assert.equal(shown.pages[1].image.uri, "data:image/png;base64,iVBORw0KGgo=");
  assert.equal(shown.pictures[0].image.uri, "data:image/png;base64,iVBORw0KGgo=");
  assert.equal(shown.pictures[1].image.uri, "figs/a.png");
  assert.equal(shown.texts[0].code.image.uri, "data:image/png;base64,iVBORw0KGgo=");
  assert.ok(!JSON.stringify(shown).includes("Buffer"), "no Buffer in what the page receives");
  assert.equal(withShowableImages(null), null);
});
