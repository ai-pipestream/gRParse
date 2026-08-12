// Streams NDJSON page events from the bridge and paints each page as it
// lands: provenance boxes on a page-scaled canvas, text in reading order.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const sampleButton = document.getElementById("sample-button");
const downloadButton = document.getElementById("download-json");
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
// Everything the stream delivered for the current run, downloadable as JSON.
let run = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function resetRun(filename) {
  results.innerHTML = "";
  Object.keys(totals).forEach((key) => { totals[key] = 0; });
  ["stat-pages", "stat-texts", "stat-digital", "stat-ocr", "stat-tables", "stat-pictures", "stat-barcodes"]
    .forEach((id) => setStat(id, "0"));
  statsBar.hidden = false;
  legend.hidden = false;
  run = { filename, startedAt: new Date().toISOString(), pages: [], complete: null };
  downloadButton.disabled = true;
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
  // When this page event actually arrived, relative to upload start — the
  // visible proof that pages stream instead of waiting for the whole document.
  const arrival = (performance.now() - startedAt) / 1000;
  const badge = document.createElement("span");
  badge.className = "arrival";
  badge.textContent = `arrived at ${arrival.toFixed(1)}s`;
  heading.appendChild(badge);
  card.appendChild(heading);

  // Canvas scaled to the advertised page size (top-left origin, pixel units).
  // When the server sends a page preview (GRPARSE_PAGE_IMAGES=on) it sits
  // under the transparent canvas, so the boxes mask the real page; the white
  // wrapper stands in when there is no preview or it is toggled off.
  const width = event.size ? event.size.width : 850;
  const height = event.size ? event.size.height : 1100;
  const wrapper = document.createElement("div");
  wrapper.className = "page-canvas";
  if (event.image) {
    const pageImage = document.createElement("img");
    pageImage.src = event.image;
    pageImage.alt = `Page ${event.pageNumber} render`;
    wrapper.appendChild(pageImage);
  }
  const canvas = document.createElement("canvas");
  const scale = Math.min(1, 920 / width);
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  wrapper.appendChild(canvas);
  card.appendChild(wrapper);

  const ctx = canvas.getContext("2d");

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

  // Reading-order text, styled by label. Digital extraction can be
  // word-granular, so consecutive items whose boxes overlap vertically are
  // joined into one visual line instead of one div per item.
  const textPane = document.createElement("div");
  textPane.className = "page-text";
  let currentLine = null;
  let previousBox = null;
  const sameVisualLine = (a, b) => {
    if (!a || !b) return false;
    const overlap = Math.min(a.b, b.b) - Math.max(a.t, b.t);
    return overlap > 0.5 * Math.min(a.b - a.t, b.b - b.t);
  };
  for (const text of event.texts) {
    if (!currentLine || !sameVisualLine(previousBox, text.bbox)) {
      currentLine = document.createElement("div");
      if (text.label === "title" || text.label === "section_header") currentLine.className = "t-title";
      else if (text.label === "list_item") currentLine.className = "t-list";
      textPane.appendChild(currentLine);
      currentLine.textContent = text.text;
    } else {
      currentLine.textContent += ` ${text.text}`;
    }
    previousBox = text.bbox;
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
  if (event.type === "page") {
    renderPage(event);
    // The preview data URI stays out of the JSON export: it is a rendering
    // aid, not parse data, and it would dwarf everything else in the file.
    if (run) {
      const { image, ...data } = event;
      run.pages.push({ ...data, arrivalSeconds: (performance.now() - startedAt) / 1000 });
    }
  } else if (event.type === "complete") {
    if (run) {
      run.complete = { totalPages: event.totalPages, elapsedSeconds: (performance.now() - startedAt) / 1000 };
      downloadButton.disabled = false;
    }
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
  resetRun(file.name);
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

document.getElementById("toggle-image").addEventListener("change", (event) => {
  results.classList.toggle("hide-page-images", !event.target.checked);
});

dropzone.addEventListener("click", () => fileInput.click());
sampleButton.addEventListener("click", async (event) => {
  event.stopPropagation(); // the surrounding dropzone opens the file picker on click
  const response = await fetch("/sample.pdf");
  if (!response.ok) {
    banner("error", `Could not fetch the bundled sample: HTTP ${response.status}`);
    return;
  }
  const bytes = await response.blob();
  parseFile(new File([bytes], "sample.pdf", { type: "application/pdf" }));
});
downloadButton.addEventListener("click", () => {
  if (!run) return;
  const blob = new Blob([JSON.stringify(run, null, 2)], { type: "application/json" });
  const anchor = document.createElement("a");
  anchor.href = URL.createObjectURL(blob);
  anchor.download = `${run.filename.replace(/\.[^.]+$/, "")}.grparse.json`;
  anchor.click();
  URL.revokeObjectURL(anchor.href);
});
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
