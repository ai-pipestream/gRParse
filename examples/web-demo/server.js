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
// The standalone fastwarc-grpc server (fastwarc.v1.WarcService) backing the
// native FastWARC tab; see the bridge section below.
const FASTWARC_TARGET = process.env.FASTWARC_TARGET || "127.0.0.1:50060";
// The grPOIc server (ai.pipestream.poi.v1.PoiParseService, Apache POI office
// parsing) backing the native POI tab; see the bridge section below.
const POIC_TARGET = process.env.POIC_TARGET || "127.0.0.1:50052";
// The grpc-asr server (ai.pipestream.asr.v1.AsrService, whisper.cpp
// speech-to-text) backing the native ASR tab; see the bridge section below.
const ASR_TARGET = process.env.ASR_TARGET || "127.0.0.1:50055";
// The grpc-enrich server (ai.pipestream.enrich.v1.EnrichService, VLM item
// annotations) backing the native Enrich tab; see the bridge section below.
const ENRICH_TARGET = process.env.ENRICH_TARGET || "127.0.0.1:50056";
// The grpc-vlm-convert server (ai.pipestream.vlm.v1.VlmConvertService, VLM
// page parse) backing the native VLM Convert tab; see the bridge section
// below.
const VLM_CONVERT_TARGET = process.env.VLM_CONVERT_TARGET || "127.0.0.1:50058";
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
  pdf: {
    repo: "grpc-pdf-inspector",
    proto: "proto/ai/pipestream/pdf/v1/pdf_service.proto",
    include: "proto",
    service: "ai.pipestream.pdf.v1.PdfParseService",
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
// Document viewer bridge: the native Document tab (public/document.html)
// renders the complete merged document instead of the flattened page events
// above. /api/document/parse runs the unary ConvertSource RPC on the same
// channel /api/parse uses and relays the response's native document as one
// protobuf-JSON NDJSON line; while that conversion runs, a parallel
// StreamProcessDocument call over the same bytes supplies live page-progress
// lines (page number and elapsed time only, no payloads). The progress leg
// is advisory: its failures stay silent and only the conversion decides the
// outcome. /api/document/status probes the service's Health RPC for the
// shell tab dot, cached like the other native-tab probes.
// ---------------------------------------------------------------------------

let documentStatusCache = null;
const DOCUMENT_STATUS_CACHE_MS = 5000;

function probeDocumentService() {
  const fallback = { reachable: false };
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (!settled) { settled = true; resolve(value); } };
    try {
      parseClient.Health({}, { deadline: Date.now() + 1500 }, (error, health) => {
        if (error) { finish(fallback); return; }
        finish({ reachable: true, status: health.status, version: health.version });
      });
    } catch (_error) {
      finish(fallback);
    }
    setTimeout(() => finish(fallback), 2000).unref();
  });
}

async function documentTabStatus() {
  if (documentStatusCache && documentStatusCache.expires > Date.now()) return documentStatusCache.payload;
  const payload = await probeDocumentService();
  documentStatusCache = { expires: Date.now() + DOCUMENT_STATUS_CACHE_MS, payload };
  return payload;
}

// ---------------------------------------------------------------------------
// FastWARC bridge: the standalone fastwarc-grpc server (fastwarc.v1.WarcService)
// has no web UI of its own, so the shell carries it as a native tab
// (public/fastwarc.html) backed by the two /api/fastwarc endpoints below: a
// GetServiceInfo probe for the status badge (grpc.health.v1 as fallback) and
// an NDJSON relay of the bidirectional ParseWarc stream. The contract loads
// from the sibling fastwarc-grpc checkout through KNOWN_UIS/resolveServiceProto
// like the other native tabs; the vendored collectors/*.proto copies speak the
// legacy chatnoir dialect (same package, incompatible wire) and stay reserved
// for the C++ collector's own client.
// ---------------------------------------------------------------------------

const FASTWARC_CHUNK_BYTES = 256 * 1024;
// Only the first few KiB of a text payload are relayed for the preview.
const PREVIEW_BYTES = 4 * 1024;

// The server speaks grpc.health.v1 but the repo does not vendor the proto;
// it is small enough to carry inline and stage into a temp dir at first use.
const HEALTH_PROTO = `syntax = "proto3";
package grpc.health.v1;
service Health {
  rpc Check(HealthCheckRequest) returns (HealthCheckResponse);
}
message HealthCheckRequest {
  string service = 1;
}
message HealthCheckResponse {
  enum ServingStatus {
    UNKNOWN = 0;
    SERVING = 1;
    NOT_SERVING = 2;
    SERVICE_UNKNOWN = 3;
  }
  ServingStatus status = 1;
}
`;

// The contract resolves from the sibling fastwarc-grpc checkout (or the
// compose image's bind-mounted /fastwarc-grpc/proto) through the KNOWN_UIS
// "fastwarc" entry; the grpc.health.v1 fallback probe is staged from the
// inline copy above. Lazy (first /api/fastwarc call) so a missing contract
// never stops the demo from booting.
let fastwarcClientsCache = null;

