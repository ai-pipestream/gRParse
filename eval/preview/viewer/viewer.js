// Static page preview: gRParse beside Docling, boxes and labels over each
// side's own page raster. Reads manifest.json and docs/<id>/{ours,docling}.json
// written by eval/preview/build.py; both are docling-core dialect JSON whose
// page images were moved out to docs/<id>/pages/ and referenced by uri.
"use strict";

const SIDES = ["ours", "docling"];
const ARENAS = ["texts", "tables", "pictures", "key_value_items", "form_items"];
const MATCH_IOU = 0.5;
const PALETTE = {
  title: "#9333ea", section_header: "#7c3aed", text: "#2563eb", paragraph: "#2563eb",
  list_item: "#0891b2", caption: "#d97706", footnote: "#a16207", formula: "#db2777",
  code: "#be185d", table: "#16a34a", picture: "#ea580c", chart: "#c2410c",
  page_header: "#78716c", page_footer: "#78716c", reference: "#0d9488",
  document_index: "#4d7c0f", key_value_region: "#65a30d", form: "#65a30d",
  checkbox_selected: "#475569", checkbox_unselected: "#475569", handwritten_text: "#9f1239",
};
const FALLBACK_COLORS = ["#0284c7", "#ca8a04", "#059669", "#e11d48", "#6d28d9", "#57534e"];

const state = { manifest: null, docs: [], visible: [], current: null, page: 1, pages: [], data: {}, hidden: new Set() };
const $ = (selector) => document.querySelector(selector);

function colorFor(label) {
  if (PALETTE[label]) return PALETTE[label];
  let hash = 0;
  for (const ch of label) hash = (hash * 31 + ch.charCodeAt(0)) >>> 0;
  return FALLBACK_COLORS[hash % FALLBACK_COLORS.length];
}

function el(tag, attrs = {}, text) {
  const node = tag === "svg" || tag === "rect" || tag === "text" || tag === "title"
    ? document.createElementNS("http://www.w3.org/2000/svg", tag) : document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  return node;
}

// ---------------------------------------------------------------------------
// Geometry (same rules as build.py)
// ---------------------------------------------------------------------------

function pageEntry(doc, pageNo) {
  return doc && doc.pages ? doc.pages[String(pageNo)] || null : null;
}

function normalizedBox(bbox, size) {
  let top = Number(bbox.t) || 0;
  let bottom = Number(bbox.b) || 0;
  if (String(bbox.coord_origin || "").toUpperCase().endsWith("BOTTOMLEFT")) {
    top = size.height - top;
    bottom = size.height - bottom;
  }
  const xs = [(Number(bbox.l) || 0) / size.width, (Number(bbox.r) || 0) / size.width].sort((a, b) => a - b);
  const ys = [top / size.height, bottom / size.height].sort((a, b) => a - b);
  return { x0: xs[0], y0: ys[0], x1: xs[1], y1: ys[1] };
}

function boxesOnPage(doc, pageNo) {
  const page = pageEntry(doc, pageNo);
  const size = page && page.size ? { width: Number(page.size.width), height: Number(page.size.height) } : null;
  if (!size || !(size.width > 0) || !(size.height > 0)) return [];
  const boxes = [];
  for (const arena of ARENAS) {
    (doc[arena] || []).forEach((item, index) => {
      // One box per item: the union of its provenance boxes on this page
      // (gRParse gives one prov per word or line, Docling one per item).
      let union = null;
      for (const prov of item.prov || []) {
        if (Number(prov.page_no) !== pageNo || !prov.bbox) continue;
        const b = normalizedBox(prov.bbox, size);
        union = union ? { x0: Math.min(union.x0, b.x0), y0: Math.min(union.y0, b.y0),
          x1: Math.max(union.x1, b.x1), y1: Math.max(union.y1, b.y1) } : b;
      }
      if (union) {
        boxes.push({
          label: String(item.label || "unknown"),
          ref: item.self_ref || `#/${arena}/${index}`,
          text: item.text || item.orig || (item.data && item.data.num_rows !== undefined
            ? `table ${item.data.num_rows} x ${item.data.num_cols}` : ""),
          box: union,
          partner: null,
        });
      }
    });
  }
  return boxes;
}

