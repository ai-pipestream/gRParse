// Streams NDJSON record events from the bridge's /api/fastwarc/parse relay
// and grows the record table as each line lands: one row per record, its
// payload length filled in when the record closes, preview expandable.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const results = document.getElementById("results");
const statsBar = document.getElementById("stats");

const totals = { records: 0, payloadBytes: 0 };
let startedAt = 0;
let clock = null;
// The table of the current run; rows arrive as the stream does.
let tableBody = null;
// The row still open for payload/preview, closed by the next "end" event.
let currentRow = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GiB`;
}

function updateStats() {
  const elapsed = (performance.now() - startedAt) / 1000;
  setStat("stat-records", `${totals.records}`);
  setStat("stat-payload", formatBytes(totals.payloadBytes));
  setStat("stat-rate", elapsed > 0 ? (totals.payloadBytes / (1024 * 1024) / elapsed).toFixed(1) : "0");
}

function resetRun() {
  results.innerHTML = "";
  totals.records = 0;
  totals.payloadBytes = 0;
  currentRow = null;
  statsBar.hidden = false;
  updateStats();
  setStat("stat-elapsed", "0.0s");
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);

  const table = document.createElement("table");
  table.className = "warc-table";
  const header = document.createElement("thead");
  header.innerHTML = "<tr><th>type</th><th>stream pos</th><th>declared length</th>"
    + "<th>payload length</th><th>record id</th><th>HTTP content type</th></tr>";
  table.appendChild(header);
  tableBody = document.createElement("tbody");
  table.appendChild(tableBody);
  results.appendChild(table);
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  updateStats();
  dropzone.classList.remove("busy");
}

function cell(row, text) {
  const td = document.createElement("td");
  td.textContent = text;
  row.appendChild(td);
  return td;
}

// The preview event always follows its record's start; attach it as a
// toggleable second row under the record's own row.
function attachPreview(text) {
  if (!currentRow) return;
  const previewRow = document.createElement("tr");
  previewRow.className = "preview-row";
  previewRow.hidden = true;
  const wrapper = document.createElement("td");
  wrapper.colSpan = 6;
  const pre = document.createElement("pre");
  pre.textContent = text;
  wrapper.appendChild(pre);
  previewRow.appendChild(wrapper);
  currentRow.after(previewRow);
  currentRow.classList.add("has-preview");
  currentRow.title = "click to toggle the payload preview";
  currentRow.addEventListener("click", () => { previewRow.hidden = !previewRow.hidden; });
}

function banner(kind, message) {
  const element = document.createElement("div");
  element.className = `banner ${kind}`;
  element.textContent = message;
  results.appendChild(element);
}

function handleEvent(event) {
  if (event.type === "start") {
    const row = document.createElement("tr");
    cell(row, event.recordType);
    cell(row, `${event.streamPos}`);
    cell(row, formatBytes(event.contentLength || 0));
    cell(row, "…");
    cell(row, event.recordId || "");
    cell(row, event.httpContentType || (event.isHttp ? "(none declared)" : ""));
    tableBody.appendChild(row);
    currentRow = row;
    totals.records += 1;
    updateStats();
  } else if (event.type === "preview") {
    attachPreview(event.text);
  } else if (event.type === "end") {
    if (currentRow) currentRow.cells[3].textContent = formatBytes(event.payloadLength || 0);
    totals.payloadBytes += event.payloadLength || 0;
    updateStats();
  } else if (event.type === "error") {
    if (event.recoverable) {
      // Recoverable record errors stay in stream order, inline in the table.
      const row = document.createElement("tr");
      row.className = "warn-row";
      const td = document.createElement("td");
      td.colSpan = 6;
      td.textContent = `recoverable error at stream pos ${event.streamPos}: ${event.message}`;
      row.appendChild(td);
      tableBody.appendChild(row);
      currentRow = null;
    } else {
      banner("error", `Stream error at stream pos ${event.streamPos ?? "?"}: ${event.message}`);
    }
  } else if (event.type === "done") {
    banner("done", `Done: ${event.records} record(s), ${formatBytes(event.payloadBytes)} of payload in ${(event.elapsedMs / 1000).toFixed(1)}s.`);
  }
}

async function parseFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  const query = new URLSearchParams({
    parse_http: document.getElementById("opt-parse-http").checked,
    verify_digests: document.getElementById("opt-verify-digests").checked,
    include_payload: document.getElementById("opt-include-payload").checked,
  });
  try {
    const response = await fetch(`api/fastwarc/parse?${query}`, { method: "POST", body: file });
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

// The options live inside the dropzone; keep their clicks from opening the
// file picker.
document.getElementById("warc-options").addEventListener("click", (event) => event.stopPropagation());

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

fetch("api/fastwarc/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (status.reachable) {
      chip.textContent = "service reachable";
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