function fastwarcClients() {
  if (fastwarcClientsCache) return fastwarcClientsCache;
  try {
    const resolved = resolveServiceProto(KNOWN_UIS.fastwarc);
    const staged = fs.mkdtempSync(path.join(os.tmpdir(), "fastwarc-health-"));
    const healthTarget = path.join(staged, "grpc", "health", "v1", "health.proto");
    fs.mkdirSync(path.dirname(healthTarget), { recursive: true });
    fs.writeFileSync(healthTarget, HEALTH_PROTO);
    const loaded = protoLoader.loadSync(
      [resolved.file, healthTarget],
      { includeDirs: [...resolved.includeDirs, staged], enums: String, longs: Number, defaults: true, oneofs: true },
    );
    const definition = grpc.loadPackageDefinition(loaded);
    fastwarcClientsCache = {
      warc: new definition.fastwarc.v1.WarcService(FASTWARC_TARGET, credentials, channelOptions),
      health: new definition.grpc.health.v1.Health(FASTWARC_TARGET, credentials, channelOptions),
    };
  } catch (error) {
    console.warn(`fastwarc: proto unavailable (${error.message})`);
    fastwarcClientsCache = { error };
  }
  return fastwarcClientsCache;
}

// One live info call behind a short cache, mirroring the other native tabs:
// GetServiceInfo carries the name/version/UiInfo for the badge; when it
// fails (an older server without the RPC) the standard health check decides
// plain reachability. 1.5s deadline so a dead server costs the badge that
// long at most, and failure reports unreachable instead of breaking the page.
let fastwarcStatusCache = null;
const FASTWARC_STATUS_CACHE_MS = 5000;

function probeFastwarc() {
  const fallback = { reachable: false };
  const clients = fastwarcClients();
  if (clients.error) return Promise.resolve(fallback);
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (!settled) { settled = true; resolve(value); } };
    const healthCheck = () => {
      try {
        clients.health.Check({ service: "" }, { deadline: Date.now() + 1500 }, (error, health) => {
          if (error) { finish(fallback); return; }
          finish({ reachable: health.status === "SERVING" });
        });
      } catch (_error) {
        finish(fallback);
      }
    };
    try {
      clients.warc.GetServiceInfo({}, { deadline: Date.now() + 1500 }, (error, info) => {
        if (error) { healthCheck(); return; }
        finish({
          reachable: true,
          name: info.name || undefined,
          version: info.version || undefined,
        });
      });
    } catch (_error) {
      healthCheck();
    }
    setTimeout(() => finish(fallback), 2000).unref();
  });
}

async function fastwarcStatus() {
  if (fastwarcStatusCache && fastwarcStatusCache.expires > Date.now()) return fastwarcStatusCache.payload;
  const payload = await probeFastwarc();
  fastwarcStatusCache = { expires: Date.now() + FASTWARC_STATUS_CACHE_MS, payload };
  return payload;
}

function recordTypeLabel(value) {
  return typeof value === "string" ? value.replace("WARC_RECORD_TYPE_", "").toLowerCase() : "unknown";
}

// Digest verification outcome for the page's digests column; unspecified
// (verification not requested) stays off the wire entirely.
function digestStatusLabel(value) {
  if (typeof value !== "string" || value === "DIGEST_STATUS_UNSPECIFIED") return undefined;
  return value.replace("DIGEST_STATUS_", "").toLowerCase();
}

// A payload is previewed only when it reads as text: an HTTP content type of
// text/* or anything json/xml/html, or no HTTP metadata to judge by at all.
function previewable(metadata) {
  if (!metadata.isHttp) return true;
  const contentType = (metadata.httpContentType || "").toLowerCase();
  if (!contentType) return true;
  return contentType.startsWith("text/")
    || contentType.includes("json")
    || contentType.includes("xml")
    || contentType.includes("html");
}

// ---------------------------------------------------------------------------
// POI bridge: grPOIc (ai.pipestream.poi.v1.PoiParseService) wraps Apache POI
// for office documents. The shell carries it as a native tab
// (public/poic.html) backed by the two /api/poic endpoints below: a
// reachability probe (GetServiceInfo) for the status badge and an NDJSON
// relay of the ParseDocument stream.
// ---------------------------------------------------------------------------

const POIC_CHUNK_BYTES = 1024 * 1024;
// Element text previews are capped so the page never receives a whole
// chapter in a single NDJSON line.
const POIC_PREVIEW_CHARS = 512;

// The contract lives in the sibling grPOIc checkout; resolveServiceProto
// (against the KNOWN_UIS "poic" entry) covers both the workspace layout and
// the demo image's bind-mounted proto dirs.
let poicClientsCache = null;

function poicClients() {
  if (poicClientsCache) return poicClientsCache;
  try {
    const resolved = resolveServiceProto(KNOWN_UIS.poic);
    const loaded = protoLoader.loadSync(resolved.file, {
      includeDirs: resolved.includeDirs, enums: String, longs: Number, defaults: true, oneofs: true,
    });
    const definition = grpc.loadPackageDefinition(loaded);
    poicClientsCache = {
      poi: new definition.ai.pipestream.poi.v1.PoiParseService(POIC_TARGET, credentials, channelOptions),
    };
  } catch (error) {
    console.warn(`poic: proto unavailable (${error.message})`);
    poicClientsCache = { error };
  }
  return poicClientsCache;
}

// One live info call behind a short cache, mirroring the fastwarc probe:
// 1.5s deadline so a dead server costs the badge that long at most, and
// failure reports unreachable instead of breaking the page. GetServiceInfo
// doubles as the health check (grPOIc's grpc.health.v1 only registers the
// empty service name) and carries the versions for the page badge.
let poicStatusCache = null;
const POIC_STATUS_CACHE_MS = 5000;