function iou(a, b) {
  const ix = Math.max(0, Math.min(a.x1, b.x1) - Math.max(a.x0, b.x0));
  const iy = Math.max(0, Math.min(a.y1, b.y1) - Math.max(a.y0, b.y0));
  const inter = ix * iy;
  const union = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
  return union > 0 ? inter / union : 0;
}

function match(left, right) {
  const pairs = [];
  left.forEach((a, i) => right.forEach((b, j) => {
    const score = iou(a.box, b.box);
    if (score >= MATCH_IOU) pairs.push([score, i, j]);
  }));
  pairs.sort((p, q) => q[0] - p[0]);
  for (const [, i, j] of pairs) {
    if (left[i].partner || right[j].partner) continue;
    left[i].partner = right[j];
    right[j].partner = left[i];
  }
}

// ---------------------------------------------------------------------------
// Corpus list
// ---------------------------------------------------------------------------

function failed(doc) {
  return SIDES.some((side) => doc[side] && doc[side].error) || Boolean(doc.error);
}

function renderList() {
  const query = $("#filter").value.trim().toLowerCase();
  const sort = $("#sort").value;
  let docs = state.docs.filter((d) => !query || (d.path || d.name).toLowerCase().includes(query));
  const score = (d) => (d.agreement && d.agreement.score !== null && d.agreement.score !== undefined ? d.agreement.score : 2);
  if (sort === "agreement") docs = [...docs].sort((a, b) => score(a) - score(b));
  if (sort === "failures") docs = [...docs].sort((a, b) => Number(failed(b)) - Number(failed(a)));
  if (sort === "slowest") docs = [...docs].sort((a, b) => ((b.ours || {}).elapsed_ms || 0) - ((a.ours || {}).elapsed_ms || 0));
  state.visible = docs;
  const list = $("#doc-list");
  list.replaceChildren();
  for (const doc of docs) {
    const item = el("li", { title: doc.path || doc.name });
    if (state.current && doc.id === state.current.id) item.classList.add("active");
    item.append(el("span", { class: "name" }, doc.path || doc.name));
    const value = doc.agreement && doc.agreement.score !== null && doc.agreement.score !== undefined
      ? `${Math.round(doc.agreement.score * 100)}%` : "";
    item.append(el("span", { class: failed(doc) ? "score fail" : "score" }, failed(doc) ? "fail" : value));
    item.addEventListener("click", () => openDoc(doc, 1));
    list.append(item);
  }
}

function renderCorpusSummary() {
  const docs = state.docs;
  const ok = (side) => docs.filter((d) => d[side] && !d[side].error).length;
  const scored = docs.filter((d) => d.agreement && d.agreement.score !== null && d.agreement.score !== undefined);
  const mean = scored.length ? scored.reduce((s, d) => s + d.agreement.score, 0) / scored.length : null;
  $("#corpus-summary").textContent =
    `${docs.length} documents. gRParse ok ${ok("ours")}, Docling ok ${ok("docling")}.` +
    (mean !== null ? ` Mean box agreement ${Math.round(mean * 100)}% over ${scored.length}.` : "");
}

// ---------------------------------------------------------------------------
// Document and page
// ---------------------------------------------------------------------------

async function loadJson(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  return response.json();
}

async function openDoc(doc, page) {
  state.current = doc;
  state.data = {};
  const failures = [];
  await Promise.all(SIDES.map(async (side) => {
    if (!doc.id || !doc[side] || doc[side].error || doc[side].pages === undefined) return;
    try {
      state.data[side] = await loadJson(`docs/${doc.id}/${side}.json`);
    } catch (error) {
      failures.push(`${side}: ${error.message}`);
    }
  }));
  const pages = new Set();
  for (const side of SIDES) for (const key of Object.keys((state.data[side] || {}).pages || {})) pages.add(Number(key));
  state.pages = [...pages].filter((n) => n > 0).sort((a, b) => a - b);
  $("#doc-title").textContent = doc.path || doc.name;
  const agreement = doc.agreement;
  $("#doc-stats").textContent = [
    agreement ? `boxes: same label ${agreement.same_label}, other label ${agreement.other_label}, gRParse only ${agreement.ours_only}, Docling only ${agreement.docling_only}` : "",
    ...failures,
  ].filter(Boolean).join(" | ");
  for (const side of SIDES) {
    const info = doc[side] || {};
    const pane = document.querySelector(`.pane[data-side="${side}"] .pane-info`);
    const timing = info.elapsed_ms ? `${(info.elapsed_ms / 1000).toFixed(1)} s` : "";
    pane.textContent = [info.status, timing, info.error].filter(Boolean).join(" · ");
  }
  renderList();
  showPage(state.pages.includes(page) ? page : state.pages[0] || 1);
}

