// Document tab: renders the complete merged document from the bridge's
// /api/document/parse relay. While the parse runs the stream carries
// page-progress lines; the final line carries the whole document as
// protobuf-JSON (camelCase fields, enum value names as strings). The page
// then resolves the body tree against the item arenas and renders one card
// per page: the page image with per-item provenance boxes on the left, the
// content in reading order on the right, with hover/click sync between the
// two panes. Items without provenance (merged in from other collectors)
// land in a trailing unpaged section grouped by collector.
"use strict";

const dropzone = document.getElementById("dropzone");
const fileInput = document.getElementById("file-input");
const sampleButton = document.getElementById("sample-button");
const results = document.getElementById("results");
const progressBar = document.getElementById("progress");
const progressPages = document.getElementById("progress-pages");
const progressElapsed = document.getElementById("progress-elapsed");
const infoBar = document.getElementById("doc-info");
const toolbar = document.getElementById("doc-toolbar");
const legendBar = document.getElementById("doc-legend");
const legendEntries = document.getElementById("legend-entries");
const legendCollectors = document.getElementById("legend-collectors");
const furnitureToggle = document.getElementById("toggle-furniture");
const readingOrderToggle = document.getElementById("toggle-reading-order");
const confidenceToggle = document.getElementById("toggle-confidence");
const confidenceLegend = document.getElementById("conf-legend");

const SVG_NS = "http://www.w3.org/2000/svg";

// ---------------------------------------------------------------------------
// Fixed label palette for the provenance boxes (border solid, fill ~25%).
// ---------------------------------------------------------------------------

const PALETTE = {
  title: { border: "#ff5d7d", fill: "rgba(255, 93, 125, 0.25)" },
  section_header: { border: "#f47eb0", fill: "rgba(244, 126, 176, 0.25)" },
  text: { border: "#d4b93c", fill: "rgba(233, 213, 110, 0.24)" },
  paragraph: { border: "#d4b93c", fill: "rgba(233, 213, 110, 0.24)" },
  table: { border: "#f0883e", fill: "rgba(240, 136, 62, 0.25)" },
  document_index: { border: "#f0883e", fill: "rgba(240, 136, 62, 0.25)" },
  picture: { border: "#ffab70", fill: "rgba(255, 171, 112, 0.25)" },
  chart: { border: "#ffab70", fill: "rgba(255, 171, 112, 0.25)" },
  list_item: { border: "#7ee787", fill: "rgba(126, 231, 135, 0.22)" },
  code: { border: "#8b949e", fill: "rgba(139, 148, 158, 0.25)" },
  formula: { border: "#d2a8ff", fill: "rgba(210, 168, 255, 0.25)" },
  caption: { border: "#79c0ff", fill: "rgba(121, 192, 255, 0.25)" },
  footnote: { border: "#79c0ff", fill: "rgba(121, 192, 255, 0.22)" },
  key_value_region: { border: "#39c5cf", fill: "rgba(57, 197, 207, 0.25)" },
};
const DEFAULT_COLOR = { border: "#6e7681", fill: "rgba(110, 118, 129, 0.2)" };

function colorFor(label) {
  return PALETTE[label] || DEFAULT_COLOR;
}

// Heatmap ramp for collector confidence: red at 0, yellow at 0.7, green at 1.
const CONFIDENCE_STOPS = [
  [0, [248, 81, 73]],
  [0.7, [233, 196, 72]],
  [1, [63, 185, 80]],
];

function confidenceColor(value) {
  const level = Math.min(1, Math.max(0, value));
  let lower = CONFIDENCE_STOPS[0];
  let upper = CONFIDENCE_STOPS[CONFIDENCE_STOPS.length - 1];
  for (let index = 1; index < CONFIDENCE_STOPS.length; index += 1) {
    if (level <= CONFIDENCE_STOPS[index][0]) {
      lower = CONFIDENCE_STOPS[index - 1];
      upper = CONFIDENCE_STOPS[index];
      break;
    }
  }
  const span = upper[0] - lower[0];
  const t = span > 0 ? (level - lower[0]) / span : 0;
  const channel = (index) => Math.round(lower[1][index] + (upper[1][index] - lower[1][index]) * t);
  const [r, g, b] = [channel(0), channel(1), channel(2)];
  return { border: `rgb(${r}, ${g}, ${b})`, fill: `rgba(${r}, ${g}, ${b}, 0.28)` };
}

// ---------------------------------------------------------------------------
// Wire-shape helpers (protobuf-JSON: camelCase fields, enum name strings).
// ---------------------------------------------------------------------------

// The BaseTextItem oneof: exactly one of these is set per entry. The decoded
// object usually carries the virtual `item` discriminator; probing the keys
// keeps this robust when it does not.
const TEXT_VARIANTS = ["title", "sectionHeader", "listItem", "code", "formula", "text", "fieldHeading", "fieldValue"];

function unwrapText(baseText) {
  if (!baseText || typeof baseText !== "object") return null;
  let key = typeof baseText.item === "string" && baseText[baseText.item] ? baseText.item : null;
  if (!key) key = TEXT_VARIANTS.find((name) => baseText[name] && typeof baseText[name] === "object") || null;
  if (!key) return null;
  const node = baseText[key];
  // Every variant wraps the base fields as `base` except code, which inlines
  // them directly (single meta slot on the wire).
  const base = key === "code" ? node : node.base;
  if (!base || typeof base !== "object") return null;
  return { variant: key, node, base };
}

function stripEnum(value, prefix, fallback) {
  return typeof value === "string" && value.startsWith(prefix)
    ? value.slice(prefix.length).toLowerCase()
    : fallback;
}

function shortLabel(label, fallback) {
  const stripped = stripEnum(label, "DOC_ITEM_LABEL_", fallback);
  return !stripped || stripped === "unspecified" ? fallback : stripped;
}

function layerOf(contentLayer) {
  return stripEnum(contentLayer, "CONTENT_LAYER_", "unspecified") || "unspecified";
}

// Body and unspecified render normally; furniture, notes, background and
// invisible layers hide behind the "show furniture / notes" toggle.
function inBodyLayer(record) {
  return record.layer === "body" || record.layer === "unspecified";
}

function refString(refItem) {
  return refItem && typeof refItem.ref === "string" && refItem.ref ? refItem.ref : null;
}

function collectorOf(record) {
  for (const source of record.base.source || []) {
    if (source && source.collector && source.collector.collector) return source.collector.collector;
  }
  return null;
}

function collectorConfidence(record) {
  for (const source of record.base.source || []) {
    if (source && source.collector && typeof source.collector.confidence === "number") {
      return source.collector.confidence;
    }
  }
  return null;
}