function probePoic() {
  const fallback = { reachable: false };
  const clients = poicClients();
  if (clients.error) return Promise.resolve(fallback);
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => { if (!settled) { settled = true; resolve(value); } };
    try {
      clients.poi.GetServiceInfo({}, { deadline: Date.now() + 1500 }, (error, info) => {
        if (error) { finish(fallback); return; }
        finish({
          reachable: true,
          serviceVersion: info.serviceVersion,
          poiVersion: info.poiVersion,
          supportedFormats: (info.supportedFormats || []).map(documentFormatLabel),
          maxDocumentBytes: info.maxDocumentBytes,
        });
      });
    } catch (_error) {
      finish(fallback);
    }
    setTimeout(() => finish(fallback), 2000).unref();
  });
}

async function poicStatus() {
  if (poicStatusCache && poicStatusCache.expires > Date.now()) return poicStatusCache.payload;
  const payload = await probePoic();
  poicStatusCache = { expires: Date.now() + POIC_STATUS_CACHE_MS, payload };
  return payload;
}

function documentFormatLabel(value) {
  return typeof value === "string" ? value.replace("DOCUMENT_FORMAT_", "").toLowerCase() : "unknown";
}

function statusStateLabel(value) {
  return typeof value === "string" ? value.replace("STATE_", "").toLowerCase() : "unspecified";
}

function capPreview(text) {
  const value = String(text || "");
  return value.length > POIC_PREVIEW_CHARS ? `${value.slice(0, POIC_PREVIEW_CHARS)}…` : value;
}

function mapPoicTimestamp(value) {
  return value && Number(value.seconds) > 0 ? new Date(Number(value.seconds) * 1000).toISOString() : undefined;
}

// Reduce one streamed content element to a compact preview line. Full text
// stays on the server side; the page gets the shape and the first few
// hundred characters.
function mapPoicElement(event) {
  if (event.paragraph) {
    const paragraph = event.paragraph;
    return {
      kind: "paragraph",
      style: paragraph.style || undefined,
      text: capPreview(paragraph.text),
      length: (paragraph.text || "").length,
    };
  }
  if (event.table) {
    const rows = event.table.rows || [];
    const firstRow = rows.length > 0 ? (rows[0].cells || []).map((cell) => cell.text).join(" | ") : "";
    return {
      kind: "table",
      rows: rows.length,
      cols: rows.reduce((widest, row) => Math.max(widest, (row.cells || []).length), 0),
      text: capPreview(firstRow),
    };
  }
  if (event.sheet) {
    const sheet = event.sheet;
    const rows = sheet.rows || [];
    const firstRow = rows.length > 0 ? (rows[0].cells || []).map((cell) => cell.formatted).join(" | ") : "";
    return {
      kind: "sheet",
      index: sheet.index,
      name: sheet.name,
      rows: rows.length,
      text: capPreview(firstRow),
    };
  }
  if (event.slide) {
    const slide = event.slide;
    return {
      kind: "slide",
      index: slide.index,
      title: slide.title || undefined,
      notes: (slide.notes || []).length,
      text: capPreview((slide.texts || []).join("\n")),
    };
  }
  if (event.embeddedObject) {
    const object = event.embeddedObject;
    return {
      kind: "embedded",
      id: object.id,
      filename: object.filename || undefined,
      contentType: object.contentType || undefined,
      sizeBytes: object.sizeBytes,
    };
  }
  return null;
}

// ---------------------------------------------------------------------------
// Headless-service bridges: grpc-asr (ai.pipestream.asr.v1.AsrService),
// grpc-enrich (ai.pipestream.enrich.v1.EnrichService), and grpc-vlm-convert
// (ai.pipestream.vlm.v1.VlmConvertService) are pipeline services with no
// web UI of their own, so the shell carries each as a native tab
// (public/asr.html, public/enrich.html, public/vlm-convert.html) backed by
// the /api/<name> endpoints below. The pattern mirrors the POI bridge:
// contracts resolve from the sibling repos through the same
// KNOWN_UIS/resolveServiceProto map, GetServiceInfo doubles as the health
// probe (1.5s deadline, 5s cache), and each conversion call relays the
// service's live event stream as NDJSON.
// ---------------------------------------------------------------------------

// Lazy client plus cached GetServiceInfo probe for one headless service.
// mapInfo reduces the info response to the status payload the page renders;
// probe failure reports unreachable instead of breaking the page.
function nativeBridge(name, target, mapInfo) {
  const state = { clients: null, status: null };
  const clients = () => {
    if (state.clients) return state.clients;
    try {
      const known = KNOWN_UIS[name];
      const resolved = resolveServiceProto(known);
      const loaded = protoLoader.loadSync(resolved.file, {
        includeDirs: resolved.includeDirs, enums: String, longs: Number, defaults: true, oneofs: true,
      });
      const definition = grpc.loadPackageDefinition(loaded);
      const ctor = known.service.split(".").reduce((node, part) => node && node[part], definition);
      if (typeof ctor !== "function") throw new Error(`${known.service} not found in ${resolved.file}`);
      state.clients = { rpc: new ctor(target, credentials, channelOptions) };
    } catch (error) {
      console.warn(`${name}: proto unavailable (${error.message})`);
      state.clients = { error };
    }
    return state.clients;
  };
  const probe = () => {
    const fallback = { reachable: false };
    const loaded = clients();
    if (loaded.error) return Promise.resolve(fallback);
    return new Promise((resolve) => {
      let settled = false;
      const finish = (value) => { if (!settled) { settled = true; resolve(value); } };
      try {
        loaded.rpc.GetServiceInfo({}, { deadline: Date.now() + 1500 }, (error, info) => {
          if (error) { finish(fallback); return; }
          finish({ reachable: true, ...mapInfo(info) });
        });
      } catch (_error) {
        finish(fallback);
      }
      setTimeout(() => finish(fallback), 2000).unref();
    });
  };
  const status = async () => {
    if (state.status && state.status.expires > Date.now()) return state.status.payload;
    const payload = await probe();
    state.status = { expires: Date.now() + 5000, payload };
    return payload;
  };
  return { clients, status };
}

