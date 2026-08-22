// Streams NDJSON transcription events from the bridge's /api/asr/transcribe
// relay and grows the transcript table as each line lands: one row per
// segment index, updated in place while the segment is partial and locked
// in when its final arrives (finals replace partials by index).
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const results = document.getElementById("results");
const statsBar = document.getElementById("stats");

const totals = { segments: 0 };
let startedAt = 0;
let clock = null;
// The table of the current run; rows arrive as the stream does, keyed by
// segment index so a final overwrites its partial in place.
let tableBody = null;
let rowsByIndex = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function formatTime(ms) {
  const total = Math.floor(ms / 1000);
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  const tenths = Math.floor((ms % 1000) / 100);
  return `${minutes}:${String(seconds).padStart(2, "0")}.${tenths}`;
}

function resetRun() {
  results.innerHTML = "";
  totals.segments = 0;
  statsBar.hidden = false;
  setStat("stat-segments", "0");
  setStat("stat-media", "—");
  setStat("stat-language", "—");
  setStat("stat-elapsed", "0.0s");
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);

  const table = document.createElement("table");
  table.className = "warc-table";
  const header = document.createElement("thead");
  header.innerHTML = "<tr><th>#</th><th>start</th><th>end</th><th>text</th></tr>";
  table.appendChild(header);
  tableBody = document.createElement("tbody");
  table.appendChild(tableBody);
  results.appendChild(table);
  rowsByIndex = new Map();
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

// One row per segment index. A partial paints dim and italic; the final for
// the same index replaces its text and clears the partial styling.
function upsertSegment(segment, isFinal) {
  let row = rowsByIndex.get(segment.index);
  if (!row) {
    row = document.createElement("tr");
    for (let i = 0; i < 4; i += 1) row.appendChild(document.createElement("td"));
    tableBody.appendChild(row);
    rowsByIndex.set(segment.index, row);
  }
  row.className = isFinal ? "" : "partial-row";
  row.cells[0].textContent = `${segment.index}`;
  row.cells[1].textContent = formatTime(segment.startMs);
  row.cells[2].textContent = formatTime(segment.endMs);
  row.cells[3].textContent = segment.text;
}

function handleEvent(event) {
  if (event.type === "media") {
    const parts = [event.audioCodec, `${event.sampleRateHz} Hz`, `${event.channels} ch`];
    if (event.hasVideo) parts.push(`+ ${event.videoCodec || "video"}`);
    setStat("stat-media", parts.filter(Boolean).join(" "));
    if (event.durationMs > 0) banner("done", `Media understood: ${formatTime(event.durationMs)} of ${parts.join(", ")}.`);
  } else if (event.type === "partial") {
    upsertSegment(event, false);
  } else if (event.type === "final") {
    upsertSegment(event, true);
    totals.segments += 1;
    setStat("stat-segments", `${totals.segments}`);
  } else if (event.type === "complete") {
    setStat("stat-language", event.language || "—");
    banner("done", `Transcript complete: ${event.segmentCount} segment(s), `
      + `${formatTime(event.durationMs)} of media, language ${event.language || "unknown"}.`);
  } else if (event.type === "grpc-error") {
    banner("error", `gRPC error (code ${event.code}): ${event.message}`);
  } else if (event.type === "done") {
    banner("done", `Done: ${event.segments} final segment(s) in ${(event.elapsedMs / 1000).toFixed(1)}s.`);
  }
}

async function transcribeFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  const query = new URLSearchParams({
    model: document.getElementById("opt-model").value,
    language: document.getElementById("opt-language").value.trim(),
    task: document.getElementById("opt-task").value,
    word_timestamps: document.getElementById("opt-word-timestamps").checked,
  });
  try {
    const response = await fetch(`api/asr/transcribe?${query}`, { method: "POST", body: file });
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
document.getElementById("asr-options").addEventListener("click", (event) => event.stopPropagation());

dropzone.addEventListener("click", () => fileInput.click());
fileInput.addEventListener("change", () => {
  if (fileInput.files.length > 0) transcribeFile(fileInput.files[0]);
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
  if (event.dataTransfer.files.length > 0) transcribeFile(event.dataTransfer.files[0]);
});

fetch("api/asr/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (status.reachable) {
      chip.textContent = status.version
        ? `grpc-asr ${status.version} (${status.backend}) reachable`
        : "service reachable";
      chip.className = "health ok";
      // TranscribeOptions.model is required and must be a loaded model, so
      // the select offers exactly what GetServiceInfo reported, first one
      // preselected. Without a model list the placeholder stays and the
      // server's INVALID_ARGUMENT reads back in the results.
      const select = document.getElementById("opt-model");
      if ((status.models || []).length > 0) {
        select.innerHTML = "";
        for (const model of status.models) {
          const option = document.createElement("option");
          option.value = model;
          option.textContent = model;
          select.appendChild(option);
        }
        select.value = status.models[0];
      }
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
