// Web demo for gRParse: accepts a document upload, feeds it to
// ParseStreamingService/StreamProcessDocument in chunks, and relays each
// page event to the browser as one NDJSON line the moment it arrives.
// The browser never sees gRPC; this process is the bridge.
"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const express = require("express");
const grpc = require("@grpc/grpc-js");
const protoLoader = require("@grpc/proto-loader");

const TARGET = process.env.GRPARSE_TARGET || "localhost:50051";
const PORT = Number(process.env.PORT || 8080);
// Optional mount prefix (e.g. "/ui/grparse") for running behind a reverse
// proxy that forwards each service under its own path. Empty keeps the
// historical behavior of serving everything from the root.
const uiBaseRaw = process.env.UI_BASE || "";
const UI_BASE = uiBaseRaw === "" || uiBaseRaw === "/"
  ? ""
  : `/${uiBaseRaw.replace(/^\/+|\/+$/g, "")}`;
// The repo keeps the contract files at its root; their imports use the
// ai/pipestream/... layout, so stage copies into that shape before loading.
const PROTO_ROOT = process.env.GRPARSE_PROTO_DIR || path.resolve(__dirname, "..", "..");
const CHUNK_BYTES = 1024 * 1024;
const MAX_UPLOAD = "500mb"; // matches the server's stream limit

function stageProtos() {
  const staged = fs.mkdtempSync(path.join(os.tmpdir(), "grparse-protos-"));
  const layout = [
    ["document.proto", "ai/pipestream/document/v1/document.proto"],
    ["parse_types.proto", "ai/pipestream/parse/v1/parse_types.proto"],
    ["parse.proto", "ai/pipestream/parse/v1/parse.proto"],
    ["parse_stream.proto", "ai/pipestream/parse/v1/parse_stream.proto"],
  ];
  for (const [source, destination] of layout) {
    const target = path.join(staged, destination);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(path.join(PROTO_ROOT, source), target);
  }
  return staged;
}

const stagedDir = stageProtos();
const definition = protoLoader.loadSync(
  [
    path.join(stagedDir, "ai/pipestream/parse/v1/parse.proto"),
    path.join(stagedDir, "ai/pipestream/parse/v1/parse_stream.proto"),
  ],
  { includeDirs: [stagedDir], enums: String, longs: Number, defaults: true, oneofs: true },
);
const parseV1 = grpc.loadPackageDefinition(definition).ai.pipestream.parse.v1;
const channelOptions = { "grpc.max_receive_message_length": 128 * 1024 * 1024 };
const credentials = grpc.credentials.createInsecure();
const parseClient = new parseV1.ParseService(TARGET, credentials, channelOptions);
const streamClient = new parseV1.ParseStreamingService(TARGET, credentials, channelOptions);

// ---------------------------------------------------------------------------
// Event mapping: reduce protobuf shapes to the compact JSON the page renders.
// ---------------------------------------------------------------------------

function structToObject(value) {
  if (!value || !value.fields) return {};
  const result = {};
  for (const [key, field] of Object.entries(value.fields)) {
    if (field.stringValue !== undefined) result[key] = field.stringValue;
    else if (field.numberValue !== undefined) result[key] = field.numberValue;
    else if (field.boolValue !== undefined) result[key] = field.boolValue;
  }
  return result;
}

function shortLabel(label) {
  return typeof label === "string" ? label.replace("DOC_ITEM_LABEL_", "").toLowerCase() : "text";
}

function mapBoundingBox(bbox) {
  if (!bbox) return null;
  return { l: bbox.l, t: bbox.t, r: bbox.r, b: bbox.b };
}

function mapText(baseText) {
  const kind = baseText.item;
  if (!kind || !baseText[kind]) return null;
  // Every text variant wraps TextItemBase as `base` except CodeItem, which
  // inlines the same fields (see document.proto).
  const node = baseText[kind];
  const base = kind === "code" ? node : node.base;
  if (!base) return null;
  const prov = Array.isArray(base.prov) && base.prov.length > 0 ? base.prov[0] : null;
  return {
    ref: base.selfRef,
    label: shortLabel(base.label),
    text: base.text,
    bbox: prov ? mapBoundingBox(prov.bbox) : null,
  };
}

function mapTable(table) {
  const prov = Array.isArray(table.prov) && table.prov.length > 0 ? table.prov[0] : null;
  const data = table.data || {};
  return {
    ref: table.selfRef,
    bbox: prov ? mapBoundingBox(prov.bbox) : null,
    numRows: data.numRows || 0,
    numCols: data.numCols || 0,
    cells: (data.tableCells || []).map((cell) => ({
      text: cell.text,
      row: cell.startRowOffsetIdx,
      col: cell.startColOffsetIdx,
      rowSpan: cell.rowSpan,
      colSpan: cell.colSpan,
      header: Boolean(cell.columnHeader),
      bbox: mapBoundingBox(cell.bbox),
    })),
  };
}