// Generic enum-prefix stripper for the page-facing labels.
function stripEnum(value, prefix) {
  return typeof value === "string" ? value.replace(prefix, "").toLowerCase() : "unknown";
}

// Generic text cap so no single NDJSON line carries a whole transcript or
// model response.
const NATIVE_PREVIEW_CHARS = 512;
function capChars(text, limit) {
  const value = String(text || "");
  return value.length > limit ? `${value.slice(0, limit)}…` : value;
}

const ASR_CHUNK_BYTES = 1024 * 1024;

const asrBridge = nativeBridge("asr", ASR_TARGET, (info) => ({
  version: info.version,
  whisperVersion: info.whisperVersion,
  backend: info.backend,
  models: info.models || [],
  maxMediaBytes: info.maxMediaBytes,
  maxDurationMs: info.maxDurationMs,
}));

// grpc-enrich and grpc-vlm-convert both depend on an external VLM server.
// Their status payloads carry vlmConfigured so the page and the shell tab
// can surface "reachable but no VLM endpoint configured" distinctly:
// without an endpoint every enrichment skips (SKIP_REASON_VLM_ERROR) and
// every conversion fails FAILED_PRECONDITION unless the request overrides.
const enrichBridge = nativeBridge("enrich", ENRICH_TARGET, (info) => ({
  serviceVersion: info.serviceVersion,
  apiVersion: info.apiVersion,
  defaultVlmEndpoint: info.defaultVlmEndpoint || "",
  vlmConfigured: Boolean(info.defaultVlmEndpoint),
  maxDocumentBytes: info.maxDocumentBytes,
  maxConcurrentVlmCalls: info.maxConcurrentVlmCalls,
}));

const vlmConvertBridge = nativeBridge("vlm-convert", VLM_CONVERT_TARGET, (info) => ({
  version: info.version,
  endpoint: info.endpoint || "",
  vlmConfigured: Boolean(info.endpoint),
  presets: (info.presets || []).map((value) => stripEnum(value, "VLM_PRESET_")),
  rawPresets: info.rawPresets || [],
  concurrency: info.concurrency,
  maxPageBytes: info.maxPageBytes,
  maxPages: info.maxPages,
}));

// One transcript segment reduced to the line the ASR page renders.
function mapAsrSegment(segment) {
  return {
    index: segment.index,
    startMs: Number(segment.startMs) || 0,
    endMs: Number(segment.endMs) || 0,
    text: segment.text,
    avgLogprob: segment.avgLogprob,
    words: (segment.words || []).length,
  };
}

// One enrichment annotation reduced to the line the Enrich page renders,
// keyed by the item's self_ref. The annotation oneof identifies the job.
function mapEnrichAnnotation(annotation) {
  const base = { type: "annotation", selfRef: annotation.selfRef, model: annotation.model || undefined };
  const kind = annotation.annotation;
  if (kind === "description") {
    const description = annotation.description;
    return { ...base, kind: "description", text: description.text, confidence: description.confidence };
  }
  if (kind === "chartTable") {
    const chart = annotation.chartTable;
    const data = chart.table || {};
    return {
      ...base,
      kind: "chart",
      title: chart.title || undefined,
      rows: data.numRows || 0,
      cols: data.numCols || 0,
      csv: capChars(chart.csv, 2048),
    };
  }
  if (kind === "code") {
    const code = annotation.code;
    return {
      ...base,
      kind: "code",
      text: capChars(code.text, 2048),
      language: stripEnum(code.language, "CODE_LANGUAGE_LABEL_"),
      languageRaw: code.languageRaw || undefined,
    };
  }
  if (kind === "formula") {
    return { ...base, kind: "formula", text: capChars(annotation.formula.text, 2048) };
  }
  return { ...base, kind: "other" };
}

