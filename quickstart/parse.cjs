// Command-line client for the quickstart: reads one document on stdin,
// sends it to gRParse's ConvertSource, and writes the requested export to
// stdout. Runs inside the demo shell image (Node, @grpc/grpc-js and the
// contract protos under /protos); ./parse is the wrapper that feeds it.
//
//   node parse.cjs --filename report.pdf [--to md|text|html|json|doctags|yaml] < report.pdf
//
// The filename matters: gRParse routes office, markup and other formats by
// name as well as by content. Status and per-collector errors go to stderr;
// the exit code is 0 on success, 2 on a partial success, 1 on failure.
"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { createRequire } = require("node:module");

const appRequire = createRequire("/app/package.json");
const grpc = appRequire("@grpc/grpc-js");
const protoLoader = appRequire("@grpc/proto-loader");

const FORMATS = {
  md: ["OUTPUT_FORMAT_MARKDOWN", "md"],
  markdown: ["OUTPUT_FORMAT_MARKDOWN", "md"],
  text: ["OUTPUT_FORMAT_TEXT", "text"],
  html: ["OUTPUT_FORMAT_HTML", "html"],
  json: ["OUTPUT_FORMAT_JSON", "json"],
  doctags: ["OUTPUT_FORMAT_DOCTAGS", "doctags"],
  yaml: ["OUTPUT_FORMAT_YAML", "yaml"],
};

function fail(message, code = 64) {
  process.stderr.write(`parse: ${message}\n`);
  process.exit(code);
}

function parseArgs(argv) {
  const options = { filename: "document", to: "md" };
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (flag === "--filename" && value) { options.filename = value; index += 1; }
    else if (flag === "--to" && value) { options.to = value.toLowerCase(); index += 1; }
    else fail(`unknown argument ${flag}; usage: parse FILE [--to ${Object.keys(FORMATS).join("|")}]`);
  }
  if (!FORMATS[options.to]) fail(`--to must be one of ${Object.keys(FORMATS).join(", ")}`);
  return options;
}

// The contracts import each other by their ai/pipestream/... paths; the image
// keeps them flat under /protos, so stage copies into that layout.
function loadParseService() {
  const root = process.env.GRPARSE_PROTO_DIR || "/protos";
  const staged = fs.mkdtempSync(path.join(os.tmpdir(), "grparse-protos-"));
  const layout = [
    ["document.proto", "ai/pipestream/document/v1/document.proto"],
    ["parse_types.proto", "ai/pipestream/parse/v1/parse_types.proto"],
    ["parse.proto", "ai/pipestream/parse/v1/parse.proto"],
    ["collectors/ebcdic.proto", "ai/pipestream/ebcdic/v1/ebcdic.proto"],
    ["collectors/lolhtml_types.proto", "lolhtml/v1/types.proto"],
    ["collectors/lolhtml_service.proto", "lolhtml/v1/lolhtml_service.proto"],
    ["collectors/opennlp_document.proto", "org/apache/opennlp/grpc/v1/opennlp_document.proto"],
    ["collectors/opennlp_annotations.proto", "org/apache/opennlp/grpc/v1/opennlp_annotations.proto"],
  ];
  for (const [source, destination] of layout) {
    const from = path.join(root, source);
    if (!fs.existsSync(from)) continue;
    const target = path.join(staged, destination);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(from, target);
  }
  const definition = protoLoader.loadSync(path.join(staged, "ai/pipestream/parse/v1/parse.proto"), {
    includeDirs: [staged], enums: String, longs: Number, defaults: false, oneofs: true,
  });
  return grpc.loadPackageDefinition(definition).ai.pipestream.parse.v1.ParseService;
}

// Read the stream rather than fd 0 directly: compose can hand the container
// a non-blocking stdin, where a synchronous read fails with EAGAIN.
async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  return Buffer.concat(chunks);
}

function waitForReady(client, seconds) {
  return new Promise((resolve, reject) => {
    client.waitForReady(Date.now() + seconds * 1000, (error) => (error ? reject(error) : resolve()));
  });
}

function convert(client, request) {
  return new Promise((resolve, reject) => {
    client.ConvertSource(request, { deadline: Date.now() + 10 * 60 * 1000 }, (error, response) =>
      (error ? reject(error) : resolve(response)));
  });
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (process.stdin.isTTY) fail("pipe a document on stdin (the ./parse wrapper does this)");
  const body = await readStdin();
  if (body.length === 0) fail("empty document on stdin");

  const [format, exportKey] = FORMATS[options.to];
  const target = process.env.GRPARSE_TARGET || "grparse:50051";
  const ParseService = loadParseService();
  const client = new ParseService(target, grpc.credentials.createInsecure(), {
    "grpc.max_receive_message_length": -1,
    "grpc.max_send_message_length": -1,
  });

  // A fresh stack is still copying models or starting collectors: wait for
  // the channel, then retry the call while a collector answers UNAVAILABLE.
  try {
    await waitForReady(client, 180);
  } catch (error) {
    fail(`gRParse at ${target} did not come up within 180 s (${error.message}); check 'docker compose logs grparse'`, 1);
  }
  const request = {
    request: {
      sources: [{ file: { base64String: body.toString("base64"), filename: options.filename } }],
      options: { toFormats: [format] },
    },
  };
  const started = Date.now();
  let converted;
  for (let attempt = 1; ; attempt += 1) {
    try {
      converted = await convert(client, request);
      break;
    } catch (error) {
      if (error.code === grpc.status.UNAVAILABLE && attempt < 10) {
        await new Promise((resolve) => setTimeout(resolve, 3000));
        continue;
      }
      fail(`${grpc.status[error.code] || error.code}: ${error.details || error.message}`, 1);
    }
  }

  const response = converted.response || {};
  const status = response.status || "CONVERSION_STATUS_UNSPECIFIED";
  for (const item of response.errors || []) {
    process.stderr.write(`parse: ${item.moduleName || item.componentType || "error"}: ${item.errorMessage}\n`);
  }
  const document = response.document || {};
  const output = (document.exports || {})[exportKey];
  if (typeof output === "string") process.stdout.write(output.endsWith("\n") ? output : `${output}\n`);
  const pages = document.doc && document.doc.pages ? Object.keys(document.doc.pages).length : 0;
  const seconds = ((Date.now() - started) / 1000).toFixed(1);
  process.stderr.write(`parse: ${options.filename}: ${status.replace("CONVERSION_STATUS_", "").toLowerCase()}, ${pages} page(s), ${seconds} s\n`);
  client.close();
  // exitCode, not process.exit(): exiting at once would cut off a large
  // export still draining into a pipe.
  if (status === "CONVERSION_STATUS_SUCCESS" && typeof output === "string") process.exitCode = 0;
  else if (status === "CONVERSION_STATUS_PARTIAL_SUCCESS") process.exitCode = 2;
  else process.exitCode = 1;
}

main().catch((error) => fail(error.stack || String(error), 1));
