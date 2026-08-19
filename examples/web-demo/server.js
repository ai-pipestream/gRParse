// Web demo for gRParse: accepts a document upload, feeds it to
// ParseStreamingService/StreamProcessDocument in chunks, and relays each
// page event to the browser as one NDJSON line the moment it arrives.
// The browser never sees gRPC; this process is the bridge.
//
// With DEMO_UIS set the same process doubles as the demo shell for the
// whole grpc-services family: the header grows a tab bar fed by
// GET /api/uis (each tab's title/description/readiness read live from the
// service's gRPC info RPC), and /ui/<name>/* is reverse-proxied to that
// service's own web frontend so each tab renders in an iframe below.
"use strict";

const fs = require("node:fs");
const http = require("node:http");
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
// Demo-shell registry: DEMO_UIS entries of the form
//   name=grpc_addr@ui_addr
// comma-separated, e.g.
//   DEMO_UIS="lol-html=127.0.0.1:50057@127.0.0.1:8083,libreoffice=127.0.0.1:50053@127.0.0.1:8084"
// grpc_addr is where the service's info RPC is called; ui_addr is the HTTP
// frontend (started with UI_BASE=/ui/<name>) that /ui/<name>/* proxies to.
// ---------------------------------------------------------------------------

// The workspace keeps every repo side by side, so by default each service's
// proto resolves against ../../../<repo>/. DEMO_PROTO_DIR overrides that:
// point it at a directory holding one file per registry name
// ("lol-html.proto", ...) and those are used instead of the repo paths.
const DEMO_PROTO_DIR = process.env.DEMO_PROTO_DIR || "";

const KNOWN_UIS = {
  "lol-html": {
    repo: "grpc-lol-html",
    proto: "proto/lolhtml/v1/lolhtml_service.proto",
    include: "proto",
    service: "lolhtml.v1.LolHtmlService",
    method: "GetServiceInfo",
  },
  libreoffice: {
    repo: "grpc-libreoffice",
    proto: "proto/ai/pipestream/office/v1/office_service.proto",
    include: "proto",
    service: "ai.pipestream.office.v1.OfficeRenderService",
    method: "GetServiceInfo",
  },
  calamine: {
    repo: "grpc-calamine",
    proto: "proto/calamine/v1/calamine_service.proto",
    include: "proto",
    service: "calamine.v1.CalamineService",
    // Calamine advertises its UiInfo on GetMetadataResponse instead of a
    // dedicated info RPC.
    method: "GetMetadata",
  },
  epub: {
    repo: "grpc-epub",
    proto: "proto/ai/pipestream/epub/v1/epub_service.proto",
    include: "proto",
    service: "ai.pipestream.epub.v1.EpubParseService",
    method: "GetServiceInfo",
  },
  xml: {
    repo: "grpc-xml",
    proto: "proto/ai/pipestream/xml/v1/xml_service.proto",
    include: "proto",
    service: "ai.pipestream.xml.v1.XmlParseService",
    method: "GetServiceInfo",
  },
  markup: {
    repo: "grpc-markup",
    proto: "proto/ai/pipestream/markup/v1/markup_service.proto",
    include: "proto",
    service: "ai.pipestream.markup.v1.MarkupParseService",
    method: "GetServiceInfo",
  },
  ebcdic: {
    repo: "grpc-ebcdic",
    proto: "proto/ai/pipestream/ebcdic/v1/ebcdic_service.proto",
    include: "proto",
    service: "ai.pipestream.ebcdic.v1.EbcdicParseService",
    method: "GetServiceInfo",
  },
  email: {
    repo: "grpc-email",
    proto: "proto/ai/pipestream/email/v1/email_service.proto",
    include: "proto",
    service: "ai.pipestream.email.v1.EmailParseService",
    method: "GetServiceInfo",
  },
  enrich: {
    repo: "grpc-enrich",
    proto: "proto/ai/pipestream/enrich/v1/enrich_service.proto",
    include: "proto",
    service: "ai.pipestream.enrich.v1.EnrichService",
    method: "GetServiceInfo",
  },
  asr: {
    repo: "grpc-asr",
    proto: "proto/ai/pipestream/asr/v1/asr_service.proto",
    include: "proto",
    service: "ai.pipestream.asr.v1.AsrService",
    method: "GetServiceInfo",
  },
  "vlm-convert": {
    repo: "grpc-vlm-convert",
    proto: "proto/ai/pipestream/vlm/v1/vlm_convert.proto",
    include: "proto",
    service: "ai.pipestream.vlm.v1.VlmConvertService",
    method: "GetServiceInfo",
  },
  poic: {
    repo: "grPOIc",
    proto: "grpoic-api/src/main/proto/ai/pipestream/poi/v1/poi_service.proto",
    include: "grpoic-api/src/main/proto",
    service: "ai.pipestream.poi.v1.PoiParseService",
    method: "GetServiceInfo",
  },
  fastwarc: {
    repo: "fastwarc-grpc",
    proto: "proto/fastwarc/v1/warc_service.proto",
    include: "proto",
    service: "fastwarc.v1.WarcService",
    method: "GetServiceInfo",
  },
};

