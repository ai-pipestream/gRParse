// The demo shell's peer registry: one entry per service the shell can dial,
// naming the sibling repository that owns its wire contract, the service
// proto inside it, and the include root its imports resolve against.
//
// This module is the single source of the list. server.js reads it to load
// the info RPC and native-tab clients; sync-peer-protos.sh reads it (via
// `node peers.js --list`) to know which include trees to vendor into
// peer-protos/, and `node peers.js --check` (also `node server.js
// --check-protos`) loads every contract without touching the network.
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const grpc = require("@grpc/grpc-js");
const protoLoader = require("@grpc/proto-loader");

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

// The vendored copies of every peer's include tree, laid out as
// peer-protos/<repo>/<include>/... so the same resolver walks them.
const PEER_PROTOS_DIR = path.join(__dirname, "peer-protos");

// Where the sibling repos live, relative to this file. A plain workspace
// checkout is <ws>/gRParse/examples/web-demo (three levels up); a git
// worktree adds one (worktrees/gRParse-shell/examples/web-demo); inside the
// demo image both collapse to "/", where the Dockerfile copies the vendored
// trees (a bind mount at the same path still overrides them). The vendored
// peer-protos/ directory comes last so a developer editing a sibling's proto
// sees the edit live, while a checkout without siblings still works.
const WORKSPACE_CANDIDATES = [
  path.resolve(__dirname, "..", "..", ".."),
  path.resolve(__dirname, "..", "..", "..", ".."),
  PEER_PROTOS_DIR,
];

const LOAD_OPTIONS = { enums: String, longs: Number, defaults: true, oneofs: true };

function resolveServiceProto(known, candidates = WORKSPACE_CANDIDATES) {
  for (const workspace of candidates) {
    const root = path.join(workspace, known.repo);
    const file = path.join(root, known.proto);
    if (fs.existsSync(file)) {
      return { file, includeDirs: [path.join(root, known.include)] };
    }
  }
  throw new Error(`${known.proto} not found under ${candidates.map((w) => path.join(w, known.repo)).join(" or ")}`);
}

// Loads one peer's contract and returns its service constructor. The
// service name is walked through the package definition so a proto that
// parses but no longer declares the service fails here, not on first use.
function loadServiceCtor(name, resolved) {
  const known = KNOWN_UIS[name];
  if (!known) throw new Error(`no proto map entry for ${name}`);
  const loaded = protoLoader.loadSync(resolved.file, { includeDirs: resolved.includeDirs, ...LOAD_OPTIONS });
  const definition = grpc.loadPackageDefinition(loaded);
  const ctor = known.service.split(".").reduce((node, part) => node && node[part], definition);
  if (typeof ctor !== "function") throw new Error(`${known.service} not found in ${resolved.file}`);
  return ctor;
}

// Unique (repo, include) pairs, in registry order: the trees the shell
// needs on disk.
function includeTrees() {
  const seen = new Set();
  const trees = [];
  for (const known of Object.values(KNOWN_UIS)) {
    const key = `${known.repo}/${known.include}`;
    if (seen.has(key)) continue;
    seen.add(key);
    trees.push({ repo: known.repo, include: known.include });
  }
  return trees;
}

// Loads every registry entry's contract from `candidates` (no network) and
// returns one {name, file, error} row per entry; error is null on success.
function checkProtos(candidates = WORKSPACE_CANDIDATES) {
  return Object.keys(KNOWN_UIS).map((name) => {
    try {
      const resolved = resolveServiceProto(KNOWN_UIS[name], candidates);
      loadServiceCtor(name, resolved);
      return { name, file: resolved.file, error: null };
    } catch (error) {
      return { name, file: null, error: error.message };
    }
  });
}

// Prints the check report and returns the exit code the caller should use.
function reportCheck(rows, log = console.log) {
  let failures = 0;
  for (const row of rows) {
    if (row.error) {
      failures += 1;
      log(`FAIL ${row.name}: ${row.error}`);
    } else {
      log(`ok   ${row.name}: ${row.file}`);
    }
  }
  log(failures === 0 ? `${rows.length} peer contracts load` : `${failures} of ${rows.length} peer contracts failed to load`);
  return failures === 0 ? 0 : 1;
}

function main(argv) {
  const mode = argv[0];
  if (mode === "--list") {
    for (const tree of includeTrees()) console.log(`${tree.repo}\t${tree.include}`);
    return 0;
  }
  if (mode === "--check") {
    return reportCheck(checkProtos());
  }
  if (mode === "--check-vendored") {
    return reportCheck(checkProtos([PEER_PROTOS_DIR]));
  }
  console.error("usage: node peers.js --list | --check | --check-vendored");
  return 2;
}

module.exports = {
  KNOWN_UIS,
  PEER_PROTOS_DIR,
  WORKSPACE_CANDIDATES,
  LOAD_OPTIONS,
  resolveServiceProto,
  loadServiceCtor,
  includeTrees,
  checkProtos,
  reportCheck,
};

if (require.main === module) {
  process.exitCode = main(process.argv.slice(2));
}
