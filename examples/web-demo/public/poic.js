// Streams NDJSON parse events from the bridge's /api/poic/parse relay and
// grows the element table as each line lands: one row per streamed content
// element (paragraph, table, sheet, slide, embedded object), with the
// document info and final status rendered as banners.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const results = document.getElementById("results");
const statsBar = document.getElementById("stats");

const totals = { elements: 0 };
let startedAt = 0;
let clock = null;
// The table of the current run; rows arrive as the stream does.
let tableBody = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function updateStats() {
  setStat("stat-elements", `${totals.elements}`);
}

function resetRun() {
  results.innerHTML = "";
  totals.elements = 0;
  statsBar.hidden = false;
  updateStats();
  setStat("stat-format", "—");
  setStat("stat-elapsed", "0.0s");
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);

  const table = document.createElement("table");
  table.className = "warc-table";
  const header = document.createElement("thead");
  header.innerHTML = "<tr><th>#</th><th>kind</th><th>detail</th><th>preview</th></tr>";
  table.appendChild(header);
  tableBody = document.createElement("tbody");
  table.appendChild(tableBody);
  results.appendChild(table);
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  dropzone.classList.remove("busy");
}

function cell(row, text) {
  const td = document.createElement("td");
  td.textContent = text;
  row.appendChild(td);
  return td;
}

// One line per element, in the table's "detail" column.
function describe(element) {
  if (element.kind === "paragraph") return element.style || "";
  if (element.kind === "table") return `${element.rows} row(s) x ${element.cols} col(s)`;
  if (element.kind === "sheet") return `sheet ${element.index}: ${element.name} (${element.rows} row(s))`;
  if (element.kind === "slide") return `slide ${element.index}${element.title ? `: ${element.title}` : ""}`;
  if (element.kind === "embedded") return element.filename || element.id || "";
  return "";
}

function previewText(element) {
  if (element.kind === "embedded") {
    return [element.contentType, element.sizeBytes !== undefined ? `${element.sizeBytes} bytes` : ""]
      .filter(Boolean)
      .join(", ");
  }
  if (element.kind === "slide" && element.notes > 0) {
    return `${element.text || ""}${element.text ? " " : ""}(${element.notes} note(s))`;
  }
  return element.text || "";
}

function banner(kind, message) {
  const element = document.createElement("div");
  element.className = `banner ${kind}`;
  element.textContent = message;
  results.appendChild(element);
}

function handleEvent(event) {
  if (event.type === "start") {
    setStat("stat-format", event.format || "unknown");
    const parts = [`Parsing ${event.format || "document"}`];
    if (event.title) parts.push(`title: ${event.title}`);
    if (event.author) parts.push(`author: ${event.author}`);
    if (event.modified) parts.push(`modified: ${event.modified}`);
    if (event.extraMetadata) parts.push(`${event.extraMetadata} extra metadata entries`);
    banner("done", parts.join(" — "));
  } else if (event.type === "preview") {
    const row = document.createElement("tr");
    cell(row, `${totals.elements + 1}`);
    cell(row, event.kind);
    cell(row, describe(event));
    cell(row, previewText(event));
    tableBody.appendChild(row);
    totals.elements += 1;
    updateStats();
  } else if (event.type === "end") {
    const counts = [
      `${event.paragraphs} paragraph(s)`,
      `${event.tables} table(s)`,
      `${event.sheets} sheet(s)`,
      `${event.slides} slide(s)`,
      `${event.embeddedObjects} embedded object(s)`,
    ].join(", ");
    banner(event.state === "ok" ? "done" : "warn", `Parse ${event.state}: ${counts}.`);
    for (const warning of event.warnings || []) banner("warn", `warning: ${warning}`);
  } else if (event.type === "grpc-error") {
    banner("error", `gRPC error (code ${event.code}): ${event.message}`);
  } else if (event.type === "done") {
    banner("done", `Done: ${event.elements} element(s) in ${(event.elapsedMs / 1000).toFixed(1)}s.`);
  }
}

async function parseFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  const query = new URLSearchParams({
    filename: file.name,
    contentType: file.type || "",
  });
  try {
    const response = await fetch(`api/poic/parse?${query}`, { method: "POST", body: file });
    if (!response.ok || !response.body) {
      banner("error", `Upload failed: HTTP ${response.status}`);
      return;
    }
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let buffered = "";
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffered += decoder.decode(value, { stream: true });
      let newline;
      while ((newline = buffered.indexOf("\n")) >= 0) {
        const line = buffered.slice(0, newline).trim();
        buffered = buffered.slice(newline + 1);
        if (line) handleEvent(JSON.parse(line));
      }
    }
  } catch (error) {
    banner("error", `Request failed: ${error.message}`);
  } finally {
    finishRun();
  }
}

dropzone.addEventListener("click", () => fileInput.click());
fileInput.addEventListener("change", () => {
  if (fileInput.files.length > 0) parseFile(fileInput.files[0]);
  fileInput.value = "";
});
dropzone.addEventListener("dragover", (event) => {
  event.preventDefault();
  dropzone.classList.add("dragover");
});
dropzone.addEventListener("dragleave", () => dropzone.classList.remove("dragover"));
dropzone.addEventListener("drop", (event) => {
  event.preventDefault();
  dropzone.classList.remove("dragover");
  if (event.dataTransfer.files.length > 0) parseFile(event.dataTransfer.files[0]);
});

fetch("api/poic/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (status.reachable) {
      chip.textContent = status.serviceVersion
        ? `grPOIc ${status.serviceVersion} (POI ${status.poiVersion}) reachable`
        : "service reachable";
      chip.className = "health ok";
    } else {
      chip.textContent = "service unreachable";
      chip.className = "health bad";
    }
  })
  .catch(() => {
    const chip = document.getElementById("health");
    chip.textContent = "bridge unreachable";
    chip.className = "health bad";
  });