function showPage(pageNo) {
  state.page = pageNo;
  const index = state.pages.indexOf(pageNo);
  $("#page-label").textContent = state.pages.length ? `page ${pageNo} of ${state.pages.length}` : "no pages";
  $("#prev-page").disabled = index <= 0;
  $("#next-page").disabled = index < 0 || index >= state.pages.length - 1;
  if (state.current && state.current.id) {
    history.replaceState(null, "", `#doc=${state.current.id}&page=${pageNo}`);
  }
  const boxes = {};
  for (const side of SIDES) boxes[side] = state.data[side] ? boxesOnPage(state.data[side], pageNo) : [];
  match(boxes.ours, boxes.docling);
  const labels = new Set([...boxes.ours, ...boxes.docling].map((b) => b.label));
  renderLegend(labels);
  for (const side of SIDES) {
    const other = side === "ours" ? "docling" : "ours";
    const page = pageEntry(state.data[side], pageNo) || pageEntry(state.data[other], pageNo);
    const size = page && page.size ? page.size : null;
    const aspect = size && Number(size.width) > 0 ? Number(size.height) / Number(size.width) : 11 / 8.5;
    renderPane(side, boxes[side], pageImage(side, pageNo) || pageImage(other, pageNo), aspect);
  }
}

function pageImage(side, pageNo) {
  const page = pageEntry(state.data[side], pageNo);
  const uri = page && page.image && page.image.uri;
  if (!uri) return null;
  return uri.startsWith("data:") || uri.startsWith("http") ? uri : `docs/${state.current.id}/${uri}`;
}

function renderPane(side, boxes, image, aspect) {
  const pane = document.querySelector(`.pane[data-side="${side}"]`);
  const holder = pane.querySelector(".page");
  const img = holder.querySelector("img");
  const svg = holder.querySelector("svg");
  holder.classList.toggle("no-image", !image);
  if (image) img.src = image; else img.removeAttribute("src");
  img.hidden = !image;
  holder.classList.toggle("hide-tags", !$("#show-tags").checked);
  // One SVG unit per thousandth of the page width, so tags keep their shape.
  const height = 1000 * aspect;
  svg.setAttribute("viewBox", `0 0 1000 ${height}`);
  holder.style.aspectRatio = image ? "" : `1000 / ${height}`;
  svg.replaceChildren();
  const onlyUnmatched = $("#show-unmatched").checked;
  for (const entry of boxes) {
    if (state.hidden.has(entry.label)) continue;
    if (onlyUnmatched && entry.partner && entry.partner.label === entry.label) continue;
    const { x0, y0, x1, y1 } = entry.box;
    const color = colorFor(entry.label);
    const rect = el("rect", {
      x: x0 * 1000, y: y0 * height, width: Math.max(1, (x1 - x0) * 1000), height: Math.max(1, (y1 - y0) * height),
      class: entry.partner ? "box" : "box unmatched", stroke: color, fill: color,
    });
    entry.node = rect;
    rect.addEventListener("mouseenter", (event) => hover(entry, event, true));
    rect.addEventListener("mousemove", moveTooltip);
    rect.addEventListener("mouseleave", (event) => hover(entry, event, false));
    svg.append(rect);
    svg.append(el("text", { x: x0 * 1000 + 2, y: Math.max(14, y0 * height - 3), class: "tag", fill: color }, entry.label));
  }
  renderCounts(pane.querySelector(".counts"), boxes);
}