function safeHref(href) {
  return typeof href === "string" && /^(https?:|mailto:|#)/i.test(href.trim()) ? href.trim() : null;
}

function isSafeImage(uri) {
  return typeof uri === "string" && (/^data:image\//i.test(uri) || /^https?:/i.test(uri));
}

// google.protobuf.Value / Struct decoded objects reduce to plain JSON for
// the metadata drawers.
function valueToPlain(value) {
  if (!value || typeof value !== "object") return value;
  if (typeof value.kind === "string" && value[value.kind] !== undefined) return valueToPlain(value[value.kind]);
  if (value.stringValue !== undefined && value.stringValue !== null) return value.stringValue;
  if (value.numberValue !== undefined && value.numberValue !== null) return value.numberValue;
  if (value.boolValue !== undefined && value.boolValue !== null) return value.boolValue;
  if (value.structValue) return structToPlain(value.structValue);
  if (value.listValue) return (value.listValue.values || []).map(valueToPlain);
  if (value.nullValue !== undefined) return null;
  return value;
}

function structToPlain(struct) {
  const out = {};
  for (const [key, field] of Object.entries((struct && struct.fields) || {})) out[key] = valueToPlain(field);
  return out;
}

// Drops empty strings, empty containers, nulls, and *_UNSPECIFIED enum
// names so the metadata drawer shows only fields that carry information.
function prune(value) {
  if (Array.isArray(value)) {
    const arr = value.map(prune).filter((entry) => entry !== undefined);
    return arr.length > 0 ? arr : undefined;
  }
  if (value && typeof value === "object") {
    const out = {};
    for (const [key, entry] of Object.entries(value)) {
      // Underscore-prefixed keys are decoder bookkeeping for optional
      // fields, not document data.
      if (key.startsWith("_")) continue;
      const pruned = prune(entry);
      if (pruned !== undefined) out[key] = pruned;
    }
    return Object.keys(out).length > 0 ? out : undefined;
  }
  if (value === null || value === undefined || value === "") return undefined;
  if (typeof value === "string" && value.endsWith("_UNSPECIFIED")) return undefined;
  return value;
}

// ---------------------------------------------------------------------------
// Document model: every arena item indexed by its JSON-pointer ref.
// ---------------------------------------------------------------------------

// Pointer segment (wire spelling) -> [document key (camelCase), item kind].
const ARENAS = [
  ["texts", "texts", "text"],
  ["tables", "tables", "table"],
  ["pictures", "pictures", "picture"],
  ["groups", "groups", "group"],
  ["key_value_items", "keyValueItems", "keyValue"],
  ["form_items", "formItems", "form"],
  ["field_regions", "fieldRegions", "fieldRegion"],
  ["field_items", "fieldItems", "fieldItem"],
];

const KIND_FALLBACK_LABEL = {
  text: "text",
  table: "table",
  picture: "picture",
  keyValue: "key_value_region",
  form: "form",
  fieldRegion: "field_region",
  fieldItem: "field_item",
};

const VARIANT_FALLBACK_LABEL = {
  title: "title",
  sectionHeader: "section_header",
  listItem: "list_item",
  code: "code",
  formula: "formula",
  text: "text",
  fieldHeading: "field_heading",
  fieldValue: "field_value",
};

let model = null;

function normalizeProv(base) {
  const entries = [];
  (base.prov || []).forEach((prov, index) => {
    if (!prov) return;
    const pageNo = Number(prov.pageNo) || 0;
    // `index` is the position in the item's own prov array, which is the key
    // the charspan table below is built against.
    if (pageNo > 0) entries.push({ pageNo, bbox: prov.bbox || null, index });
  });
  return entries;
}

// Per-provenance character ranges, relative to the item's own text.
//
// Producers disagree on the charspan base: some count from the start of the
// item, others from the start of the document. Every prov entry of one item
// shares the same base, so the smallest start in the item is that base and
// subtracting it lands both conventions on item-relative offsets. Ranges that
// clamp to nothing, or that cover the whole item (nothing to narrow to), are
// dropped here so hovering never has to decide.
function computeSpans(base, text) {
  const provs = base.prov || [];
  let origin = null;
  for (const prov of provs) {
    const start = prov && prov.charspan ? Number(prov.charspan.start) : NaN;
    if (!Number.isFinite(start)) continue;
    origin = origin === null ? start : Math.min(origin, start);
  }
  if (origin === null || text.length === 0) return null;
  const spans = new Map();
  provs.forEach((prov, index) => {
    if (!prov || !prov.charspan) return;
    let start = Number(prov.charspan.start) - origin;
    let end = Number(prov.charspan.end) - origin;
    if (!Number.isFinite(start) || !Number.isFinite(end)) return;
    if (end < start) [start, end] = [end, start];
    start = Math.min(Math.max(start, 0), text.length);
    end = Math.min(Math.max(end, 0), text.length);
    if (end <= start) return;
    if (start === 0 && end >= text.length) return;
    spans.set(index, { start, end });
  });
  return spans.size > 0 ? spans : null;
}

// Depth-first order of the body tree, which is the document's reading order.
// Every rendered item gets its position stamped on it once so later passes
// (reading-order arrows, navigation) never have to walk the tree again.
function computeWalkOrder(doc, built) {
  const order = [];
  const seen = new Set();
  const visit = (refs) => {
    for (const refItem of refs || []) {
      const ref = refString(refItem);
      if (!ref || seen.has(ref) || ref === "#/body" || ref === "#/furniture") continue;
      seen.add(ref);
      const record = built.byRef.get(ref);
      if (!record) continue;
      if (record.kind !== "group") {
        record.walkIndex = order.length;
        order.push(record);
      }
      visit(record.base.children);
    }
  };
  visit(rootChildren(doc));
  return order;
}

// Cell rectangles for a table, keyed by the anchor position the rendered
// cell carries, so a hovered td can find its own box. Cells without a bbox
// (the producer never located them) simply have no entry.
function tableCellBoxes(data) {
  if (!data || typeof data !== "object") return [];
  const boxes = [];
  const seen = new Set();
  const push = (cell) => {
    if (!cell || !cell.bbox) return;
    const pos = `${Number(cell.startRowOffsetIdx) || 0}:${Number(cell.startColOffsetIdx) || 0}`;
    if (seen.has(pos)) return;
    seen.add(pos);
    boxes.push({ pos, bbox: cell.bbox });
  };
  for (const cell of data.tableCells || []) push(cell);
  for (const row of data.grid || []) {
    const cells = row && Array.isArray(row.cells) ? row.cells : (Array.isArray(row) ? row : []);
    for (const cell of cells) push(cell);
  }
  return boxes;
}

function indexDocument(doc) {
  const built = {
    doc,
    items: [],
    byRef: new Map(),
    labelCounts: new Map(),
    collectorCounts: new Map(),
    unpagedCollectors: [],
    walkOrder: [],
    pageSequences: new Map(),
    // ref -> rendered elements, filled in as cards build. Hover and click
    // sync read this map instead of scanning the DOM.
    view: new Map(),
  };
  for (const [segment, docKey, kind] of ARENAS) {
    const arena = Array.isArray(doc[docKey]) ? doc[docKey] : [];
    arena.forEach((raw, index) => {
      let variant = null;
      let node = raw;
      let base = raw;
      if (kind === "text") {
        const unwrapped = unwrapText(raw);
        if (!unwrapped) return;
        ({ variant, node, base } = unwrapped);
      }
      if (!base || typeof base !== "object") return;
      const label = kind === "group"
        ? "group"
        : shortLabel(base.label, variant ? VARIANT_FALLBACK_LABEL[variant] : KIND_FALLBACK_LABEL[kind]);
      const prov = kind === "group" ? [] : normalizeProv(base);
      const text = base.text || base.orig || "";
      const record = {
        ref: `#/${segment}/${index}`,
        kind,
        variant,
        raw: node,
        base,
        label,
        prov,
        text,
        spans: kind === "group" ? null : computeSpans(base, text),
        confidence: null,
        cellBoxes: kind === "table" ? tableCellBoxes(node.data) : null,
        page: prov.length > 0 ? prov[0].pageNo : null,
        layer: layerOf(base.contentLayer),
      };
      record.confidence = collectorConfidence(record);
      // Items merged in without a collector source belong to the base
      // document; the filter lists them under that name.
      record.collector = kind === "group" ? null : (collectorOf(record) || "base");
      if (record.collector) {
        built.collectorCounts.set(record.collector, (built.collectorCounts.get(record.collector) || 0) + 1);
      }
      built.items.push(record);
      built.byRef.set(record.ref, record);
      if (typeof base.selfRef === "string" && base.selfRef && base.selfRef !== record.ref) {
        built.byRef.set(base.selfRef, record);
      }
      if (kind !== "group" && prov.length > 0) {
        built.labelCounts.set(label, (built.labelCounts.get(label) || 0) + prov.length);
      }
    });
  }
  built.walkOrder = computeWalkOrder(doc, built);
  // Collector names of unpaged items, in arena order, for the trailing
  // section's bucket order.
  const seenCollectors = new Set();
  for (const record of built.items) {
    if (record.kind === "group" || record.page !== null) continue;
    const name = collectorOf(record) || "unattributed";
    if (!seenCollectors.has(name)) {
      seenCollectors.add(name);
      built.unpagedCollectors.push(name);
    }
  }
  return built;
}

function rootChildren(doc) {
  const children = [];
  if (doc.body && Array.isArray(doc.body.children)) children.push(...doc.body.children);
  return children;
}

function furnitureChildren(doc) {
  return doc.furniture && Array.isArray(doc.furniture.children) ? doc.furniture.children : [];
}

function pageNumbers(doc) {
  const numbers = new Set();
  for (const key of Object.keys(doc.pages || {})) {
    const parsed = Number(key);
    if (Number.isFinite(parsed) && parsed > 0) numbers.add(parsed);
  }
  for (const record of model.items) {
    for (const prov of record.prov) numbers.add(prov.pageNo);
  }
  return [...numbers].sort((a, b) => a - b);
}

function pageInfo(pageNo) {
  const pages = model.doc.pages || {};
  return pages[pageNo] || pages[String(pageNo)] || null;
}

function pageSize(pageNo) {
  const page = pageInfo(pageNo);
  if (page && page.size && Number(page.size.width) > 0 && Number(page.size.height) > 0) {
    return { width: Number(page.size.width), height: Number(page.size.height) };
  }
  // No advertised size: measure the boxes so overlays still land sensibly.
  let width = 0;
  let height = 0;
  for (const record of model.items) {
    for (const prov of record.prov) {
      if (prov.pageNo !== pageNo || !prov.bbox) continue;
      width = Math.max(width, Number(prov.bbox.r) || 0);
      height = Math.max(height, Number(prov.bbox.b) || 0, Number(prov.bbox.t) || 0);
    }
  }
  return { width: width || 850, height: height || 1100 };
}

// ---------------------------------------------------------------------------
// Small DOM helpers. All document content goes through textContent.
// ---------------------------------------------------------------------------

function el(tag, className) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  return node;
}

function badge(text, className) {
  const span = el("span", className || "dv-badge");
  span.textContent = text;
  return span;
}

function banner(kind, message) {
  const element = el("div", `banner ${kind}`);
  element.textContent = message;
  results.appendChild(element);
}

// ---------------------------------------------------------------------------
// Content rendering (right pane + unpaged section).
// ---------------------------------------------------------------------------

function viewEntryFor(ref) {
  let entry = model.view.get(ref);
  if (!entry) {
    entry = { boxes: [], contents: [] };
    model.view.set(ref, entry);
  }
  return entry;
}

function decorate(element, record) {
  element.classList.add("dv-item");
  element.dataset.ref = record.ref;
  element.dataset.label = record.label;
  if (record.collector) element.dataset.collector = record.collector;
  if (!inBodyLayer(record)) element.classList.add("dv-furn");
  // Cards build lazily, so an item arriving after a filter change starts out
  // in the state that filter left it in.
  if (labelHidden(record)) element.classList.add("label-off");
  if (collectorHidden(record)) element.classList.add("collector-off");
  viewEntryFor(record.ref).contents.push(element);
}

function metaDrawer(record) {
  const meta = record.base.meta;
  if (!meta || typeof meta !== "object") return null;
  const plain = { ...meta };
  if (meta.customFields && typeof meta.customFields === "object") {
    const custom = {};
    for (const [key, value] of Object.entries(meta.customFields)) custom[key] = valueToPlain(value);
    plain.customFields = custom;
  }
  const pruned = prune(plain);
  if (pruned === undefined) return null;
  const details = el("details", "dv-meta");
  const summary = el("summary");
  summary.textContent = "metadata";
  const pre = el("pre");
  pre.textContent = JSON.stringify(pruned, null, 2);
  details.append(summary, pre);
  return details;
}

// Appends the item element plus its optional metadata drawer. Elements that
// cannot legally contain a <details> get wrapped instead.
function attach(container, main, record, ctx) {
  decorate(main, record);
  if (ctx.showConfidence) {
    const confidence = collectorConfidence(record);
    if (confidence !== null) main.appendChild(badge(`${Math.round(confidence * 100)}%`, "dv-conf"));
  }
  const drawer = metaDrawer(record);
  if (!drawer) {
    container.appendChild(main);
    return;
  }
  if (/^(LI|FIGURE|DIV|SECTION|DL)$/.test(main.tagName)) {
    main.appendChild(drawer);
    container.appendChild(main);
    return;
  }
  const wrap = el("div", "dv-item-block");
  if (main.classList.contains("dv-furn")) wrap.classList.add("dv-furn");
  if (main.classList.contains("label-off")) wrap.classList.add("label-off");
  if (main.classList.contains("collector-off")) wrap.classList.add("collector-off");
  wrap.append(main, drawer);
  container.appendChild(wrap);
}

function applyTextInto(element, record) {
  let text = record.base.text || record.base.orig || "";
  if (record.label === "checkbox_selected") text = `☑ ${text}`;
  if (record.label === "checkbox_unselected") text = `☐ ${text}`;
  const href = safeHref(record.base.hyperlink);
  if (href) {
    const anchor = el("a");
    anchor.href = href;
    anchor.target = "_blank";
    anchor.rel = "noopener";
    anchor.textContent = text;
    element.appendChild(anchor);
  } else {
    element.textContent = text;
  }
  const formatting = record.base.formatting;
  if (formatting) {
    if (formatting.bold) element.classList.add("dv-b");
    if (formatting.italic) element.classList.add("dv-i");
    if (formatting.underline) element.classList.add("dv-u");
    if (formatting.strikethrough) element.classList.add("dv-strike");
  }
  return element;
}

function buildHeading(record) {
  if (record.variant === "title") return applyTextInto(el("h1", "dv-title"), record);
  const level = Math.max(1, Number(record.raw.level) || 1);
  const tag = `h${Math.min(6, level + 1)}`;
  return applyTextInto(el(tag, "dv-heading"), record);
}

function buildCode(record) {
  const block = el("div", "dv-code-block");
  const language = record.raw.codeLanguageRaw
    || stripEnum(record.raw.codeLanguage, "CODE_LANGUAGE_LABEL_", "");
  if (language && language !== "unspecified" && language !== "unknown") {
    block.appendChild(badge(language, "dv-code-lang"));
  }
  const pre = el("pre");
  const code = el("code");
  code.textContent = record.base.text || record.base.orig || "";
  pre.appendChild(code);
  block.appendChild(pre);
  return block;
}

// Resolves the item's captions[] refs to their text items. Each rendered
// caption is consumed so the tree walk does not render it a second time,
// and keeps its own ref so hover still pairs it with its provenance box.
function buildCaption(record, ctx) {
  const parts = [];
  for (const refItem of record.raw.captions || []) {
    const ref = refString(refItem);
    if (!ref) continue;
    const caption = model.byRef.get(ref);
    if (!caption) continue;
    ctx.consumed.add(caption.ref);
    parts.push(caption);
  }
  if (parts.length === 0) return null;
  const figcaption = el("figcaption", "dv-figcaption");
  for (const caption of parts) {
    const span = el("span");
    applyTextInto(span, caption);
    decorate(span, caption);
    figcaption.appendChild(span);
  }
  return figcaption;
}

function buildGridFromCells(data) {
  const numRows = Math.max(0, Number(data.numRows) || 0);
  const numCols = Math.max(0, Number(data.numCols) || 0);
  if (numRows === 0 || numCols === 0) return [];
  const grid = Array.from({ length: numRows }, () => new Array(numCols).fill(null));
  for (const cell of data.tableCells || []) {
    if (!cell) continue;
    const startRow = Math.max(0, Number(cell.startRowOffsetIdx) || 0);
    const endRow = Math.min(numRows, Math.max(startRow + 1, Number(cell.endRowOffsetIdx) || 0));
    const startCol = Math.max(0, Number(cell.startColOffsetIdx) || 0);
    const endCol = Math.min(numCols, Math.max(startCol + 1, Number(cell.endColOffsetIdx) || 0));
    for (let row = startRow; row < endRow && row < numRows; row += 1) {
      for (let col = startCol; col < endCol && col < numCols; col += 1) {
        if (!grid[row][col]) grid[row][col] = cell;
      }
    }
  }
  return grid;
}

function buildTable(record, ctx) {
  const wrap = el("figure", "dv-table-wrap");
  const data = record.raw.data || {};
  let grid = Array.isArray(data.grid) && data.grid.length > 0
    ? data.grid.map((row) => (row && Array.isArray(row.cells) ? row.cells : []))
    : buildGridFromCells(data);
  const table = el("table", "dv-table");
  let emitted = 0;
  grid.forEach((row, rowIndex) => {
    const tr = el("tr");
    row.forEach((cell, colIndex) => {
      if (!cell) return;
      const startRow = Number(cell.startRowOffsetIdx) || 0;
      const startCol = Number(cell.startColOffsetIdx) || 0;
      // The grid repeats a spanning cell at every position it covers; emit
      // it only at its anchor.
      if (startRow !== rowIndex || startCol !== colIndex) return;
      const header = Boolean(cell.columnHeader || cell.rowHeader || cell.rowSection);
      const td = el(header ? "th" : "td");
      const rowSpan = Number(cell.rowSpan) || 0;
      const colSpan = Number(cell.colSpan) || 0;
      if (rowSpan > 1) td.rowSpan = rowSpan;
      if (colSpan > 1) td.colSpan = colSpan;
      if (cell.rowSection) td.classList.add("dv-row-section");
      // The anchor position pairs this cell with its rectangle on the page.
      td.dataset.cellpos = `${startRow}:${startCol}`;
      td.textContent = cell.text || "";
      tr.appendChild(td);
      emitted += 1;
    });
    if (tr.childNodes.length > 0) table.appendChild(tr);
  });
  // A grid whose offsets do not match its positions (defensive): render the
  // rows verbatim rather than dropping everything.
  if (emitted === 0 && grid.some((row) => row.some(Boolean))) {
    table.textContent = "";
    for (const row of grid) {
      const tr = el("tr");
      for (const cell of row) {
        if (!cell) continue;
        const td = el(cell.columnHeader || cell.rowHeader ? "th" : "td");
        td.dataset.cellpos = `${Number(cell.startRowOffsetIdx) || 0}:${Number(cell.startColOffsetIdx) || 0}`;
        td.textContent = cell.text || "";
        tr.appendChild(td);
      }
      if (tr.childNodes.length > 0) table.appendChild(tr);
    }
  }
  if (table.childNodes.length === 0) {
    const empty = el("div", "dv-empty-note");
    empty.textContent = "table with no cells";
    wrap.appendChild(empty);
  } else {
    const scroller = el("div", "dv-table-scroll");
    scroller.appendChild(table);
    wrap.appendChild(scroller);
  }
  const caption = buildCaption(record, ctx);
  if (caption) wrap.appendChild(caption);
  return wrap;
}

function buildPicture(record, ctx) {
  const figure = el("figure", "dv-figure");
  const uri = record.raw.image && record.raw.image.uri;
  if (isSafeImage(uri)) {
    const img = el("img", "dv-figure-img");
    img.src = uri;
    img.alt = record.label;
    img.loading = "lazy";
    figure.appendChild(img);
  } else {
    const placeholder = el("div", "dv-figure-missing");
    placeholder.textContent = record.label === "chart" ? "chart (no image payload)" : "picture (no image payload)";
    figure.appendChild(placeholder);
  }
  const badges = el("div", "dv-figure-badges");
  for (const annotation of record.raw.annotations || []) {
    if (!annotation) continue;
    if (annotation.classification && Array.isArray(annotation.classification.predictedClasses)) {
      const top = annotation.classification.predictedClasses[0];
      if (top && top.className) {
        badges.appendChild(badge(`${top.className} ${Math.round((top.confidence || 0) * 100)}%`));
      }
    }
    if (annotation.misc && annotation.misc.kind === "barcode") {
      const content = structToPlain(annotation.misc.content);
      if (content.value) badges.appendChild(badge(`${content.format || "barcode"}: ${content.value}`));
    }
    if (annotation.description && annotation.description.text) {
      const description = el("div", "dv-figure-desc");
      description.textContent = annotation.description.text;
      badges.appendChild(description);
    }
  }
  if (badges.childNodes.length > 0) figure.appendChild(badges);
  const caption = buildCaption(record, ctx);
  if (caption) figure.appendChild(caption);
  return figure;
}

function buildGraph(record) {
  const container = el("div", "dv-kv-wrap");
  const graph = record.raw.graph || {};
  const cells = Array.isArray(graph.cells) ? graph.cells : [];
  const links = Array.isArray(graph.links) ? graph.links : [];
  const byId = new Map(cells.map((cell) => [Number(cell.cellId) || 0, cell]));
  const linked = new Set();
  const dl = el("dl", "dv-kv");
  for (const link of links) {
    if (!link || link.label !== "GRAPH_LINK_LABEL_TO_VALUE") continue;
    const key = byId.get(Number(link.sourceCellId) || 0);
    const value = byId.get(Number(link.targetCellId) || 0);
    if (!key || !value) continue;
    const dt = el("dt");
    dt.textContent = key.text || "";
    const dd = el("dd");
    dd.textContent = value.text || "";
    dl.append(dt, dd);
    linked.add(Number(key.cellId) || 0);
    linked.add(Number(value.cellId) || 0);
  }
  for (const cell of cells) {
    if (linked.has(Number(cell.cellId) || 0) || !cell.text) continue;
    const dd = el("dd", "dv-kv-lone");
    dd.textContent = cell.text;
    dl.appendChild(dd);
  }
  if (dl.childNodes.length > 0) container.appendChild(dl);
  else {
    const empty = el("div", "dv-empty-note");
    empty.textContent = `${record.label} (no cells)`;
    container.appendChild(empty);
  }
  return container;
}

function buildTextItem(record) {
  switch (record.variant) {
    case "title":
    case "sectionHeader":
      return buildHeading(record);
    case "fieldHeading":
      return applyTextInto(el("h6", "dv-field-heading"), record);
    case "listItem":
      return applyTextInto(el("li", "dv-list-item"), record);
    case "code":
      return buildCode(record);
    case "formula":
      return applyTextInto(el("div", "dv-formula"), record);
    case "fieldValue": {
      const element = applyTextInto(el("div", "dv-field-value"), record);
      if (record.raw.kind) element.appendChild(badge(record.raw.kind, "dv-kind-badge"));
      return element;
    }
    default: {
      // Generic TextItem entries still carry the semantic label; producers
      // are free to encode a title as either TitleItem or TextItem+TITLE.
      if (record.label === "title") return applyTextInto(el("h1", "dv-title"), record);
      if (record.label === "section_header") return applyTextInto(el("h2", "dv-heading"), record);
      if (record.label === "list_item") return applyTextInto(el("div", "dv-li"), record);
      if (record.label === "caption") return applyTextInto(el("div", "dv-caption"), record);
      if (record.label === "footnote") return applyTextInto(el("div", "dv-footnote"), record);
      if (record.label === "reference") return applyTextInto(el("div", "dv-reference"), record);
      if (record.label === "page_header" || record.label === "page_footer") {
        return applyTextInto(el("div", "dv-page-furniture"), record);
      }
      return applyTextInto(el("p", "dv-paragraph"), record);
    }
  }
}

function buildItemElement(record, ctx) {
  switch (record.kind) {
    case "text":
      return buildTextItem(record);
    case "table":
      return buildTable(record, ctx);
    case "picture":
      return buildPicture(record, ctx);
    case "keyValue":
    case "form":
      return buildGraph(record);
    case "fieldRegion":
    case "fieldItem":
      return el("div", "dv-field-region");
    default:
      return null;
  }
}

function groupContainer(record) {
  const label = stripEnum(record.base.label, "GROUP_LABEL_", "unspecified") || "unspecified";
  if (label === "ordered_list") return el("ol", "dv-list");
  if (label === "list") return el("ul", "dv-list");
  if (label === "inline") return el("div", "dv-inline-group");
  const section = el("section", `dv-group dv-group-${label}`);
  if (record.base.name) {
    const name = el("div", "dv-group-name");
    name.textContent = record.base.name;
    section.appendChild(name);
  }
  return section;
}

// Depth-first walk of a children list, rendering the items the context's
// predicate includes. Groups render as containers whenever anything inside
// them rendered; a revisit guard tolerates reference cycles.
function renderRun(refs, ctx, container) {
  for (const refItem of refs || []) {
    const ref = refString(refItem);
    if (!ref || ctx.seen.has(ref) || ctx.consumed.has(ref)) continue;
    if (ref === "#/body" || ref === "#/furniture") continue;
    ctx.seen.add(ref);
    const record = model.byRef.get(ref);
    if (!record) continue;
    if (record.kind === "group") {
      const group = groupContainer(record);
      renderRun(record.base.children, ctx, group);
      // Only groups that actually rendered something appear; a group whose
      // items all live on other pages leaves no empty shell behind.
      if (group.querySelector(".dv-item")) container.appendChild(group);
      continue;
    }
    if (ctx.include(record)) {
      const main = buildItemElement(record, ctx);
      if (main) attach(container, main, record, ctx);
    }
    renderRun(record.base.children, ctx, container);
  }
}

function makeContext(include, options) {
  return {
    include,
    seen: new Set(),
    consumed: (options && options.consumed) || new Set(),
    showConfidence: Boolean(options && options.showConfidence),
  };
}

// ---------------------------------------------------------------------------
// Provenance overlay (left pane).
// ---------------------------------------------------------------------------

const hiddenLabels = new Set();
const hiddenCollectors = new Set();

// Label and collector filters are independent classes, so an item is only
// visible when both of its filters are on.
function labelHidden(record) {
  return hiddenLabels.has(record.label);
}

function collectorHidden(record) {
  return record.collector !== null && hiddenCollectors.has(record.collector);
}

// A wrapped item hides with its metadata drawer, not on its own.
function hideTargetOf(element) {
  const parent = element.parentElement;
  return parent && parent.classList.contains("dv-item-block") ? parent : element;
}

// One pass over the model per filter change. Hover and render paths never
// look at the filter sets, so this is the only place visibility is decided.
function applyFilters() {
  if (!model) return;
  for (const record of model.items) {
    if (record.kind === "group") continue;
    const entry = model.view.get(record.ref);
    if (!entry) continue;
    const byLabel = labelHidden(record);
    const byCollector = collectorHidden(record);
    for (const box of entry.boxes) {
      box.el.classList.toggle("label-off", byLabel);
      box.el.classList.toggle("collector-off", byCollector);
    }
    for (const element of entry.contents) {
      const target = hideTargetOf(element);
      element.classList.toggle("label-off", byLabel);
      element.classList.toggle("collector-off", byCollector);
      target.classList.toggle("label-off", byLabel);
      target.classList.toggle("collector-off", byCollector);
    }
  }
  // A collector bucket with nothing left to show goes away with its items.
  for (const bucket of results.querySelectorAll(".dv-collector-bucket")) {
    bucket.classList.toggle("dv-bucket-off", !bucket.querySelector(".dv-item:not(.label-off):not(.collector-off)"));
  }
}

function boxGeometry(bbox, size) {
  const width = size.width || 1;
  const height = size.height || 1;
  let left = Number(bbox.l) || 0;
  let right = Number(bbox.r) || 0;
  let top = Number(bbox.t) || 0;
  let bottom = Number(bbox.b) || 0;
  if (bbox.coordOrigin === "COORD_ORIGIN_BOTTOMLEFT") {
    const newTop = height - Math.max(top, bottom);
    const newBottom = height - Math.min(top, bottom);
    top = newTop;
    bottom = newBottom;
  }
  if (right < left) [left, right] = [right, left];
  if (bottom < top) [top, bottom] = [bottom, top];
  const clamp = (value) => Math.min(100, Math.max(0, value));
  return {
    left: clamp((left / width) * 100),
    top: clamp((top / height) * 100),
    width: clamp(((right - left) / width) * 100),
    height: clamp(((bottom - top) / height) * 100),
  };
}

function buildBoxLayer(pageNo, size) {
  const layer = el("div", "dv-box-layer");
  const boxes = [];
  for (const record of model.items) {
    for (const prov of record.prov) {
      if (prov.pageNo !== pageNo || !prov.bbox) continue;
      boxes.push({ record, prov, geometry: boxGeometry(prov.bbox, size) });
    }
  }
  // Large boxes first so small ones stay hoverable on top of them.
  boxes.sort((a, b) => (b.geometry.width * b.geometry.height) - (a.geometry.width * a.geometry.height));
  for (const { record, prov, geometry } of boxes) {
    const box = el("div", "dv-box");
    box.style.left = `${geometry.left}%`;
    box.style.top = `${geometry.top}%`;
    box.style.width = `${geometry.width}%`;
    box.style.height = `${geometry.height}%`;
    const color = colorFor(record.label);
    box.style.borderColor = color.border;
    box.style.background = color.fill;
    // The heatmap colours are stamped on once here; switching the view on
    // only flips a class on the root, it never repaints the boxes.
    if (record.confidence === null) {
      box.classList.add("dv-conf-none");
    } else {
      const heat = confidenceColor(record.confidence);
      box.style.setProperty("--dv-conf-border", heat.border);
      box.style.setProperty("--dv-conf-fill", heat.fill);
    }
    box.dataset.ref = record.ref;
    box.dataset.label = record.label;
    box.dataset.prov = String(prov.index);
    box.title = record.label;
    if (record.collector) box.dataset.collector = record.collector;
    if (!inBodyLayer(record)) box.classList.add("dv-furn");
    if (labelHidden(record)) box.classList.add("label-off");
    if (collectorHidden(record)) box.classList.add("collector-off");
    viewEntryFor(record.ref).boxes.push({ el: box, provIndex: prov.index, geometry, size });
    layer.appendChild(box);
  }
  return layer;
}

// ---------------------------------------------------------------------------
// Reading-order overlay: one SVG layer per page, computed with the page card.
// ---------------------------------------------------------------------------

// The page's items in body-walk order, each with the first box it owns on
// this page. Items without provenance drop out; the ones around them keep
// their relative order, so the numbering reads as the document does.
function pageSequence(pageNo, size) {
  const cached = model.pageSequences.get(pageNo);
  if (cached) return cached;
  const sequence = [];
  for (const record of model.walkOrder) {
    const prov = record.prov.find((entry) => entry.pageNo === pageNo && entry.bbox);
    if (!prov) continue;
    sequence.push({ record, geometry: boxGeometry(prov.bbox, size) });
  }
  model.pageSequences.set(pageNo, sequence);
  return sequence;
}

function svgEl(tag, className) {
  const node = document.createElementNS(SVG_NS, tag);
  if (className) node.setAttribute("class", className);
  return node;
}

function buildArrowLayer(pageNo, size) {
  const sequence = pageSequence(pageNo, size);
  const layer = svgEl("svg", "dv-arrow-layer");
  // Same coordinate space as the percentage-positioned boxes: the viewport
  // is the page visual, so page units map straight onto it.
  layer.setAttribute("viewBox", `0 0 ${size.width} ${size.height}`);
  layer.setAttribute("preserveAspectRatio", "none");
  if (sequence.length === 0) return layer;

  const stroke = Math.max(1, size.width / 500);
  const headId = `dv-arrow-head-${pageNo}`;
  const defs = svgEl("defs");
  const marker = svgEl("marker");
  marker.setAttribute("id", headId);
  marker.setAttribute("viewBox", "0 0 10 10");
  marker.setAttribute("refX", "9");
  marker.setAttribute("refY", "5");
  marker.setAttribute("markerWidth", "5");
  marker.setAttribute("markerHeight", "5");
  marker.setAttribute("orient", "auto-start-reverse");
  const head = svgEl("path");
  head.setAttribute("d", "M0,0 L10,5 L0,10 z");
  head.setAttribute("fill", "currentColor");
  marker.appendChild(head);
  defs.appendChild(marker);
  layer.appendChild(defs);

  const centers = sequence.map(({ geometry }) => ({
    x: ((geometry.left + geometry.width / 2) / 100) * size.width,
    y: ((geometry.top + geometry.height / 2) / 100) * size.height,
  }));
  for (let index = 1; index < centers.length; index += 1) {
    const from = centers[index - 1];
    const to = centers[index];
    const line = svgEl("line", "dv-arrow");
    line.setAttribute("x1", String(from.x));
    line.setAttribute("y1", String(from.y));
    line.setAttribute("x2", String(to.x));
    line.setAttribute("y2", String(to.y));
    line.setAttribute("stroke", "currentColor");
    line.setAttribute("stroke-width", String(stroke));
    line.setAttribute("marker-end", `url(#${headId})`);
    layer.appendChild(line);
  }

  const fontSize = Math.max(8, size.width / 55);
  const chipHeight = fontSize * 1.35;
  for (let index = 0; index < sequence.length; index += 1) {
    const { geometry, record } = sequence[index];
    const number = String(index + 1);
    const chipWidth = Math.max(chipHeight, fontSize * (0.55 * number.length + 0.7));
    const x = Math.min(size.width - chipWidth, Math.max(0, (geometry.left / 100) * size.width));
    const y = Math.min(size.height - chipHeight, Math.max(0, (geometry.top / 100) * size.height));
    const chip = svgEl("g", "dv-order-chip");
    chip.setAttribute("data-order-ref", record.ref);
    const plate = svgEl("rect");
    plate.setAttribute("x", String(x));
    plate.setAttribute("y", String(y));
    plate.setAttribute("width", String(chipWidth));
    plate.setAttribute("height", String(chipHeight));
    plate.setAttribute("rx", String(chipHeight * 0.25));
    plate.setAttribute("fill", "currentColor");
    const text = svgEl("text", "dv-order-chip-text");
    text.setAttribute("x", String(x + chipWidth / 2));
    text.setAttribute("y", String(y + chipHeight / 2));
    text.setAttribute("font-size", String(fontSize));
    text.setAttribute("text-anchor", "middle");
    text.setAttribute("dominant-baseline", "central");
    text.textContent = number;
    chip.append(plate, text);
    layer.appendChild(chip);
  }
  return layer;
}

// ---------------------------------------------------------------------------
// Page cards (built lazily: page images are large data URIs).
// ---------------------------------------------------------------------------

let lazyObserver = null;

function buildPageCardContent(card, pageNo) {
  const size = pageSize(pageNo);
  const visual = el("div", "dv-page-visual");
  visual.style.aspectRatio = `${size.width} / ${size.height}`;
  const page = pageInfo(pageNo);
  const uri = page && page.image && page.image.uri;
  if (isSafeImage(uri)) {
    const img = el("img", "dv-page-img");
    img.src = uri;
    img.alt = `Page ${pageNo}`;
    visual.appendChild(img);
  }
  visual.appendChild(buildBoxLayer(pageNo, size));
  visual.appendChild(buildArrowLayer(pageNo, size));

  const content = el("div", "dv-page-content");
  const consumed = new Set();
  renderRun(rootChildren(model.doc), makeContext((record) => record.page === pageNo, { consumed }), content);
  const furniture = el("div", "dv-furn dv-furn-block");
  renderRun(
    furnitureChildren(model.doc),
    makeContext((record) => record.page === pageNo, { consumed }),
    furniture,
  );
  if (furniture.querySelector(".dv-item")) content.appendChild(furniture);
  if (!content.querySelector(".dv-item")) {
    const empty = el("div", "dv-empty-note");
    empty.textContent = "no content items on this page";
    content.insertBefore(empty, content.firstChild);
  }

  card.textContent = "";
  const heading = el("h2");
  heading.textContent = `Page ${pageNo}`;
  card.append(heading, visual, content);
}

function buildViewer(doc) {
  results.textContent = "";
  results.classList.add("dv-root");
  results.classList.toggle("show-furniture", furnitureToggle.checked);
  results.classList.toggle("show-reading-order", readingOrderToggle.checked);
  results.classList.toggle("show-confidence", confidenceToggle.checked);
  confidenceLegend.hidden = !confidenceToggle.checked;
  toolbar.hidden = false;
  if (lazyObserver) lazyObserver.disconnect();
  model = indexDocument(doc);
  hoveredRef = null;
  hoveredProv = null;
  hoveredTableRef = null;
  hotCell = null;
  charMarks = [];
  hiddenLabels.clear();
  hiddenCollectors.clear();
  renderInfo(doc);
  renderLegend();

  const pages = pageNumbers(doc);
  lazyObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (!entry.isIntersecting) continue;
      const card = entry.target;
      lazyObserver.unobserve(card);
      buildPageCardContent(card, Number(card.dataset.page));
    }
  }, { root: null, rootMargin: "900px 0px" });

  for (const pageNo of pages) {
    const card = el("article", "page-card dv-page-card dv-pending");
    card.dataset.page = String(pageNo);
    const heading = el("h2");
    heading.textContent = `Page ${pageNo}`;
    card.appendChild(heading);
    results.appendChild(card);
    lazyObserver.observe(card);
  }

  renderUnpaged();
  if (pages.length === 0 && !results.querySelector(".dv-unpaged")) {
    banner("warn", "The document has no pages and no items to render.");
  }
}