// A converted page's Document fragment reduced to the summary the VLM
// Convert page renders: item counts plus a capped per-item preview. mapText
// above reads the same canonical document shape, so it is reused here.
function mapVlmPageDocument(pageDocument) {
  const doc = pageDocument.document || {};
  const texts = (doc.texts || []).map(mapText).filter(Boolean);
  return {
    type: "page",
    pageNo: pageDocument.pageNo,
    texts: texts.length,
    tables: (doc.tables || []).length,
    pictures: (doc.pictures || []).length,
    items: texts.slice(0, 200).map((item) => ({
      label: item.label,
      text: capChars(item.text, NATIVE_PREVIEW_CHARS),
    })),
  };
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

// Status badge for the native Document tab; cached like the other probes.
surface.get("/api/document/status", (_request, response) => {
  documentTabStatus().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// Raw document bytes in (same 500 MiB cap as /api/parse; body-parser answers
// 413 past the limit), NDJSON out: page-progress lines while the parse runs
// ({ type: "page", pageNumber, totalPages, elapsedMs }), then exactly one
// { type: "document", elapsedMs, document } line carrying the complete
// merged document as protobuf-JSON (lowerCamelCase field names, enum value
// names as strings), or { type: "error" } on failure. The page sends the
// filename and content type as query params, like /api/parse.
surface.post(
  "/api/document/parse",
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

    const startedAt = Date.now();
    let finished = false;
    const send = (value) => { if (!finished) response.write(`${JSON.stringify(value)}\n`); };
    const finish = () => {
      if (finished) return;
      finished = true;
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;

    // Progress leg: a second streaming parse of the same bytes, mapped down
    // to page number and elapsed time per finished page. It exists purely so
    // the page can show live progress while the unary conversion below runs;
    // the double parse is the accepted cost of that (the two calls run
    // concurrently against the same server). Errors here are swallowed: the
    // conversion alone decides success.
    let progress = null;
    try {
      progress = streamClient.StreamProcessDocument({ deadline });
      progress.on("data", (event) => {
        if (event.page) {
          send({
            type: "page",
            pageNumber: event.page.pageNumber,
            totalPages: event.totalPages,
            elapsedMs: Date.now() - startedAt,
          });
        }
      });
      progress.on("error", () => {});
      progress.on("end", () => {});
      const meta = { documentId: filename, filename, contentType };
      for (let offset = 0; offset < body.length; offset += CHUNK_BYTES) {
        progress.write({ ...meta, data: body.subarray(offset, offset + CHUNK_BYTES) });
      }
      progress.write({ ...meta, complete: true });
      progress.end();
    } catch (_error) {
      progress = null;
    }
    const cancelProgress = () => {
      if (!progress) return;
      try { progress.cancel(); } catch (_error) { /* already closed */ }
      progress = null;
    };

    // The conversion leg: the response's document.doc is the complete merged
    // document as a native message, which proto-loader has already decoded
    // into the protobuf-JSON shape the page renders (camelCase fields, enum
    // names). No export formats are requested; a rendered export would only
    // duplicate the same document as a string.
    const call = parseClient.ConvertSource(
      {
        request: {
          sources: [{ file: { base64String: body.toString("base64"), filename } }],
        },
      },
      { deadline },
      (error, converted) => {
        cancelProgress();
        if (error) {
          send({ type: "error", code: error.code, message: error.message });
          finish();
          return;
        }
        const documentResponse = converted && converted.response && converted.response.document;
        const doc = documentResponse ? documentResponse.doc : null;
        if (!doc) {
          send({ type: "error", message: "conversion returned no document" });
          finish();
          return;
        }
        send({ type: "document", elapsedMs: Date.now() - startedAt, document: doc });
        finish();
      },
    );
    response.on("close", () => {
      finished = true;
      cancelProgress();
      try { call.cancel(); } catch (_error) { /* already settled */ }
    });
  },
);

// Status badge for the native FastWARC tab; cached like the /api/uis probes
// so a refreshing page never hammers the server.
surface.get("/api/fastwarc/status", (_request, response) => {
  fastwarcStatus().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// Raw archive bytes in (same 500 MiB cap as /api/parse; body-parser answers
// 413 past the limit), one NDJSON line per record event out. The page sends
// the parse flags as query params.
surface.post(
  "/api/fastwarc/parse",
  express.raw({ type: () => true, limit: MAX_UPLOAD }),
  (request, response) => {
    const body = request.body;
    if (!Buffer.isBuffer(body) || body.length === 0) {
      response.status(400).json({ error: "empty upload" });
      return;
    }
    const clients = fastwarcClients();
    if (clients.error) {
      response.status(502).json({ error: `fastwarc proto unavailable: ${clients.error.message}` });
      return;
    }
    const flag = (name, fallback) => (request.query[name] === undefined ? fallback : request.query[name] !== "false");

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const startedAt = Date.now();
    let records = 0;
    let payloadBytes = 0;
    // Per-record preview assembly: reset by record_start, fed by
    // payload_chunk, flushed by record_end.
    let preview = null;
    const send = (value) => response.write(`${JSON.stringify(value)}\n`);
    const finish = () => {
      send({ type: "done", records, payloadBytes, elapsedMs: Date.now() - startedAt });
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = clients.warc.ParseWarc({ deadline });

    // Responses arrive one protocol event per message, ordered per record:
    // record_start, payload chunks, record_end; record_error for failures.
    call.on("data", (event) => {
      if (event.recordStart) {
        const metadata = event.recordStart.metadata || {};
        records += 1;
        preview = { wanted: previewable(metadata), chunks: [], bytes: 0 };
        send({
          type: "start",
          recordIndex: Number(metadata.recordIndex) || 0,
          recordType: recordTypeLabel(metadata.recordType),
          streamPos: metadata.streamPos,
          contentLength: metadata.contentLength,
          recordId: metadata.recordId || undefined,
          recordDate: metadata.recordDate
            ? new Date(Number(metadata.recordDate.seconds) * 1000).toISOString()
            : undefined,
          isHttp: Boolean(metadata.isHttp),
          httpContentType: metadata.httpContentType || undefined,
        });
        return;
      }
      if (event.payloadChunk) {
        const data = event.payloadChunk.data || Buffer.alloc(0);
        if (preview && preview.wanted && preview.bytes < PREVIEW_BYTES) {
          const slice = data.subarray(0, PREVIEW_BYTES - preview.bytes);
          preview.chunks.push(slice);
          preview.bytes += slice.length;
        }
        return;
      }
      if (event.recordEnd) {
        if (preview && preview.chunks.length > 0) {
          send({ type: "preview", text: Buffer.concat(preview.chunks).toString("utf8") });
        }
        preview = null;
        // The server's count is authoritative even if a preview truncated.
        payloadBytes += Number(event.recordEnd.payloadLength) || 0;
        send({
          type: "end",
          recordIndex: Number(event.recordEnd.recordIndex) || 0,
          payloadLength: event.recordEnd.payloadLength,
          blockDigest: digestStatusLabel(event.recordEnd.blockDigestStatus),
          payloadDigest: digestStatusLabel(event.recordEnd.payloadDigestStatus),
          digestDetail: event.recordEnd.digestDetail || undefined,
        });
        return;
      }
      if (event.recordError) {
        const failure = event.recordError;
        send({
          type: "error",
          streamPos: failure.streamPos,
          recoverable: Boolean(failure.recoverable),
          message: failure.message,
        });
      }
    });
    // A non-recoverable record_error already ended the upstream stream; the
    // done line reports whatever arrived before it.
    call.on("end", finish);
    call.on("error", (error) => {
      send({ type: "error", recoverable: false, message: error.message });
      finish();
    });
    response.on("close", () => call.cancel());

    call.write({
      config: {
        parseHttp: flag("parse_http", true),
        verifyDigests: flag("verify_digests", false),
      },
    });
    for (let offset = 0; offset < body.length; offset += FASTWARC_CHUNK_BYTES) {
      call.write({ chunk: body.subarray(offset, offset + FASTWARC_CHUNK_BYTES) });
    }
    call.end();
  },
);

// Status badge for the native POI tab; cached like the fastwarc probe.
surface.get("/api/poic/status", (_request, response) => {
  poicStatus().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// Raw office bytes in (same 500 MiB cap as /api/parse; body-parser answers
// 413 past the limit), one NDJSON line per parse event out: a start line
// from DocumentInfo, one preview line per content element, an end line from
// ParseStatus, grpc-error on failure, done last. The page sends the
// filename and content type as query params.
surface.post(
  "/api/poic/parse",
  express.raw({ type: () => true, limit: MAX_UPLOAD }),
  (request, response) => {
    const body = request.body;
    if (!Buffer.isBuffer(body) || body.length === 0) {
      response.status(400).json({ error: "empty upload" });
      return;
    }
    const clients = poicClients();
    if (clients.error) {
      response.status(502).json({ error: `poic proto unavailable: ${clients.error.message}` });
      return;
    }
    const filename = String(request.query.filename || "document");
    const contentType = String(request.query.contentType || "");

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const startedAt = Date.now();
    let elements = 0;
    let finished = false;
    const send = (value) => response.write(`${JSON.stringify(value)}\n`);
    // grpc-js can emit end after error; the done line goes out exactly once.
    const finish = () => {
      if (finished) return;
      finished = true;
      send({ type: "done", elements, elapsedMs: Date.now() - startedAt });
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = clients.poi.ParseDocument({ deadline });

    call.on("data", (event) => {
      if (event.documentInfo) {
        const metadata = event.documentInfo.metadata || {};
        send({
          type: "start",
          documentId: event.documentInfo.documentId || undefined,
          format: documentFormatLabel(event.documentInfo.format),
          title: metadata.title || undefined,
          author: metadata.author || undefined,
          lastModifiedBy: metadata.lastModifiedBy || undefined,
          created: mapPoicTimestamp(metadata.created),
          modified: mapPoicTimestamp(metadata.modified),
          extraMetadata: (metadata.tail || []).length,
        });
        return;
      }
      if (event.status) {
        send({
          type: "end",
          state: statusStateLabel(event.status.state),
          warnings: event.status.warnings || [],
          paragraphs: event.status.paragraphs,
          tables: event.status.tables,
          sheets: event.status.sheets,
          slides: event.status.slides,
          embeddedObjects: event.status.embeddedObjects,
        });
        return;
      }
      const element = mapPoicElement(event);
      if (element) {
        elements += 1;
        send({ type: "preview", ...element });
      }
    });
    call.on("end", finish);
    call.on("error", (error) => {
      send({ type: "grpc-error", code: error.code, message: error.message });
      finish();
    });
    response.on("close", () => call.cancel());

    // Identity fields ride the first chunk only; the last chunk is marked
    // complete (a single-chunk upload is the common case).
    const totalChunks = Math.ceil(body.length / POIC_CHUNK_BYTES);
    for (let index = 0; index < totalChunks; index += 1) {
      const first = index === 0;
      call.write({
        ...(first ? { documentId: filename, filename, contentType } : {}),
        data: body.subarray(index * POIC_CHUNK_BYTES, (index + 1) * POIC_CHUNK_BYTES),
        complete: index === totalChunks - 1,
      });
    }
    call.end();
  },
);

// Status badge for the native ASR tab; cached like the other probes.
surface.get("/api/asr/status", (_request, response) => {
  asrBridge.status().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// Raw media bytes in (same 500 MiB cap as /api/parse), one NDJSON line per
// transcription event out: a media line from MediaInfo, partial/final lines
// as the decoder commits segments (finals replace partials by index), a
// complete line from the trailer, grpc-error on failure, done last. The
// page sends the model/language/task options as query params.
surface.post(
  "/api/asr/transcribe",
  express.raw({ type: () => true, limit: MAX_UPLOAD }),
  (request, response) => {
    const body = request.body;
    if (!Buffer.isBuffer(body) || body.length === 0) {
      response.status(400).json({ error: "empty upload" });
      return;
    }
    const clients = asrBridge.clients();
    if (clients.error) {
      response.status(502).json({ error: `asr proto unavailable: ${clients.error.message}` });
      return;
    }

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const startedAt = Date.now();
    let finals = 0;
    let finished = false;
    const send = (value) => response.write(`${JSON.stringify(value)}\n`);
    const finish = () => {
      if (finished) return;
      finished = true;
      send({ type: "done", segments: finals, elapsedMs: Date.now() - startedAt });
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = clients.rpc.Transcribe({ deadline });

    call.on("data", (event) => {
      if (event.mediaInfo) {
        const media = event.mediaInfo;
        send({
          type: "media",
          durationMs: Number(media.durationMs) || 0,
          audioCodec: media.audioCodec,
          sampleRateHz: media.sampleRateHz,
          channels: media.channels,
          hasVideo: Boolean(media.hasVideo),
          videoCodec: media.videoCodec || undefined,
        });
      } else if (event.partialSegment) {
        send({ type: "partial", ...mapAsrSegment(event.partialSegment) });
      } else if (event.finalSegment) {
        finals += 1;
        send({ type: "final", ...mapAsrSegment(event.finalSegment) });
      } else if (event.keyframe) {
        // Keyframes are not requested by the page; report them compactly if
        // a future option turns them on, never the PNG bytes.
        send({ type: "keyframe", timestampMs: Number(event.keyframe.timestampMs) || 0 });
      } else if (event.complete) {
        const complete = event.complete;
        send({
          type: "complete",
          language: complete.language,
          durationMs: Number(complete.durationMs) || 0,
          segmentCount: complete.segmentCount,
          tokenCount: Number(complete.tokenCount) || 0,
          keyframeCount: complete.keyframeCount,
        });
      }
    });
    call.on("end", finish);
    call.on("error", (error) => {
      send({ type: "grpc-error", code: error.code, message: error.message });
      finish();
    });
    response.on("close", () => call.cancel());

    // Options ride the first message alone, then the encoded media in file
    // order, then half-close.
    call.write({
      options: {
        model: String(request.query.model || ""),
        language: String(request.query.language || ""),
        task: request.query.task === "translate" ? "TASK_TRANSLATE" : "TASK_TRANSCRIBE",
        wordTimestamps: request.query.word_timestamps === "true",
      },
    });
    for (let offset = 0; offset < body.length; offset += ASR_CHUNK_BYTES) {
      call.write({ chunk: { data: body.subarray(offset, offset + ASR_CHUNK_BYTES) } });
    }
    call.end();
  },
);

// Status badge for the native Enrich tab; cached like the other probes.
surface.get("/api/enrich/status", (_request, response) => {
  enrichBridge.status().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// JSON in ({ code, formula, image: {base64, mime}, describe, chart,
// vlmEndpoint }), one NDJSON line per enrichment event out. The bridge maps
// the form fields into a minimal ai.pipestream.document.v1.Document —
// pasted code as a CodeItem, a pasted formula as a FormulaItem, an uploaded
// image as a PictureItem with an inline data-URI ImageRef (labelled CHART
// when chart extraction is requested, which is how the service selects
// pictures for that job) — and streams EnrichDocument with the document
// inline in the options message.
surface.post(
  "/api/enrich/annotate",
  express.json({ limit: MAX_UPLOAD }),
  (request, response) => {
    const clients = enrichBridge.clients();
    if (clients.error) {
      response.status(502).json({ error: `enrich proto unavailable: ${clients.error.message}` });
      return;
    }
    const body = request.body || {};
    const code = typeof body.code === "string" ? body.code.trim() : "";
    const formula = typeof body.formula === "string" ? body.formula.trim() : "";
    const image = body.image && typeof body.image.base64 === "string" && body.image.base64
      ? { base64: body.image.base64, mime: String(body.image.mime || "image/png") }
      : null;
    const describe = Boolean(body.describe) && Boolean(image);
    const chart = Boolean(body.chart) && Boolean(image);
    if (!code && !formula && !describe && !chart) {
      response.status(400).json({ error: "nothing to enrich: paste code or a formula, or upload an image" });
      return;
    }

    const document = { name: "web-demo" };
    const texts = [];
    if (code) {
      texts.push({
        code: {
          selfRef: `#/texts/${texts.length}`,
          label: "DOC_ITEM_LABEL_CODE",
          orig: code,
          text: code,
        },
      });
    }
    if (formula) {
      texts.push({
        formula: {
          base: {
            selfRef: `#/texts/${texts.length}`,
            label: "DOC_ITEM_LABEL_FORMULA",
            orig: formula,
            text: formula,
          },
        },
      });
    }
    if (texts.length > 0) document.texts = texts;
    if (image) {
      document.pictures = [{
        selfRef: "#/pictures/0",
        label: chart ? "DOC_ITEM_LABEL_CHART" : "DOC_ITEM_LABEL_PICTURE",
        image: {
          mimetype: image.mime,
          uri: `data:${image.mime};base64,${image.base64}`,
        },
      }];
    }

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const startedAt = Date.now();
    let annotations = 0;
    let finished = false;
    const send = (value) => response.write(`${JSON.stringify(value)}\n`);
    const finish = () => {
      if (finished) return;
      finished = true;
      send({ type: "done", annotations, elapsedMs: Date.now() - startedAt });
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = clients.rpc.EnrichDocument({ deadline });

    call.on("data", (event) => {
      if (event.started) {
        const started = event.started;
        send({
          type: "started",
          pictureDescriptions: started.pictureDescriptions,
          chartExtractions: started.chartExtractions,
          codeEnrichments: started.codeEnrichments,
          formulaEnrichments: started.formulaEnrichments,
        });
      } else if (event.annotation) {
        annotations += 1;
        send(mapEnrichAnnotation(event.annotation));
      } else if (event.skipped) {
        const skipped = event.skipped;
        send({
          type: "skipped",
          selfRef: skipped.selfRef,
          reason: stripEnum(skipped.reason, "SKIP_REASON_"),
          detail: skipped.detail || undefined,
        });
      } else if (event.complete) {
        const complete = event.complete;
        send({
          type: "complete",
          succeeded: complete.succeeded,
          skipped: complete.skipped,
          failed: complete.failed,
        });
      }
    });
    call.on("end", finish);
    call.on("error", (error) => {
      send({ type: "grpc-error", code: error.code, message: error.message });
      finish();
    });
    response.on("close", () => call.cancel());

    call.write({
      options: {
        doPictureDescription: describe,
        doChartExtraction: chart,
        doCodeEnrichment: Boolean(code),
        doFormulaEnrichment: Boolean(formula),
        vlmEndpoint: String(body.vlmEndpoint || ""),
        document,
      },
    });
    call.end();
  },
);

// Status badge for the native VLM Convert tab; cached like the other
// probes. The payload's vlmConfigured/endpoint fields let the page say
// "reachable but no VLM endpoint configured" instead of a generic failure.
surface.get("/api/vlm-convert/status", (_request, response) => {
  vlmConvertBridge.status().then(
    (status) => response.json(status),
    () => response.json({ reachable: false }),
  );
});

// JSON in ({ preset, presetRaw, responseFormat, prompt, endpoint, pages:
// [{ pageNo, png: base64, width, height }] }), one NDJSON line per
// conversion event out: started lines as pages enter the model queue, a
// page line per converted Document fragment (in completion order, keyed by
// pageNo), raw lines for mapping failures or endpoint errors, a complete
// line from the trailer, grpc-error on failure, done last. A missing VLM
// endpoint surfaces as the service's FAILED_PRECONDITION grpc-error.
surface.post(
  "/api/vlm-convert/convert",
  express.json({ limit: MAX_UPLOAD }),
  (request, response) => {
    const clients = vlmConvertBridge.clients();
    if (clients.error) {
      response.status(502).json({ error: `vlm-convert proto unavailable: ${clients.error.message}` });
      return;
    }
    const body = request.body || {};
    const pages = Array.isArray(body.pages) ? body.pages : [];
    if (pages.length === 0 || pages.some((page) => typeof page.png !== "string" || !page.png)) {
      response.status(400).json({ error: "pages must be a non-empty array of { pageNo, png (base64) }" });
      return;
    }

    response.setHeader("Content-Type", "application/x-ndjson");
    response.setHeader("Cache-Control", "no-store");

    const startedAt = Date.now();
    let pagesOut = 0;
    let finished = false;
    const send = (value) => response.write(`${JSON.stringify(value)}\n`);
    const finish = () => {
      if (finished) return;
      finished = true;
      send({ type: "done", pages: pagesOut, elapsedMs: Date.now() - startedAt });
      response.end();
    };

    const deadline = Date.now() + 10 * 60 * 1000;
    const call = clients.rpc.ConvertPages({ deadline });

    call.on("data", (event) => {
      if (event.pageStarted) {
        send({ type: "started", pageNo: event.pageStarted.pageNo });
      } else if (event.pageDocument) {
        pagesOut += 1;
        send(mapVlmPageDocument(event.pageDocument));
      } else if (event.pageRaw) {
        const raw = event.pageRaw;
        send({
          type: "raw",
          pageNo: raw.pageNo,
          text: capChars(raw.text, 2048) || undefined,
          error: raw.error || undefined,
        });
      } else if (event.complete) {
        const complete = event.complete;
        send({
          type: "complete",
          pagesStarted: complete.pagesStarted,
          pagesOk: complete.pagesOk,
          pagesFailed: complete.pagesFailed,
        });
      }
    });
    call.on("end", finish);
    call.on("error", (error) => {
      send({ type: "grpc-error", code: error.code, message: error.message });
      finish();
    });
    response.on("close", () => call.cancel());

    // Enum names pass through only when well-formed; anything else falls
    // back to the preset's default (UNSPECIFIED).
    const enumOrDefault = (value, prefix, fallback) =>
      (typeof value === "string" && value.startsWith(prefix) && /^[A-Z0-9_]+$/.test(value) ? value : fallback);
    call.write({
      options: {
        preset: enumOrDefault(body.preset, "VLM_PRESET_", "VLM_PRESET_UNSPECIFIED"),
        presetRaw: String(body.presetRaw || ""),
        responseFormat: enumOrDefault(body.responseFormat, "RESPONSE_FORMAT_", "RESPONSE_FORMAT_UNSPECIFIED"),
        prompt: String(body.prompt || ""),
        endpoint: String(body.endpoint || ""),
      },
    });
    for (const page of pages) {
      call.write({
        pageImage: {
          png: Buffer.from(page.png, "base64"),
          pageNo: Number(page.pageNo) || 0,
          width: Number(page.width) || 0,
          height: Number(page.height) || 0,
        },
      });
    }
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
