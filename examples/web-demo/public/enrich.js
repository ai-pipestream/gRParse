// Streams NDJSON enrichment events from the bridge's /api/enrich/annotate
// relay and grows the annotation table as each line lands, grouped by
// annotation type (description / chart / code / formula / skipped): one
// group header per type, one row per ItemAnnotation or ItemSkipped event,
// keyed by the item's self_ref.
"use strict";

const results = document.getElementById("results");
const statsBar = document.getElementById("stats");
const runButton = document.getElementById("run-button");

const totals = { annotations: 0, skipped: 0 };
let startedAt = 0;
let clock = null;
// The table of the current run; rows arrive as the stream does, grouped
// under one header row per annotation kind, in first-seen order.
let tableBody = null;
let groups = null;

function setStat(id, value) {
  document.getElementById(id).textContent = value;
}

function resetRun() {
  results.innerHTML = "";
  totals.annotations = 0;
  totals.skipped = 0;
  statsBar.hidden = false;
  setStat("stat-annotations", "0");
  setStat("stat-skipped", "0");
  setStat("stat-elapsed", "0.0s");
  startedAt = performance.now();
  clock = setInterval(() => {
    setStat("stat-elapsed", `${((performance.now() - startedAt) / 1000).toFixed(1)}s`);
  }, 100);

  const table = document.createElement("table");
  table.className = "warc-table";
  const header = document.createElement("thead");
  header.innerHTML = "<tr><th>item</th><th>model</th><th>result</th></tr>";
  table.appendChild(header);
  tableBody = document.createElement("tbody");
  table.appendChild(tableBody);
  results.appendChild(table);
  groups = new Map();
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  runButton.disabled = false;
}

function banner(kind, message) {
  const element = document.createElement("div");
  element.className = `banner ${kind}`;
  element.textContent = message;
  results.appendChild(element);
}

// Rows group under one header per annotation kind, appended in stream order
// within their group (events across items may interleave out of order; the
// grouping is what keeps the table readable).
function groupRow(kind) {
  let group = groups.get(kind);
  if (!group) {
    const header = document.createElement("tr");
    header.className = "group-row";
    const td = document.createElement("td");
    td.colSpan = 3;
    td.textContent = kind;
    header.appendChild(td);
    tableBody.appendChild(header);
    group = { last: header };
    groups.set(kind, group);
  }
  const row = document.createElement("tr");
  group.last.after(row);
  group.last = row;
  return row;
}

function cell(row, text) {
  const td = document.createElement("td");
  td.textContent = text;
  row.appendChild(td);
  return td;
}

function annotationText(event) {
  if (event.kind === "description") {
    return event.confidence !== undefined
      ? `${event.text} (confidence ${event.confidence.toFixed(2)})`
      : event.text;
  }
  if (event.kind === "chart") {
    const dims = `${event.rows} row(s) x ${event.cols} col(s)`;
    const parts = [event.title, dims, event.csv].filter(Boolean);
    return parts.join(" — ");
  }
  if (event.kind === "code") {
    const language = event.languageRaw || event.language;
    return language && language !== "unspecified" ? `[${language}] ${event.text}` : event.text;
  }
  return event.text || "";
}

function handleEvent(event) {
  if (event.type === "started") {
    const parts = [
      `${event.pictureDescriptions} description(s)`,
      `${event.chartExtractions} chart(s)`,
      `${event.codeEnrichments} code item(s)`,
      `${event.formulaEnrichments} formula(s)`,
    ];
    banner("done", `Enrichment started: ${parts.join(", ")} selected.`);
  } else if (event.type === "annotation") {
    const row = groupRow(event.kind);
    cell(row, event.selfRef);
    cell(row, event.model || "");
    cell(row, annotationText(event));
    totals.annotations += 1;
    setStat("stat-annotations", `${totals.annotations}`);
  } else if (event.type === "skipped") {
    const row = groupRow("skipped");
    row.className = "warn-row";
    cell(row, event.selfRef);
    cell(row, "");
    cell(row, `${event.reason}${event.detail ? `: ${event.detail}` : ""}`);
    totals.skipped += 1;
    setStat("stat-skipped", `${totals.skipped}`);
  } else if (event.type === "complete") {
    const kind = event.failed > 0 || event.skipped > 0 ? "warn" : "done";
    banner(kind, `Enrichment complete: ${event.succeeded} succeeded, `
      + `${event.skipped} skipped, ${event.failed} failed.`);
  } else if (event.type === "grpc-error") {
    banner("error", `gRPC error (code ${event.code}): ${event.message}`);
  } else if (event.type === "done") {
    banner("done", `Done: ${totals.annotations} annotation(s) in ${(event.elapsedMs / 1000).toFixed(1)}s.`);
  }
}

function readImage(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error("could not read the image file"));
    reader.onload = () => {
      // reader.result is a data URI; the base64 payload follows the comma.
      const comma = String(reader.result).indexOf(",");
      resolve({ base64: String(reader.result).slice(comma + 1), mime: file.type || "image/png" });
    };
    reader.readAsDataURL(file);
  });
}

async function runEnrichment() {
  const code = document.getElementById("in-code").value;
  const formula = document.getElementById("in-formula").value;
  const imageInput = document.getElementById("in-image");
  const file = imageInput.files.length > 0 ? imageInput.files[0] : null;
  if (!code.trim() && !formula.trim() && !file) {
    resetRun();
    finishRun();
    banner("error", "Nothing to enrich: paste code or a formula, or choose an image.");
    return;
  }
  resetRun();
  runButton.disabled = true;
  try {
    const body = {
      code,
      formula,
      describe: document.getElementById("opt-describe").checked,
      chart: document.getElementById("opt-chart").checked,
      vlmEndpoint: document.getElementById("opt-endpoint").value.trim(),
    };
    if (file) body.image = await readImage(file);
    const response = await fetch("api/enrich/annotate", {
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

runButton.addEventListener("click", runEnrichment);

fetch("api/enrich/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (!status.reachable) {
      chip.textContent = "service unreachable";
      chip.className = "health bad";
      return;
    }
    // The service is a client of an external VLM server: reachable with no
    // default endpoint means every enrichment will skip with vlm-error
    // unless the request overrides the endpoint, so say so up front.
    if (status.vlmConfigured) {
      chip.textContent = `grpc-enrich ${status.serviceVersion || ""} reachable — VLM: ${status.defaultVlmEndpoint}`.trim();
      chip.className = "health ok";
    } else {
      chip.textContent = "service reachable — no default VLM endpoint configured (set an override below)";
      chip.className = "health warn";
    }
  })
  .catch(() => {
    const chip = document.getElementById("health");
    chip.textContent = "bridge unreachable";
    chip.className = "health bad";
  });
