// Streams NDJSON page events from the bridge and paints each page as it
// lands: provenance boxes on a page-scaled canvas, text in reading order.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const results = document.getElementById("results");
const statsBar = document.getElementById("stats");
const legend = document.getElementById("legend");

const BOX_COLORS = {
  text: "#8b949e",
  title: "#e3b341",
  section_header: "#e3b341",
  list_item: "#56d4dd",
  table: "#3fb950",
  picture: "#d2a8ff",
};

const totals = { pages: 0, texts: 0, digital: 0, ocr: 0, tables: 0, pictures: 0, barcodes: 0 };
let startedAt = 0;
let clock = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function resetRun() {
  results.innerHTML = "";
  Object.keys(totals).forEach((key) => { totals[key] = 0; });
  ["stat-pages", "stat-texts", "stat-digital", "stat-ocr", "stat-tables", "stat-pictures", "stat-barcodes"]
    .forEach((id) => setStat(id, "0"));
  statsBar.hidden = false;
  legend.hidden = false;
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  dropzone.classList.remove("busy");
}

function contentTypeFor(name) {
  const ext = name.toLowerCase().split(".").pop();
  return {
    pdf: "application/pdf", png: "image/png", jpg: "image/jpeg",
    jpeg: "image/jpeg", tif: "image/tiff", tiff: "image/tiff",
  }[ext] || "";
}

function drawBox(ctx, scale, bbox, color, dashed) {
  if (!bbox) return;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.setLineDash(dashed ? [5, 3] : []);
  ctx.strokeRect(bbox.l * scale, bbox.t * scale, (bbox.r - bbox.l) * scale, (bbox.b - bbox.t) * scale);
}

function renderPage(event) {
  const card = document.createElement("article");
  card.className = "page-card";

  const heading = document.createElement("h2");
  heading.textContent = `Page ${event.pageNumber}` +
    (event.totalPages ? ` of ${event.totalPages}` : "");
  card.appendChild(heading);

  // Canvas scaled to the advertised page size (top-left origin, pixel units).
  const width = event.size ? event.size.width : 850;
  const height = event.size ? event.size.height : 1100;
  const canvas = document.createElement("canvas");
  const scale = Math.min(1, 920 / width);
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  card.appendChild(canvas);

  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  const sourceByRef = new Map(event.offsets.map((offset) => [offset.ref, offset.source]));

  for (const text of event.texts) {
    const color = BOX_COLORS[text.label] || BOX_COLORS.text;
    const dashed = sourceByRef.get(text.ref) === "ocr";
    drawBox(ctx, scale, text.bbox, color, dashed);
  }
  for (const table of event.tables) {
    drawBox(ctx, scale, table.bbox, BOX_COLORS.table, false);
    for (const cell of table.cells) drawBox(ctx, scale, cell.bbox, BOX_COLORS.table, true);
  }
  for (const picture of event.pictures) {
    drawBox(ctx, scale, picture.bbox, BOX_COLORS.picture, false);
  }

  // Reading-order text, styled by label.
  const textPane = document.createElement("div");
  textPane.className = "page-text";
  for (const text of event.texts) {
    const line = document.createElement("div");
    if (text.label === "title" || text.label === "section_header") line.className = "t-title";
    else if (text.label === "list_item") line.className = "t-list";
    line.textContent = text.text;
    textPane.appendChild(line);
  }
  card.appendChild(textPane);

  const extras = [];
  for (const table of event.tables) {
    extras.push(`table ${table.numRows}×${table.numCols} (${table.cells.length} cells)`);
  }
  for (const picture of event.pictures) {
    const top = picture.classes[0];
    let entry = "picture";
    if (top) entry += ` <code>${top.name} ${(top.confidence * 100).toFixed(0)}%</code>`;
    for (const barcode of picture.barcodes) {
      entry += ` <code>${barcode.format}: ${barcode.value}</code>`;
    }
    extras.push(entry);
  }
  if (extras.length > 0) {
    const extrasRow = document.createElement("div");
    extrasRow.className = "page-extras";
    extrasRow.innerHTML = extras.join(" · ");
    card.appendChild(extrasRow);
  }

  results.appendChild(card);

  totals.pages += 1;
  totals.texts += event.texts.length;
  totals.tables += event.tables.length;
  totals.pictures += event.pictures.length;
  for (const offset of event.offsets) totals[offset.source === "digital" ? "digital" : "ocr"] += 1;
  for (const picture of event.pictures) totals.barcodes += picture.barcodes.length;
  setStat("stat-pages", event.totalPages ? `${totals.pages}/${event.totalPages}` : `${totals.pages}`);
  setStat("stat-texts", `${totals.texts}`);
  setStat("stat-digital", `${totals.digital}`);
  setStat("stat-ocr", `${totals.ocr}`);
  setStat("stat-tables", `${totals.tables}`);
  setStat("stat-pictures", `${totals.pictures}`);
  setStat("stat-barcodes", `${totals.barcodes}`);
}

function banner(kind, message) {
  const element = document.createElement("div");
  element.className = `banner ${kind}`;
  element.textContent = message;
  results.appendChild(element);
}

function handleEvent(event) {
  if (event.type === "page") renderPage(event);
  else if (event.type === "complete") {
    const failures = event.collectorFailures || [];
    banner("done", failures.length === 0
      ? `Complete: ${event.totalPages} page(s).`
      : `Complete with collector failures: ${failures.map((f) => f.error).join("; ")}`);
  } else if (event.type === "collector") {
    banner("done", `Collector document: ${event.texts} texts, ${event.tables} tables, ${event.pictures} pictures.`);
  } else if (event.type === "error") {
    banner("error", `Stream error: ${event.message}`);
  }
}

async function parseFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  const query = new URLSearchParams({ filename: file.name, contentType: contentTypeFor(file.name) });
  try {
    const response = await fetch(`/api/parse?${query}`, { method: "POST", body: file });
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

fetch("/api/health")
  .then((response) => response.json())
  .then((health) => {
    const chip = document.getElementById("health");
    if (health.ok) {
      chip.textContent = `service healthy (${health.target})`;
      chip.className = "health ok";
    } else {
      chip.textContent = `service unreachable: ${health.error}`;
      chip.className = "health bad";
    }
  })
  .catch(() => {
    const chip = document.getElementById("health");
    chip.textContent = "bridge unreachable";
    chip.className = "health bad";
  });
