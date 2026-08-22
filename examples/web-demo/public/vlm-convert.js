// Streams NDJSON conversion events from the bridge's /api/vlm-convert/convert
// relay: the page uploads one PNG per page (base64 in a JSON body, with the
// pixel size read client-side so DocTags locations scale), and each page's
// Document fragment renders as a card the moment its VLM call returns —
// pages finish out of order, keyed by page number.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const results = document.getElementById("results");
const statsBar = document.getElementById("stats");

const totals = { pages: 0, failed: 0, items: 0 };
let startedAt = 0;
let clock = null;
// One card per page, created by its started event (or on demand when the
// page's result arrives first) and filled by its page/raw event.
let cards = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function resetRun() {
  results.innerHTML = "";
  totals.pages = 0;
  totals.failed = 0;
  totals.items = 0;
  statsBar.hidden = false;
  setStat("stat-pages", "0");
  setStat("stat-failed", "0");
  setStat("stat-items", "0");
  setStat("stat-elapsed", "0.0s");
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);
  cards = new Map();
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  dropzone.classList.remove("busy");
}

function banner(kind, message) {
  const element = document.createElement("div");
  element.className = `banner ${kind}`;
  element.textContent = message;
  results.appendChild(element);
}

function pageCard(pageNo) {
  let card = cards.get(pageNo);
  if (!card) {
    card = document.createElement("div");
    card.className = "vlm-card";
    const title = document.createElement("h2");
    title.textContent = `page ${pageNo}`;
    const status = document.createElement("p");
    status.className = "hint";
    status.textContent = "queued…";
    card.append(title, status);
    card.statusLine = status;
    cards.set(pageNo, card);
    results.appendChild(card);
  }
  return card;
}

function renderItems(card, items) {
  const table = document.createElement("table");
  table.className = "warc-table";
  const header = document.createElement("thead");
  header.innerHTML = "<tr><th>label</th><th>text</th></tr>";
  table.appendChild(header);
  const body = document.createElement("tbody");
  for (const item of items) {
    const row = document.createElement("tr");
    const label = document.createElement("td");
    label.textContent = item.label;
    const text = document.createElement("td");
    text.textContent = item.text;
    row.append(label, text);
    body.appendChild(row);
  }
  table.appendChild(body);
  card.appendChild(table);
}

function handleEvent(event) {
  if (event.type === "started") {
    pageCard(event.pageNo).statusLine.textContent = "in the model queue…";
  } else if (event.type === "page") {
    const card = pageCard(event.pageNo);
    card.statusLine.textContent =
      `${event.texts} text item(s), ${event.tables} table(s), ${event.pictures} picture(s)`;
    renderItems(card, event.items || []);
    totals.pages += 1;
    totals.items += event.texts + event.tables + event.pictures;
    setStat("stat-pages", `${totals.pages}`);
    setStat("stat-items", `${totals.items}`);
  } else if (event.type === "raw") {
    const card = pageCard(event.pageNo);
    if (event.error) {
      card.statusLine.textContent = `failed: ${event.error}`;
      card.classList.add("failed");
    } else {
      card.statusLine.textContent = "mapping failed; raw model output:";
    }
    if (event.text) {
      const pre = document.createElement("pre");
      pre.className = "vlm-raw";
      pre.textContent = event.text;
      card.appendChild(pre);
    }
    totals.failed += 1;
    setStat("stat-failed", `${totals.failed}`);
  } else if (event.type === "complete") {
    const kind = event.pagesFailed > 0 ? "warn" : "done";
    banner(kind, `Conversion complete: ${event.pagesOk} of ${event.pagesStarted} page(s) converted, `
      + `${event.pagesFailed} failed.`);
  } else if (event.type === "grpc-error") {
    // FAILED_PRECONDITION (gRPC code 9) is the service saying no VLM
    // endpoint is configured and the request did not override it.
    const detail = event.code === 9
      ? `${event.message} — the service has no VLM endpoint configured; set the endpoint override above.`
      : event.message;
    banner("error", `gRPC error (code ${event.code}): ${detail}`);
  } else if (event.type === "done") {
    banner("done", `Done: ${event.pages} page(s) in ${(event.elapsedMs / 1000).toFixed(1)}s.`);
  }
}

// Reads one PNG into { base64, width, height }: the bytes via FileReader,
// the pixel size by decoding it as an image (the service scales DocTags
// locations by the raster size).
function readPage(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error(`could not read ${file.name}`));
    reader.onload = () => {
      const dataUri = String(reader.result);
      const image = new Image();
      image.onerror = () => reject(new Error(`${file.name} does not decode as an image`));
      image.onload = () => resolve({
        base64: dataUri.slice(dataUri.indexOf(",") + 1),
        width: image.naturalWidth,
        height: image.naturalHeight,
      });
      image.src = dataUri;
    };
    reader.readAsDataURL(file);
  });
}

async function convertFiles(fileList) {
  const files = Array.from(fileList)
    .filter((file) => file.type === "image/png" || /\.png$/i.test(file.name))
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  resetRun();
  if (files.length === 0) {
    finishRun();
    banner("error", "No PNG files: the service accepts PNG page images only.");
    return;
  }
  dropzone.classList.add("busy");
  try {
    const pages = [];
    for (const [index, file] of files.entries()) {
      const page = await readPage(file);
      pages.push({ pageNo: index + 1, png: page.base64, width: page.width, height: page.height });
    }
    const body = {
      preset: document.getElementById("opt-preset").value,
      presetRaw: document.getElementById("opt-preset-raw").value.trim(),
      responseFormat: document.getElementById("opt-format").value,
      endpoint: document.getElementById("opt-endpoint").value.trim(),
      pages,
    };
    const response = await fetch("api/vlm-convert/convert", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!response.ok || !response.body) {
      let detail = `HTTP ${response.status}`;
      try {
        const payload = await response.json();
        if (payload && payload.error) detail = payload.error;
      } catch (_error) { /* the status line is the fallback */ }
      banner("error", `Request failed: ${detail}`);
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
document.getElementById("vlm-options").addEventListener("click", (event) => event.stopPropagation());

dropzone.addEventListener("click", () => fileInput.click());
fileInput.addEventListener("change", () => {
  if (fileInput.files.length > 0) convertFiles(fileInput.files);
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
  if (event.dataTransfer.files.length > 0) convertFiles(event.dataTransfer.files);
});

fetch("api/vlm-convert/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (!status.reachable) {
      chip.textContent = "service unreachable";
      chip.className = "health bad";
      return;
    }
    // This service is an HTTP client of an external VLM server: reachable
    // with no configured endpoint means every conversion fails
    // FAILED_PRECONDITION unless the request overrides the endpoint.
    if (status.vlmConfigured) {
      chip.textContent = `grpc-vlm-convert ${status.version || ""} reachable — VLM: ${status.endpoint}`.trim();
      chip.className = "health ok";
    } else {
      chip.textContent = "service reachable — no VLM endpoint configured (set an override below)";
      chip.className = "health warn";
    }
  })
  .catch(() => {
    const chip = document.getElementById("health");
    chip.textContent = "bridge unreachable";
    chip.className = "health bad";
  });