// ---------------------------------------------------------------------------
// Unpaged content (collector documents merged in without page provenance).
// ---------------------------------------------------------------------------

function renderUnpaged() {
  if (model.unpagedCollectors.length === 0) return;
  const section = el("article", "page-card dv-unpaged");
  const heading = el("h2");
  heading.textContent = "Unpaged content";
  const note = el("span", "dv-note");
  note.textContent = "items without page provenance, grouped by collector";
  heading.appendChild(note);
  section.appendChild(heading);
  const roots = [...rootChildren(model.doc), ...furnitureChildren(model.doc)];
  for (const name of model.unpagedCollectors) {
    const bucket = el("div", "dv-collector-bucket");
    const header = el("div", "dv-collector-header");
    header.appendChild(badge(name, "dv-collector-badge"));
    bucket.appendChild(header);
    const content = el("div", "dv-unpaged-content");
    renderRun(
      roots,
      makeContext(
        (record) => record.page === null && (collectorOf(record) || "unattributed") === name,
        { showConfidence: true },
      ),
      content,
    );
    if (content.querySelector(".dv-item")) {
      bucket.appendChild(content);
      section.appendChild(bucket);
    }
  }
  if (section.querySelector(".dv-collector-bucket")) results.appendChild(section);
}

// ---------------------------------------------------------------------------
// Info bar and legend.
// ---------------------------------------------------------------------------