function parseRegistry(raw) {
  const entries = [];
  for (const spec of (raw || "").split(",")) {
    const trimmed = spec.trim();
    if (!trimmed) continue;
    const match = /^([A-Za-z0-9][A-Za-z0-9_-]*)=([^@\s]+)@([^@\s]+)$/.exec(trimmed);
    if (!match) {
      console.warn(`DEMO_UIS: ignoring malformed entry ${JSON.stringify(trimmed)} (want name=grpc_addr@ui_addr)`);
      continue;
    }
    const [, name, grpcAddr, uiAddr] = match;
    if (entries.some((entry) => entry.name === name)) {
      console.warn(`DEMO_UIS: ignoring duplicate entry for ${JSON.stringify(name)}`);
      continue;
    }
    const uiUrl = /^https?:\/\//.test(uiAddr) ? uiAddr : `http://${uiAddr}`;
    entries.push({ name, grpcAddr, uiUrl: uiUrl.replace(/\/+$/, "") });
  }
  return entries;
}

const registry = parseRegistry(process.env.DEMO_UIS);

// Loads the info-RPC client for one registry entry. Resolved lazily on the
// first /api/uis call so a missing proto file for a service nobody queries
// never stops the demo from booting; failures are cached as unreachable.
const infoClients = new Map();

// Where the sibling repos live, relative to this file. A plain workspace
// checkout is <ws>/gRParse/examples/web-demo (three levels up); a git
// worktree adds one (worktrees/gRParse-shell/examples/web-demo); inside the
// demo image both collapse to "/", where compose bind-mounts the sibling
// proto dirs. First candidate that has the file wins.
const WORKSPACE_CANDIDATES = [
  path.resolve(__dirname, "..", "..", ".."),
  path.resolve(__dirname, "..", "..", "..", ".."),
];

function resolveServiceProto(known) {
  for (const workspace of WORKSPACE_CANDIDATES) {
    const root = path.join(workspace, known.repo);
    const file = path.join(root, known.proto);
    if (fs.existsSync(file)) {
      return { file, includeDirs: [path.join(root, known.include)] };
    }
  }
  throw new Error(`${known.proto} not found under ${WORKSPACE_CANDIDATES.map((w) => path.join(w, known.repo)).join(" or ")}`);
}

function infoClientFor(entry) {
  if (infoClients.has(entry.name)) return infoClients.get(entry.name);
  let client = null;
  try {
    let file;
    let includeDirs;
    if (DEMO_PROTO_DIR) {
      file = path.join(DEMO_PROTO_DIR, `${entry.name}.proto`);
      includeDirs = [DEMO_PROTO_DIR];
    } else {
      const known = KNOWN_UIS[entry.name];
      if (!known) throw new Error(`no proto map entry for ${entry.name}; set DEMO_PROTO_DIR with ${entry.name}.proto`);
      ({ file, includeDirs } = resolveServiceProto(known));
    }
    const loaded = protoLoader.loadSync(file, {
      includeDirs, enums: String, longs: Number, defaults: true, oneofs: true,
    });
    const packageDefinition = grpc.loadPackageDefinition(loaded);
    const known = KNOWN_UIS[entry.name] || {};
    const serviceName = known.service;
    const method = known.method || "GetServiceInfo";
    if (!serviceName) throw new Error(`no service name known for ${entry.name}`);
    const ctor = serviceName.split(".").reduce((node, part) => node && node[part], packageDefinition);
    if (typeof ctor !== "function") throw new Error(`${serviceName} not found in ${file}`);
    client = { rpc: new ctor(entry.grpcAddr, credentials), method };
  } catch (error) {
    console.warn(`DEMO_UIS: ${entry.name}: proto unavailable (${error.message})`);
    client = { error };
  }
  infoClients.set(entry.name, client);
  return client;
}