function mapPicture(picture) {
  const prov = Array.isArray(picture.prov) && picture.prov.length > 0 ? picture.prov[0] : null;
  // The classifier's distribution rides an annotation (see document_assembly.cpp),
  // sorted most-confident first.
  const predictions = [];
  const barcodes = [];
  for (const annotation of picture.annotations || []) {
    if (annotation.classification) {
      for (const predicted of annotation.classification.predictedClasses || []) {
        predictions.push(predicted);
      }
    }
    if (annotation.misc && annotation.misc.kind === "barcode") {
      const content = structToObject(annotation.misc.content);
      barcodes.push({ format: content.format, value: content.value });
    }
  }
  return {
    ref: picture.selfRef,
    bbox: prov ? mapBoundingBox(prov.bbox) : null,
    classes: predictions.slice(0, 3).map((p) => ({ name: p.className, confidence: p.confidence })),
    barcodes,
    imageUri: picture.image && picture.image.uri ? picture.image.uri : null,
  };
}

function mapEvent(event) {
  if (event.page) {
    const page = event.page;
    return {
      type: "page",
      pageNumber: page.pageNumber,
      totalPages: event.totalPages,
      size: page.pageMeta && page.pageMeta.size ? page.pageMeta.size : null,
      // Present when the server runs with GRPARSE_PAGE_IMAGES=on: a data URI
      // preview of the page raster the boxes were measured on.
      image: page.pageMeta && page.pageMeta.image && page.pageMeta.image.uri
        ? page.pageMeta.image.uri
        : null,
      texts: (page.texts || []).map(mapText).filter(Boolean),
      offsets: (page.textOffsets || []).map((offset) => ({
        ref: offset.selfRef,
        start: offset.utfStart,
        end: offset.utfEnd,
        source: offset.source === "TEXT_SOURCE_DIGITAL_PDF" ? "digital" : "ocr",
        confidence: offset.confidence,
      })),
      tables: (page.tables || []).map(mapTable),
      pictures: (page.pictures || []).map(mapPicture),
    };
  }
  if (event.complete) {
    return {
      type: "complete",
      totalPages: event.totalPages,
      collectorFailures: (event.complete.collectorFailures || []).map((failure) => ({
        collector: failure.collector,
        error: failure.error,
      })),
    };
  }
  if (event.collectorDocument) {
    const doc = event.collectorDocument.document || {};
    return {
      type: "collector",
      collector: event.collectorDocument.collector,
      texts: (doc.texts || []).length,
      tables: (doc.tables || []).length,
      pictures: (doc.pictures || []).length,
      warnings: event.collectorDocument.warnings || [],
    };
  }
  return { type: "other" };
}

// ---------------------------------------------------------------------------
// HTTP surface
// ---------------------------------------------------------------------------

const app = express();
// Everything the page talks to lives on one router, mounted at UI_BASE when
// set so the whole surface (assets, API, sample) moves under the prefix.
const surface = express.Router();

if (UI_BASE) {
  // Inject the base into the entry page so app.js can prefix its fetches;
  // static would otherwise ship index.html verbatim.
  const indexHtml = fs.readFileSync(path.join(__dirname, "public", "index.html"), "utf8");
  const injected = indexHtml.replace(
    '<meta name="viewport" content="width=device-width, initial-scale=1" />',
    `<meta name="viewport" content="width=device-width, initial-scale=1" />\n  <meta name="ui-base" content="${UI_BASE}" />`,
  );
  surface.get("/", (_request, response) => response.type("html").send(injected));
  surface.get("/index.html", (_request, response) => response.type("html").send(injected));
  // Relative asset URLs only resolve under the base with a trailing slash.
  // Express matches this route loosely, so guard on the exact path to keep
  // "$UI_BASE/" itself from redirecting to itself.
  app.get(UI_BASE, (request, response, next) => {
    if (request.path !== UI_BASE) {
      next();
      return;
    }
    response.redirect(`${UI_BASE}/`);
  });
}

surface.use(express.static(path.join(__dirname, "public")));

surface.get("/api/health", (_request, response) => {
  parseClient.Health({}, { deadline: Date.now() + 5000 }, (error, health) => {
    if (error) {
      response.status(502).json({ ok: false, target: TARGET, error: error.message });
      return;
    }
    response.json({ ok: true, target: TARGET, status: health.status, version: health.version });
  });
});

surface.post(
  "/api/parse",
  express.raw({ type: () => true, limit: MAX_UPLOAD }),
  (request, response) => {
    const filename = String(request.query.filename || "document");
    const contentType = String(request.query.contentType || "");
    const body = request.body;
    if (!Buffer.isBuffer(body) || body.length === 0) {
      response.status(400).json({ error: "empty upload" });
      return;
    }

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = streamClient.StreamProcessDocument({ deadline });
    call.on("data", (event) => response.write(`${JSON.stringify(mapEvent(event))}\n`));
    call.on("end", () => response.end());
    call.on("error", (error) => {
      response.write(`${JSON.stringify({ type: "error", message: error.message })}\n`);
      response.end();
    });
    response.on("close", () => call.cancel());

    const meta = { documentId: filename, filename, contentType };
    for (let offset = 0; offset < body.length; offset += CHUNK_BYTES) {
      call.write({ ...meta, data: body.subarray(offset, offset + CHUNK_BYTES) });
    }
    call.write({ ...meta, complete: true });
    call.end();
  },
);

app.use(UI_BASE || "/", surface);

app.listen(PORT, () => {
  const base = UI_BASE ? `${UI_BASE}/` : "/";
  console.log(`gRParse web demo on http://localhost:${PORT}${base} -> ${TARGET}`);
});