function infoChip(label, value) {
  const chip = el("span", "dv-info-chip");
  const strong = el("strong");
  strong.textContent = value;
  chip.appendChild(strong);
  chip.appendChild(document.createTextNode(` ${label}`));
  return chip;
}

function renderInfo(doc) {
  infoBar.textContent = "";
  infoBar.hidden = false;
  const name = el("span", "dv-info-name");
  name.textContent = doc.name || (doc.origin && doc.origin.filename) || "document";
  infoBar.appendChild(name);
  if (doc.origin && doc.origin.mimetype) infoBar.appendChild(badge(doc.origin.mimetype, "dv-mime-badge"));
  const pageCount = Object.keys(doc.pages || {}).length;
  infoBar.appendChild(infoChip(pageCount === 1 ? "page" : "pages", String(pageCount)));
  const counts = [
    ["texts", (doc.texts || []).length],
    ["tables", (doc.tables || []).length],
    ["pictures", (doc.pictures || []).length],
    ["groups", (doc.groups || []).length],
    ["key-values", (doc.keyValueItems || []).length],
    ["forms", (doc.formItems || []).length],
  ];
  for (const [label, count] of counts) {
    if (count > 0) infoBar.appendChild(infoChip(label, String(count)));
  }
}

function renderLegend() {
  legendEntries.textContent = "";
  legendBar.hidden = false;
  const entries = [...model.labelCounts.entries()].sort((a, b) => b[1] - a[1]);
  for (const [label, count] of entries) {
    const entry = el("label", "dv-legend-item");
    const checkbox = el("input");
    checkbox.type = "checkbox";
    checkbox.checked = !hiddenLabels.has(label);
    checkbox.addEventListener("change", () => {
      if (checkbox.checked) hiddenLabels.delete(label);
      else hiddenLabels.add(label);
      applyFilters();
    });
    const swatch = el("i", "dv-swatch");
    const color = colorFor(label);
    swatch.style.borderColor = color.border;
    swatch.style.background = color.fill;
    const text = el("span");
    text.textContent = `${label.replace(/_/g, " ")} ×${count}`;
    entry.append(checkbox, swatch, text);
    legendEntries.appendChild(entry);
  }
  renderCollectorLegend();
}