function renderCounts(table, boxes) {
  const counts = new Map();
  for (const entry of boxes) {
    const row = counts.get(entry.label) || { all: 0, matched: 0 };
    row.all += 1;
    if (entry.partner && entry.partner.label === entry.label) row.matched += 1;
    counts.set(entry.label, row);
  }
  table.replaceChildren();
  const head = el("tr");
  for (const title of ["label", "boxes", "same label on other side"]) head.append(el("th", {}, title));
  table.append(head);
  for (const [label, row] of [...counts.entries()].sort()) {
    const tr = el("tr");
    const name = el("td");
    name.append(el("span", { class: "swatch", style: `background:${colorFor(label)}` }), ` ${label}`);
    tr.append(name, el("td", {}, String(row.all)), el("td", {}, String(row.matched)));
    table.append(tr);
  }
}

function renderLegend(labels) {
  const legend = $("#legend");
  legend.replaceChildren();
  for (const label of [...labels].sort()) {
    const toggle = el("label");
    const box = el("input", { type: "checkbox" });
    box.checked = !state.hidden.has(label);
    box.addEventListener("change", () => {
      if (box.checked) state.hidden.delete(label); else state.hidden.add(label);
      showPage(state.page);
    });
    toggle.append(box, el("span", { class: "swatch", style: `background:${colorFor(label)}` }), label);
    legend.append(toggle);
  }
}

function hover(entry, event, on) {
  for (const node of [entry.node, entry.partner && entry.partner.node]) if (node) node.classList.toggle("hot", on);
  const tooltip = $("#tooltip");
  if (!on) { tooltip.hidden = true; return; }
  const partner = entry.partner
    ? `\nmatched: ${entry.partner.label} (IoU ${iou(entry.box, entry.partner.box).toFixed(2)})`
    : "\nno match on the other side";
  const text = entry.text ? `\n\n${String(entry.text).slice(0, 400)}` : "";
  tooltip.textContent = `${entry.label}  ${entry.ref}${partner}${text}`;
  tooltip.hidden = false;
  moveTooltip(event);
}

function moveTooltip(event) {
  const tooltip = $("#tooltip");
  const x = Math.min(event.clientX + 14, window.innerWidth - tooltip.offsetWidth - 8);
  const y = Math.min(event.clientY + 14, window.innerHeight - tooltip.offsetHeight - 8);
  tooltip.style.left = `${Math.max(8, x)}px`;
  tooltip.style.top = `${Math.max(8, y)}px`;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

function stepPage(delta) {
  const index = state.pages.indexOf(state.page) + delta;
  if (index >= 0 && index < state.pages.length) showPage(state.pages[index]);
}

function stepDoc(delta) {
  const list = state.visible;
  const index = list.findIndex((d) => state.current && d.id === state.current.id) + delta;
  if (index >= 0 && index < list.length) openDoc(list[index], 1);
}

function wire() {
  $("#filter").addEventListener("input", renderList);
  $("#sort").addEventListener("change", renderList);
  $("#prev-page").addEventListener("click", () => stepPage(-1));
  $("#next-page").addEventListener("click", () => stepPage(1));
  $("#prev-doc").addEventListener("click", () => stepDoc(-1));
  $("#next-doc").addEventListener("click", () => stepDoc(1));
  $("#show-tags").addEventListener("change", () => showPage(state.page));
  $("#show-unmatched").addEventListener("change", () => showPage(state.page));
  document.addEventListener("keydown", (event) => {
    if (event.target.matches("input[type=search]")) return;
    if (event.key === "j" || event.key === "ArrowRight") stepPage(1);
    if (event.key === "k" || event.key === "ArrowLeft") stepPage(-1);
    if (event.key === "n") stepDoc(1);
    if (event.key === "p") stepDoc(-1);
  });
}

async function main() {
  wire();
  try {
    state.manifest = await loadJson("manifest.json");
  } catch (error) {
    $("#doc-title").textContent = `Could not load manifest.json (${error.message}). Serve this folder over HTTP.`;
    return;
  }
  state.docs = state.manifest.documents || [];
  const run = state.manifest.run || {};
  $("#run-info").textContent = [run.grparse_service, run.docling ? `Docling: ${run.docling}` : "", run.started]
    .filter(Boolean).join(" · ");
  renderCorpusSummary();
  renderList();
  const params = new URLSearchParams(location.hash.slice(1));
  const wanted = params.get("doc") ? state.docs.find((d) => d.id === params.get("doc")) : null;
  const first = wanted || state.visible.find((d) => d.id);
  if (first) openDoc(first, Number(params.get("page")) || 1);
}

main();