// One live info-RPC call per registry entry; 1.5s deadline so a dead
// service only ever costs the tab bar that long.
function fetchUi(entry) {
  const fallback = { name: entry.name, title: entry.name, path: `/ui/${entry.name}`, description: "", reachable: false };
  const loaded = infoClientFor(entry);
  if (loaded.error) return Promise.resolve(fallback);
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (!settled) { settled = true; resolve(value); } };
    try {
      loaded.rpc[loaded.method]({}, { deadline: Date.now() + 1500 }, (error, response) => {
        if (error) { finish(fallback); return; }
        const ui = (response && response.ui) || {};
        finish({
          name: entry.name,
          title: ui.title || entry.name,
          path: ui.path || `/ui/${entry.name}`,
          description: ui.description || "",
          reachable: true,
        });
      });
    } catch (_error) {
      finish(fallback);
    }
    setTimeout(() => finish(fallback), 2000).unref();
  });
}

// Short cache so a tab bar refreshing every few seconds doesn't hammer the
// services with one gRPC call per tab per refresh.
let uisCache = null;
const UIS_CACHE_MS = 5000;

async function aggregatedUis() {
  if (uisCache && uisCache.expires > Date.now()) return uisCache.payload;
  const payload = await Promise.all(registry.map(fetchUi));
  uisCache = { expires: Date.now() + UIS_CACHE_MS, payload };
  return payload;
}

// Raw http.request piping: no body buffering, so NDJSON/SSE-style streams
// from the frontends flow through chunk by chunk, and large uploads stream
// up rather than landing in memory here first.
function proxyToFrontend(entry, request, response) {
  // originalUrl keeps the /ui/<name> prefix express strips from req.url at
  // the mount: the frontends serve under their UI_BASE, so the path goes
  // upstream unchanged.
  const upstream = http.request(`${entry.uiUrl}${request.originalUrl}`, {
    method: request.method,
    headers: { ...request.headers, host: new URL(entry.uiUrl).host },
  }, (upstreamResponse) => {
    response.writeHead(upstreamResponse.statusCode || 502, upstreamResponse.headers);
    upstreamResponse.pipe(response);
  });
  upstream.on("error", (error) => {
    if (response.headersSent) {
      response.destroy(error);
      return;
    }
    response.status(502).json({ error: `frontend for ${entry.name} unreachable: ${error.message}` });
  });
  request.pipe(upstream);
}

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

// Shell mode is mutually exclusive with UI_BASE: the shell owns the root
// (and /ui/* for its proxies); as one tab inside another shell, this app is
// a plain frontend serving under its prefix.
const shellMode = registry.length > 0 && !UI_BASE;

// Meta/script additions the entry page needs: ui-base when mounted under a
// prefix (so app.js prefixes its fetches), shell mode's meta flag plus the
// shell.js tag when the tab bar should render. With neither set the page
// ships byte-identical to the static file.
function serveIndex(_request, response) {
  if (!UI_BASE && !shellMode) {
    response.sendFile(path.join(__dirname, "public", "index.html"));
    return;
  }
  const indexHtml = fs.readFileSync(path.join(__dirname, "public", "index.html"), "utf8");
  const tags = [];
  if (UI_BASE) tags.push(`<meta name="ui-base" content="${UI_BASE}" />`);
  if (shellMode) tags.push('<meta name="shell" content="on" />');
  let injected = indexHtml.replace(
    '<meta name="viewport" content="width=device-width, initial-scale=1" />',
    `<meta name="viewport" content="width=device-width, initial-scale=1" />\n  ${tags.join("\n  ")}`,
  );
  if (shellMode) {
    injected = injected.replace(
      '<script src="app.js"></script>',
      '<script src="shell.js"></script>\n  <script src="app.js"></script>',
    );
  }
  response.type("html").send(injected);
}

if (UI_BASE) {
  surface.get("/", serveIndex);
  surface.get("/index.html", serveIndex);
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
} else if (shellMode) {
  surface.get("/", serveIndex);
  surface.get("/index.html", serveIndex);
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

if (shellMode) {
  // Tab data for the shell header: one entry per registered service, with
  // the UiInfo block read live off the service's gRPC info RPC.
  surface.get("/api/uis", (_request, response) => {
    aggregatedUis().then(
      (uis) => response.json({ uis }),
      (error) => response.status(500).json({ error: error.message }),
    );
  });

  // Each registered frontend is proxied under its shell path; the FEs are
  // started with UI_BASE=/ui/<name>, so the path forwards unchanged.
  for (const entry of registry) {
    app.use(`/ui/${entry.name}`, (request, response) => proxyToFrontend(entry, request, response));
  }
}

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
  if (shellMode) {
    console.log(`demo shell tabs: ${registry.map((entry) => `${entry.name} (${entry.grpcAddr} -> ${entry.uiUrl})`).join(", ")}`);
  }
});