// Every collector that contributed items, most prolific first. Filtering by
// collector combines with the label filters above: an item shows only when
// both of its filters are on.
function renderCollectorLegend() {
  legendCollectors.textContent = "";
  const entries = [...model.collectorCounts.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
  for (const [name, count] of entries) {
    const entry = el("label", "dv-legend-item dv-collector-item");
    const checkbox = el("input");
    checkbox.type = "checkbox";
    checkbox.checked = !hiddenCollectors.has(name);
    checkbox.dataset.collector = name;
    checkbox.addEventListener("change", () => {
      if (checkbox.checked) hiddenCollectors.delete(name);
      else hiddenCollectors.add(name);
      applyFilters();
    });
    const text = el("span");
    text.textContent = `${name} ×${count}`;
    entry.append(checkbox, text);
    legendCollectors.appendChild(entry);
  }
}

furnitureToggle.addEventListener("change", () => {
  results.classList.toggle("show-furniture", furnitureToggle.checked);
});

// Feature toggles only flip a class on the root; the layers they reveal are
// built with the page card and never recomputed.
readingOrderToggle.addEventListener("change", () => {
  results.classList.toggle("show-reading-order", readingOrderToggle.checked);
});

confidenceToggle.addEventListener("change", () => {
  results.classList.toggle("show-confidence", confidenceToggle.checked);
  confidenceLegend.hidden = !confidenceToggle.checked;
});

// ---------------------------------------------------------------------------
// Hover / click sync between boxes and content, by shared data-ref.
// ---------------------------------------------------------------------------

let hoveredRef = null;
let hoveredProv = null;
let charMarks = [];

// Text that belongs to the viewer's own chrome rather than to the item.
const MARK_SKIP = ".dv-meta, .dv-badge, .dv-conf, .dv-code-lang, .dv-kind-badge, .dv-figure-badges, .dv-figcaption";

// Wraps [start, end) of an element's own text in <mark>, splitting across
// text nodes when the range spans several. Returns the marks it created so
// the caller can take them out again; nothing else about the DOM changes.
function markRange(root, start, end, className) {
  const targets = [];
  let offset = 0;
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, null);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const parent = node.parentElement;
    if (!parent || (parent !== root && parent.closest(MARK_SKIP))) continue;
    const length = node.nodeValue.length;
    const from = Math.max(start, offset);
    const to = Math.min(end, offset + length);
    if (from < to) targets.push([node, from - offset, to - offset]);
    offset += length;
    if (offset >= end) break;
  }
  const marks = [];
  for (const [node, from, to] of targets) {
    const range = document.createRange();
    range.setStart(node, from);
    range.setEnd(node, to);
    const mark = el("mark", className);
    range.surroundContents(mark);
    marks.push(mark);
  }
  return marks;
}

function unmark(marks) {
  for (const mark of marks) {
    const parent = mark.parentNode;
    if (!parent) continue;
    while (mark.firstChild) parent.insertBefore(mark.firstChild, mark);
    parent.removeChild(mark);
    parent.normalize();
  }
}

function clearCharMarks() {
  if (charMarks.length === 0) return;
  unmark(charMarks);
  charMarks = [];
}

// Hovering one box narrows the highlight to the slice of text that box
// covers, when the producer said which slice that is.
function applyCharMarks(ref, provIndex) {
  const record = model && model.byRef.get(ref);
  if (!record || !record.spans || provIndex === null) return;
  const span = record.spans.get(provIndex);
  if (!span) return;
  const entry = model.view.get(ref);
  if (!entry) return;
  for (const element of entry.contents) {
    charMarks.push(...markRange(element, span.start, span.end, "dv-charmark"));
  }
}

// Every box the item owns lights up, so an item broken into per-line
// provenance shows all of its lines at once; the box actually under the
// pointer, if any, gets the stronger outline.
// ---------------------------------------------------------------------------
// Table cell rectangles, drawn inside the table's own box on first hover.
// ---------------------------------------------------------------------------

let hoveredTableRef = null;
let hotCell = null;

function buildCellLayer(record, boxInfo) {
  const layer = el("div", "dv-cell-layer");
  const outer = boxInfo.geometry;
  if (outer.width > 0 && outer.height > 0) {
    for (const cell of record.cellBoxes) {
      const inner = boxGeometry(cell.bbox, boxInfo.size);
      const left = ((inner.left - outer.left) / outer.width) * 100;
      const top = ((inner.top - outer.top) / outer.height) * 100;
      const width = (inner.width / outer.width) * 100;
      const height = (inner.height / outer.height) * 100;
      if (width <= 0 || height <= 0) continue;
      const sub = el("div", "dv-cell");
      sub.style.left = `${left}%`;
      sub.style.top = `${top}%`;
      sub.style.width = `${width}%`;
      sub.style.height = `${height}%`;
      sub.dataset.cellpos = cell.pos;
      layer.appendChild(sub);
    }
  }
  boxInfo.el.appendChild(layer);
  return layer;
}

// Built once per table box and kept; later hovers only flip classes.
function showTableCells(ref) {
  const record = model && model.byRef.get(ref);
  const entry = record && model.view.get(ref);
  if (!record || !entry || !record.cellBoxes || record.cellBoxes.length === 0) return;
  for (const boxInfo of entry.boxes) {
    if (!boxInfo.cellLayer) boxInfo.cellLayer = buildCellLayer(record, boxInfo);
    boxInfo.el.classList.add("dv-cells-on");
  }
}

function hideTableCells(ref) {
  const entry = ref && model ? model.view.get(ref) : null;
  if (!entry) return;
  for (const boxInfo of entry.boxes) boxInfo.el.classList.remove("dv-cells-on");
}

function setHotCell(ref, pos) {
  if (hotCell) {
    hotCell.classList.remove("dv-cell-hot");
    hotCell = null;
  }
  const entry = ref && pos && model ? model.view.get(ref) : null;
  if (!entry) return;
  for (const boxInfo of entry.boxes) {
    const sub = boxInfo.cellLayer && boxInfo.cellLayer.querySelector(`[data-cellpos="${pos}"]`);
    if (sub) {
      sub.classList.add("dv-cell-hot");
      hotCell = sub;
      return;
    }
  }
}

function syncTableCells(target) {
  const wrap = target ? target.closest(".dv-table-wrap[data-ref]") : null;
  const ref = wrap ? wrap.dataset.ref : null;
  if (ref !== hoveredTableRef) {
    hideTableCells(hoveredTableRef);
    setHotCell(null, null);
    hoveredTableRef = ref;
    if (ref) showTableCells(ref);
  }
  if (!ref) return;
  const cell = target.closest("td[data-cellpos], th[data-cellpos]");
  const pos = cell ? cell.dataset.cellpos : null;
  if (!hotCell || !cell || hotCell.dataset.cellpos !== pos) setHotCell(ref, pos);
}

function setHighlight(ref, on, provIndex) {
  const entry = ref && model ? model.view.get(ref) : null;
  if (!entry) return;
  for (const box of entry.boxes) {
    box.el.classList.toggle("dv-hl", on);
    box.el.classList.toggle("dv-hl-self", on && provIndex !== null && provIndex !== undefined && box.provIndex === provIndex);
  }
  for (const element of entry.contents) element.classList.toggle("dv-hl", on);
}

results.addEventListener("mouseover", (event) => {
  syncTableCells(event.target);
  const target = event.target.closest("[data-ref]");
  const ref = target ? target.dataset.ref : null;
  const provAttr = target && target.dataset.prov;
  const provIndex = provAttr === undefined || provAttr === null || provAttr === "" ? null : Number(provAttr);
  if (ref === hoveredRef && provIndex === hoveredProv) return;
  setHighlight(hoveredRef, false, hoveredProv);
  clearCharMarks();
  hoveredRef = ref;
  hoveredProv = provIndex;
  setHighlight(hoveredRef, true, provIndex);
  if (hoveredRef && hoveredProv !== null) applyCharMarks(hoveredRef, hoveredProv);
});

results.addEventListener("mouseleave", () => {
  setHighlight(hoveredRef, false, hoveredProv);
  clearCharMarks();
  hideTableCells(hoveredTableRef);
  setHotCell(null, null);
  hoveredTableRef = null;
  hoveredRef = null;
  hoveredProv = null;
});

results.addEventListener("click", (event) => {
  if (event.target.closest("a, summary, input")) return;
  const target = event.target.closest("[data-ref]");
  if (!target) return;
  const entry = model && model.view.get(target.dataset.ref);
  if (!entry) return;
  const wantBox = !target.classList.contains("dv-box");
  const counterpart = wantBox
    ? (entry.boxes.length > 0 ? entry.boxes[0].el : null)
    : (entry.contents.length > 0 ? entry.contents[0] : null);
  if (!counterpart) return;
  counterpart.scrollIntoView({ behavior: "smooth", block: "center" });
  counterpart.classList.add("dv-flash");
  setTimeout(() => counterpart.classList.remove("dv-flash"), 1600);
});

// ---------------------------------------------------------------------------
// Upload plumbing: NDJSON relay from /api/document/parse.
// ---------------------------------------------------------------------------

let startedAt = 0;
let clock = null;

function contentTypeFor(name) {
  const ext = name.toLowerCase().split(".").pop();
  return {
    pdf: "application/pdf", png: "image/png", jpg: "image/jpeg",
    jpeg: "image/jpeg", tif: "image/tiff", tiff: "image/tiff",
  }[ext] || "";
}

function resetRun() {
  results.textContent = "";
  results.classList.remove("dv-root");
  infoBar.hidden = true;
  toolbar.hidden = true;
  legendBar.hidden = true;
  progressPages.textContent = "";
  progressBar.hidden = false;
  if (lazyObserver) {
    lazyObserver.disconnect();
    lazyObserver = null;
  }
  model = null;
  hoveredRef = null;
  hoveredProv = null;
  hoveredTableRef = null;
  hotCell = null;
  charMarks = [];
  startedAt = performance.now();
  clock = setInterval(() => {
    progressElapsed.textContent = `${((performance.now() - startedAt) / 1000).toFixed(1)}s`;
  }, 100);
}

function finishRun() {
  if (clock) clearInterval(clock);
  clock = null;
  progressBar.hidden = true;
  dropzone.classList.remove("busy");
}

function handleEvent(event) {
  if (event.type === "page") {
    const chip = el("span", "dv-progress-chip");
    const total = event.totalPages ? `/${event.totalPages}` : "";
    chip.textContent = `p${event.pageNumber}${total} · ${((performance.now() - startedAt) / 1000).toFixed(1)}s`;
    progressPages.appendChild(chip);
    // Long documents: only the most recent arrivals stay visible.
    while (progressPages.childNodes.length > 14) progressPages.removeChild(progressPages.firstChild);
  } else if (event.type === "document") {
    if (event.document && typeof event.document === "object") buildViewer(event.document);
    else banner("error", "The stream's document line carried no document.");
  } else if (event.type === "error") {
    banner("error", `Parse failed: ${event.message || "unknown error"}`);
  }
}

async function parseFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  const query = new URLSearchParams({
    filename: file.name,
    contentType: file.type || contentTypeFor(file.name),
  });
  try {
    const response = await fetch(`api/document/parse?${query}`, { method: "POST", body: file });
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
sampleButton.addEventListener("click", async (event) => {
  event.stopPropagation(); // the surrounding dropzone opens the file picker on click
  const response = await fetch("sample.pdf");
  if (!response.ok) {
    banner("error", `Could not fetch the bundled sample: HTTP ${response.status}`);
    return;
  }
  const bytes = await response.blob();
  parseFile(new File([bytes], "sample.pdf", { type: "application/pdf" }));
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

fetch("api/document/status")
  .then((response) => response.json())
  .then((status) => {
    const chip = document.getElementById("health");
    if (status.reachable) {
      chip.textContent = status.version ? `service reachable (${status.version})` : "service reachable";
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
