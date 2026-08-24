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
const sampleButtons = document.querySelectorAll(".dv-sample-btn");
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
const searchInput = document.getElementById("search-input");
const searchCount = document.getElementById("search-count");
const searchPrev = document.getElementById("search-prev");
const searchNext = document.getElementById("search-next");
const downloadButton = document.getElementById("download-json");

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

// Cell texts of a table, keyed the same way as the cell rectangles, so a
// search hit inside a table knows which cell it landed in.
function tableCellTexts(data) {
  if (!data || typeof data !== "object") return [];
  const cells = [];
  const seen = new Set();
  const push = (cell) => {
    if (!cell || !cell.text) return;
    const pos = `${Number(cell.startRowOffsetIdx) || 0}:${Number(cell.startColOffsetIdx) || 0}`;
    if (seen.has(pos)) return;
    seen.add(pos);
    cells.push({ pos, text: cell.text });
  };
  for (const cell of data.tableCells || []) push(cell);
  for (const row of data.grid || []) {
    const rowCells = row && Array.isArray(row.cells) ? row.cells : (Array.isArray(row) ? row : []);
    for (const cell of rowCells) push(cell);
  }
  return cells;
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
    // Lowercased once here; search never touches the document again.
    searchEntries: [],
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
      if (text) built.searchEntries.push({ ref: record.ref, pos: null, lower: text.toLowerCase() });
      if (kind === "table") {
        for (const cell of tableCellTexts(node.data)) {
          built.searchEntries.push({ ref: record.ref, pos: cell.pos, lower: cell.text.toLowerCase() });
        }
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
  // Hidden items drop out of the match list rather than being navigated to.
  if (searchQuery) runSearch();
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
  anchoredRef = null;
  anchorMarks = [];
  hiddenLabels.clear();
  hiddenCollectors.clear();
  clearSearch();
  renderInfo(doc);
  renderLegend();

  const pages = pageNumbers(doc);
  lazyObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (!entry.isIntersecting) continue;
      const card = entry.target;
      lazyObserver.unobserve(card);
      buildPageCardContent(card, Number(card.dataset.page));
      // The card's content is new, so any active search has to mark it too.
      if (searchQuery) paintSearchMarks();
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
  applyAnchorFromHash();
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
  if (loadedDialect) infoBar.appendChild(badge(`dialect: ${loadedDialect}`, "dv-dialect-badge"));
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

// Charspans on the wire count Unicode code points (the schema contract);
// DOM ranges and JS string indices count UTF-16 units. These convert at the
// boundary, with a fast path when the text has no surrogate pairs.
function cpToUtf16(text, cp) {
  if (!/[\uD800-\uDFFF]/.test(text)) return Math.min(cp, text.length);
  let units = 0;
  let points = 0;
  for (const ch of text) {
    if (points >= cp) break;
    points += 1;
    units += ch.length;
  }
  return units;
}

function utf16ToCp(text, units) {
  if (!/[\uD800-\uDFFF]/.test(text)) return Math.min(units, text.length);
  let cp = 0;
  let at = 0;
  for (const ch of text) {
    if (at >= units) break;
    at += ch.length;
    cp += 1;
  }
  return cp;
}

// The text markRange and textOffsetOf walk: every text node under root,
// skipping the same chrome both of them skip.
function walkedText(root) {
  let text = "";
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, null);
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const parent = node.parentElement;
    if (!parent || (parent !== root && parent.closest(MARK_SKIP))) continue;
    text += node.nodeValue;
  }
  return text;
}

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
  const itemText = record.text || "";
  const from = cpToUtf16(itemText, span.start);
  const to = cpToUtf16(itemText, span.end);
  if (to <= from) return;
  for (const element of entry.contents) {
    charMarks.push(...markRange(element, from, to, "dv-charmark"));
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
  syncCopyButton(target);
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
  scheduleHideCopyButton();
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
// Deep links: #item=<ref>[&cs=<start>-<end>] scrolls both panes to one item
// and outlines it persistently (distinct from the transient hover outline)
// until another anchor lands or the viewer is told to forget it. Parsed
// once the document finishes rendering and again on every hashchange, so a
// link works while the document stays loaded; neither path re-walks the
// document, both just read the ref -> elements map hover already built.
// ---------------------------------------------------------------------------

let anchoredRef = null;
let anchorMarks = [];
let anchorNoticeTimer = null;

const anchorNotice = el("div", "dv-anchor-notice");
anchorNotice.hidden = true;
document.body.appendChild(anchorNotice);

function showAnchorNotice(message) {
  anchorNotice.textContent = message;
  anchorNotice.hidden = false;
  if (anchorNoticeTimer) clearTimeout(anchorNoticeTimer);
  anchorNoticeTimer = setTimeout(() => {
    anchorNotice.hidden = true;
    anchorNoticeTimer = null;
  }, 3200);
}

// Removes whatever anchor is currently applied, if any. Safe to call with
// no anchor active.
function clearAnchor() {
  if (anchoredRef && model) {
    const entry = model.view.get(anchoredRef);
    if (entry) {
      for (const box of entry.boxes) box.el.classList.remove("dv-anchored");
      for (const element of entry.contents) element.classList.remove("dv-anchored");
    }
  }
  if (anchorMarks.length > 0) unmark(anchorMarks);
  anchorMarks = [];
  anchoredRef = null;
}

// "12-34" -> {start, end}; anything else (missing, malformed, empty range)
// is treated as no charspan rather than an error.
function parseCharspan(raw) {
  const match = typeof raw === "string" ? /^(\d+)-(\d+)$/.exec(raw.trim()) : null;
  if (!match) return null;
  const start = Number(match[1]);
  const end = Number(match[2]);
  return end > start ? { start, end } : null;
}

function parseAnchorHash() {
  const raw = location.hash.startsWith("#") ? location.hash.slice(1) : location.hash;
  if (!raw) return null;
  const params = new URLSearchParams(raw);
  const ref = params.get("item");
  if (!ref) return null;
  return { ref, cs: parseCharspan(params.get("cs")) };
}

// Applies (or reapplies) the current #item=/&cs= fragment against whatever
// document is loaded right now. Never throws: a ref the document does not
// have just leaves a notice, and a missing document is a silent no-op.
function applyAnchorFromHash() {
  const anchor = parseAnchorHash();
  clearAnchor();
  if (!anchor || !model) return;
  const record = model.byRef.get(anchor.ref);
  if (record && record.page) ensurePageBuilt(record.page);
  const entry = record ? model.view.get(record.ref) : null;
  if (!record || !entry || (entry.boxes.length === 0 && entry.contents.length === 0)) {
    showAnchorNotice(`No item matches "${anchor.ref}" in this document.`);
    return;
  }
  anchoredRef = record.ref;
  for (const box of entry.boxes) box.el.classList.add("dv-anchored");
  for (const element of entry.contents) element.classList.add("dv-anchored");
  if (entry.boxes[0]) scrollElementIntoView(entry.boxes[0].el);
  if (entry.contents[0]) scrollElementIntoView(entry.contents[0]);
  if (anchor.cs) {
    const text = record.text || "";
    const start = cpToUtf16(text, Math.max(anchor.cs.start, 0));
    const end = cpToUtf16(text, Math.max(anchor.cs.end, 0));
    if (end > start) {
      for (const element of entry.contents) {
        anchorMarks.push(...markRange(element, start, end, "dv-anchormark"));
      }
    }
  }
}

window.addEventListener("hashchange", applyAnchorFromHash);
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && anchoredRef) clearAnchor();
});

// ---------------------------------------------------------------------------
// Copy-anchor affordance: one floating button, shown next to whichever
// content element is hovered (never one button per item), that writes a
// deep link to that item to the clipboard.
// ---------------------------------------------------------------------------

function buildCopyIcon() {
  const svg = svgEl("svg", "dv-copy-icon");
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("width", "12");
  svg.setAttribute("height", "12");
  svg.setAttribute("aria-hidden", "true");
  const path = svgEl("path");
  path.setAttribute("fill", "currentColor");
  path.setAttribute(
    "d",
    "M6.5 9.5a1 1 0 0 1 0-1.4l3-3a3 3 0 1 1 4.24 4.24l-1.5 1.5a1 1 0 1 1-1.42-1.42l1.5-1.5a1 1 0 1 0-1.4-1.4l-3 3a1 1 0 0 1-1.42 0zm3-3a1 1 0 0 1 0 1.4l-3 3a3 3 0 1 1-4.24-4.24l1.5-1.5A1 1 0 1 1 5.18 6.6l-1.5 1.5a1 1 0 1 0 1.4 1.4l3-3a1 1 0 0 1 1.42 0z",
  );
  svg.appendChild(path);
  return svg;
}

const copyButton = el("button", "dv-copy-btn");
copyButton.type = "button";
copyButton.hidden = true;
copyButton.setAttribute("aria-label", "copy link to this item");
copyButton.appendChild(buildCopyIcon());
document.body.appendChild(copyButton);

let copyHoverTarget = null;
let copyHideTimer = null;

function cancelHideCopyButton() {
  if (copyHideTimer) {
    clearTimeout(copyHideTimer);
    copyHideTimer = null;
  }
}

function hideCopyButtonNow() {
  copyButton.hidden = true;
  delete copyButton.dataset.ref;
  copyHoverTarget = null;
}

// A short delay (rather than hiding immediately) lets the pointer travel
// from the hovered item onto the button itself without it disappearing
// first; entering the button, or a still-valid item, cancels the hide.
function scheduleHideCopyButton() {
  cancelHideCopyButton();
  copyHideTimer = setTimeout(() => {
    hideCopyButtonNow();
    copyHideTimer = null;
  }, 150);
}

function positionCopyButton(target) {
  const rect = target.getBoundingClientRect();
  const size = 22;
  copyButton.style.top = `${Math.max(4, rect.top + 4)}px`;
  copyButton.style.left = `${Math.max(4, rect.right - size - 4)}px`;
}

// Called from the results mouseover delegate with whatever [data-ref]
// element the pointer is currently over (a box, a content element, or
// none); only content elements get the affordance.
function syncCopyButton(target) {
  if (!target || !target.classList.contains("dv-item")) {
    scheduleHideCopyButton();
    return;
  }
  cancelHideCopyButton();
  copyHoverTarget = target;
  copyButton.dataset.ref = target.dataset.ref;
  copyButton.hidden = false;
  positionCopyButton(target);
}

copyButton.addEventListener("mouseenter", cancelHideCopyButton);
copyButton.addEventListener("mouseleave", scheduleHideCopyButton);

// Boundary offset (in markRange's own coordinate space) of a selection
// endpoint inside `root`, so a copied charspan lands on the exact text the
// user selected.
function textOffsetOf(root, node, nodeOffset) {
  if (node.nodeType === 3) {
    let offset = 0;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, null);
    for (let cur = walker.nextNode(); cur; cur = walker.nextNode()) {
      const parent = cur.parentElement;
      if (!parent || (parent !== root && parent.closest(MARK_SKIP))) continue;
      if (cur === node) return offset + nodeOffset;
      offset += cur.nodeValue.length;
    }
    return offset;
  }
  // The boundary sits between child nodes (e.g. a triple-click selection):
  // measure everything up to that child.
  const probe = document.createRange();
  probe.selectNodeContents(root);
  probe.setEnd(node, Math.min(nodeOffset, node.childNodes.length));
  return probe.toString().length;
}

// The active selection's charspan, in item-relative text offsets, but only
// when the whole selection lies inside `container`; otherwise null, which
// tells the caller to omit &cs= entirely.
function selectionSpanWithin(container) {
  try {
    const selection = typeof window.getSelection === "function" ? window.getSelection() : null;
    if (!selection || selection.rangeCount === 0 || selection.isCollapsed) return null;
    const range = selection.getRangeAt(0);
    if (!container.contains(range.startContainer) || !container.contains(range.endContainer)) return null;
    const startUnits = textOffsetOf(container, range.startContainer, range.startOffset);
    const endUnits = textOffsetOf(container, range.endContainer, range.endOffset);
    if (endUnits <= startUnits) return null;
    const text = walkedText(container);
    const start = utf16ToCp(text, startUnits);
    const end = utf16ToCp(text, endUnits);
    return end > start ? { start, end } : null;
  } catch (error) {
    return null;
  }
}

function anchorUrl(ref, cs) {
  const params = new URLSearchParams();
  params.set("item", ref);
  if (cs) params.set("cs", `${cs.start}-${cs.end}`);
  const base = location.href.split("#")[0];
  return `${base}#${params.toString()}`;
}

function flashCopied() {
  copyButton.classList.add("dv-copied");
  copyButton.setAttribute("aria-label", "copied");
  setTimeout(() => {
    copyButton.classList.remove("dv-copied");
    copyButton.setAttribute("aria-label", "copy link to this item");
  }, 1200);
}

async function performCopyAnchor() {
  const ref = copyButton.dataset.ref;
  if (!ref || !model) return;
  const record = model.byRef.get(ref);
  if (!record) return;
  const cs = copyHoverTarget ? selectionSpanWithin(copyHoverTarget) : null;
  const url = anchorUrl(ref, cs);
  try {
    if (navigator.clipboard && typeof navigator.clipboard.writeText === "function") {
      await navigator.clipboard.writeText(url);
    }
    flashCopied();
  } catch (error) {
    // Clipboard access denied or unavailable: nothing to recover from here,
    // and nothing worth interrupting the user over.
  }
}

copyButton.addEventListener("click", (event) => {
  event.preventDefault();
  event.stopPropagation();
  performCopyAnchor();
});

// ---------------------------------------------------------------------------
// Search over the prebuilt text index.
// ---------------------------------------------------------------------------

// Enough to navigate a long document without marking up thousands of nodes.
const MAX_MATCHES = 1000;
const SEARCH_DEBOUNCE_MS = 150;

let searchQuery = "";
let searchMatches = [];
let searchCurrent = -1;
let searchMarks = [];
let searchTimer = null;
let searchBoxes = [];

function scrollElementIntoView(element) {
  if (element && typeof element.scrollIntoView === "function") {
    element.scrollIntoView({ behavior: "smooth", block: "center" });
  }
}

function pageCard(pageNo) {
  return results.querySelector(`.dv-page-card[data-page="${pageNo}"]`);
}

// Navigation can land on a page that has not rendered yet, so build it now
// rather than waiting for the observer to catch up.
function ensurePageBuilt(pageNo) {
  const card = pageCard(pageNo);
  if (!card || card.querySelector(".dv-page-visual")) return card;
  if (lazyObserver) lazyObserver.unobserve(card);
  buildPageCardContent(card, pageNo);
  return card;
}

function contentElementsFor(match) {
  const entry = model ? model.view.get(match.ref) : null;
  if (!entry) return [];
  if (match.pos === null) return entry.contents;
  const cells = [];
  for (const element of entry.contents) {
    const cell = element.querySelector(`td[data-cellpos="${match.pos}"], th[data-cellpos="${match.pos}"]`);
    if (cell) cells.push(cell);
  }
  return cells;
}

function clearSearchMarks() {
  if (searchMarks.length > 0) unmark(searchMarks);
  searchMarks = [];
  for (const match of searchMatches) match.marks = [];
}

function paintSearchMarks() {
  clearSearchMarks();
  for (const match of searchMatches) {
    for (const element of contentElementsFor(match)) {
      match.marks.push(...markRange(element, match.start, match.end, "dv-searchmark"));
    }
    searchMarks.push(...match.marks);
  }
  highlightCurrent();
}

function highlightCurrent() {
  for (const mark of searchMarks) mark.classList.remove("dv-searchmark-current");
  for (const box of searchBoxes) box.classList.remove("dv-search-box");
  searchBoxes = [];
  const match = searchMatches[searchCurrent];
  if (!match) return;
  for (const mark of match.marks) mark.classList.add("dv-searchmark-current");
  const entry = model.view.get(match.ref);
  for (const box of (entry ? entry.boxes : [])) {
    box.el.classList.add("dv-search-box");
    searchBoxes.push(box.el);
  }
}

function renderSearchCount() {
  if (!searchQuery) {
    searchCount.textContent = "";
    return;
  }
  const total = searchMatches.length;
  const noun = total === 1 ? "match" : "matches";
  searchCount.textContent = searchCurrent >= 0
    ? `${searchCurrent + 1}/${total} ${noun}`
    : `${total} ${noun}`;
}

// Substring scan over the lowercased index. Filtered-out items never enter
// the result list, so navigation cannot land on something invisible.
function runSearch() {
  searchCurrent = -1;
  searchMatches = [];
  if (model && searchQuery) {
    for (const entry of model.searchEntries) {
      const record = model.byRef.get(entry.ref);
      if (!record || labelHidden(record) || collectorHidden(record)) continue;
      const order = typeof record.walkIndex === "number" ? record.walkIndex : Number.MAX_SAFE_INTEGER;
      let from = 0;
      for (;;) {
        const at = entry.lower.indexOf(searchQuery, from);
        if (at < 0) break;
        searchMatches.push({
          ref: entry.ref,
          pos: entry.pos,
          start: at,
          end: at + searchQuery.length,
          order,
          page: record.page,
          marks: [],
        });
        from = at + searchQuery.length;
        if (searchMatches.length >= MAX_MATCHES) break;
      }
      if (searchMatches.length >= MAX_MATCHES) break;
    }
    // Document order: body walk position first, then position within the item.
    searchMatches.sort((a, b) => a.order - b.order || a.start - b.start);
  }
  paintSearchMarks();
  renderSearchCount();
}

function stepSearch(delta) {
  if (searchMatches.length === 0) return;
  const next = searchCurrent < 0
    ? (delta > 0 ? 0 : searchMatches.length - 1)
    : (searchCurrent + delta + searchMatches.length) % searchMatches.length;
  searchCurrent = next;
  const match = searchMatches[next];
  if (match.page) {
    const card = ensurePageBuilt(match.page);
    // A freshly built card carries none of the marks, so repaint them all.
    if (card && match.marks.length === 0) paintSearchMarks();
    scrollElementIntoView(card);
  }
  highlightCurrent();
  const element = contentElementsFor(match)[0];
  scrollElementIntoView(element);
  renderSearchCount();
}

function setSearchQuery(value) {
  searchQuery = value.trim().toLowerCase();
  runSearch();
}

function clearSearch() {
  searchInput.value = "";
  setSearchQuery("");
}

searchInput.addEventListener("input", () => {
  if (searchTimer) clearTimeout(searchTimer);
  searchTimer = setTimeout(() => {
    searchTimer = null;
    setSearchQuery(searchInput.value);
  }, SEARCH_DEBOUNCE_MS);
});

searchInput.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    event.preventDefault();
    clearSearch();
    return;
  }
  const forward = event.key === "Enter" ? !event.shiftKey : event.key === "ArrowDown";
  if (event.key !== "Enter" && event.key !== "ArrowDown" && event.key !== "ArrowUp") return;
  event.preventDefault();
  // Typing then hitting enter straight away should not lose the last
  // keystroke, and neither should pasting a term and hitting enter.
  if (searchTimer) {
    clearTimeout(searchTimer);
    searchTimer = null;
  }
  if (searchInput.value.trim().toLowerCase() !== searchQuery) setSearchQuery(searchInput.value);
  stepSearch(forward ? 1 : -1);
});

searchPrev.addEventListener("click", () => stepSearch(-1));
searchNext.addEventListener("click", () => stepSearch(1));

// ---------------------------------------------------------------------------
// JSON dialects.
//
// The viewer reads the wire shape the bridge relays: camelCase fields, enum
// value names as strings, text items wrapped in their oneof variant. The
// other JSON shape in circulation spells its fields in snake_case, writes
// references as {"$ref": ...}, charspans as two-element arrays and enums as
// bare lowercase names. Files in that dialect are converted here, in the
// browser, before the walker sees them.
// ---------------------------------------------------------------------------

const CANONICAL_SCHEMA_NAME = "DoclingDocument";
const DIALECT_LABELS = { canonical: "canonical json", native: "protobuf-json" };

let loadedDialect = null;

// Every field the walker reads that is spelled differently in the other
// dialect. Anything not listed keeps its name.
const CANONICAL_KEYS = {
  self_ref: "selfRef",
  content_layer: "contentLayer",
  page_no: "pageNo",
  coord_origin: "coordOrigin",
  code_language: "codeLanguageRaw",
  key_value_items: "keyValueItems",
  form_items: "formItems",
  field_regions: "fieldRegions",
  field_items: "fieldItems",
  table_cells: "tableCells",
  num_rows: "numRows",
  num_cols: "numCols",
  row_span: "rowSpan",
  col_span: "colSpan",
  start_row_offset_idx: "startRowOffsetIdx",
  end_row_offset_idx: "endRowOffsetIdx",
  start_col_offset_idx: "startColOffsetIdx",
  end_col_offset_idx: "endColOffsetIdx",
  column_header: "columnHeader",
  row_header: "rowHeader",
  row_section: "rowSection",
  predicted_classes: "predictedClasses",
  class_name: "className",
  cell_id: "cellId",
  source_cell_id: "sourceCellId",
  target_cell_id: "targetCellId",
  custom_fields: "customFields",
};

const CANONICAL_TEXT_VARIANTS = {
  title: "title",
  section_header: "sectionHeader",
  list_item: "listItem",
  code: "code",
  formula: "formula",
};

// Fields that belong to the variant wrapper rather than to the base item.
const VARIANT_FIELDS = ["level", "enumerated", "marker", "kind"];

function enumName(prefix, value) {
  return typeof value === "string" && value ? `${prefix}${value.toUpperCase()}` : undefined;
}

function looksCanonical(parsed) {
  if (parsed.schema_name === CANONICAL_SCHEMA_NAME) return true;
  const roots = [parsed.body, parsed.furniture];
  for (const root of roots) {
    const children = root && Array.isArray(root.children) ? root.children : [];
    for (const child of children) {
      if (child && typeof child === "object" && typeof child.$ref === "string") return true;
    }
    if (root && typeof root === "object" && typeof root.self_ref === "string") return true;
  }
  for (const arena of ["texts", "tables", "pictures", "groups"]) {
    const first = Array.isArray(parsed[arena]) ? parsed[arena][0] : null;
    if (first && typeof first === "object" && typeof first.self_ref === "string") return true;
  }
  return false;
}

function detectDialect(parsed) {
  return looksCanonical(parsed) ? "canonical" : "native";
}

// Key renames and {"$ref": x} -> {ref: x}, applied throughout.
function convertKeys(value) {
  if (Array.isArray(value)) return value.map(convertKeys);
  if (value && typeof value === "object") {
    const out = {};
    for (const [key, entry] of Object.entries(value)) {
      out[key === "$ref" ? "ref" : (CANONICAL_KEYS[key] || key)] = convertKeys(entry);
    }
    return out;
  }
  return value;
}

function adaptProv(base) {
  if (!Array.isArray(base.prov)) return;
  base.prov = base.prov.map((prov) => {
    if (!prov || typeof prov !== "object") return prov;
    const out = { ...prov };
    if (Array.isArray(prov.charspan)) {
      out.charspan = { start: Number(prov.charspan[0]) || 0, end: Number(prov.charspan[1]) || 0 };
    }
    if (out.bbox && typeof out.bbox === "object") {
      const origin = enumName("COORD_ORIGIN_", out.bbox.coordOrigin);
      out.bbox = origin ? { ...out.bbox, coordOrigin: origin } : { ...out.bbox };
    }
    return out;
  });
}

function adaptBase(base, labelPrefix) {
  const label = enumName(labelPrefix, base.label);
  if (label) base.label = label;
  const layer = enumName("CONTENT_LAYER_", base.contentLayer);
  if (layer) base.contentLayer = layer;
  adaptProv(base);
}

// Text items arrive flat, carrying their semantics in the label. The walker
// reads them through the oneof wrapper, so the label picks the variant and
// the variant-only fields move onto the wrapper.
function adaptTextItem(item) {
  const name = typeof item.label === "string" ? item.label.toLowerCase() : "text";
  const variant = CANONICAL_TEXT_VARIANTS[name] || "text";
  adaptBase(item, "DOC_ITEM_LABEL_");
  if (variant === "code") return { item: variant, code: item };
  const node = {};
  const base = {};
  for (const [key, value] of Object.entries(item)) {
    if (VARIANT_FIELDS.includes(key)) node[key] = value;
    else base[key] = value;
  }
  node.base = base;
  return { item: variant, [variant]: node };
}

function adaptTable(table) {
  adaptBase(table, "DOC_ITEM_LABEL_");
  const data = table.data;
  if (!data || typeof data !== "object") return table;
  for (const cell of data.tableCells || []) {
    if (cell && cell.bbox && typeof cell.bbox === "object") {
      const origin = enumName("COORD_ORIGIN_", cell.bbox.coordOrigin);
      if (origin) cell.bbox.coordOrigin = origin;
    }
  }
  // Rows arrive as plain arrays of cells; the walker reads {cells: [...]}.
  if (Array.isArray(data.grid)) {
    data.grid = data.grid.map((row) => (Array.isArray(row) ? { cells: row } : row));
    for (const row of data.grid) {
      for (const cell of (row && row.cells) || []) {
        if (cell && cell.bbox && typeof cell.bbox === "object") {
          const origin = enumName("COORD_ORIGIN_", cell.bbox.coordOrigin);
          if (origin) cell.bbox.coordOrigin = origin;
        }
      }
    }
  }
  return table;
}

// Annotations are a flat list tagged by kind; the walker reads them as a
// oneof, so the two kinds it renders are re-nested.
function adaptAnnotations(item) {
  if (!Array.isArray(item.annotations)) return;
  item.annotations = item.annotations.map((annotation) => {
    if (!annotation || typeof annotation !== "object" || annotation.kind === undefined) return annotation;
    if (Array.isArray(annotation.predictedClasses)) {
      return { classification: { predictedClasses: annotation.predictedClasses } };
    }
    if (annotation.kind === "description" && typeof annotation.text === "string") {
      return { description: { text: annotation.text } };
    }
    return annotation;
  });
}

function adaptGraph(item) {
  adaptBase(item, "DOC_ITEM_LABEL_");
  const graph = item.graph;
  if (!graph || typeof graph !== "object") return item;
  for (const cell of graph.cells || []) {
    const label = cell && enumName("GRAPH_CELL_LABEL_", cell.label);
    if (label) cell.label = label;
  }
  for (const link of graph.links || []) {
    const label = link && enumName("GRAPH_LINK_LABEL_", link.label);
    if (label) link.label = label;
  }
  return item;
}

function adaptCanonicalDocument(raw) {
  const doc = convertKeys(raw);
  doc.texts = (Array.isArray(doc.texts) ? doc.texts : []).map(adaptTextItem);
  doc.tables = (Array.isArray(doc.tables) ? doc.tables : []).map(adaptTable);
  doc.pictures = (Array.isArray(doc.pictures) ? doc.pictures : []).map((picture) => {
    adaptBase(picture, "DOC_ITEM_LABEL_");
    adaptAnnotations(picture);
    return picture;
  });
  doc.groups = (Array.isArray(doc.groups) ? doc.groups : []).map((group) => {
    adaptBase(group, "GROUP_LABEL_");
    return group;
  });
  doc.keyValueItems = (Array.isArray(doc.keyValueItems) ? doc.keyValueItems : []).map(adaptGraph);
  doc.formItems = (Array.isArray(doc.formItems) ? doc.formItems : []).map(adaptGraph);
  // The canonical schema name describes the file we came from, not the shape
  // the viewer now holds.
  delete doc.schema_name;
  return doc;
}

// Parses one uploaded JSON file. Anything unreadable reports itself instead
// of leaving an empty viewer behind.
function loadDocumentJson(text) {
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch (error) {
    banner("error", `That file is not valid JSON: ${error.message}`);
    return false;
  }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    banner("error", "That JSON file does not hold a document object.");
    return false;
  }
  const dialect = detectDialect(parsed);
  try {
    const document_ = dialect === "canonical" ? adaptCanonicalDocument(parsed) : parsed;
    loadedDialect = DIALECT_LABELS[dialect];
    buildViewer(document_);
  } catch (error) {
    loadedDialect = null;
    results.textContent = "";
    banner("error", `That JSON file could not be read as a document: ${error.message}`);
    return false;
  }
  return true;
}

function downloadFileName(doc) {
  const name = (doc && typeof doc.name === "string" && doc.name) || "document";
  const stem = name.replace(/\.[^.]*$/, "").replace(/[^\w.-]+/g, "-").replace(/^-+|-+$/g, "");
  return `${stem || "document"}.json`;
}

// Saves what the viewer currently holds, in the dialect it reads.
function downloadDocument() {
  if (!model) return null;
  const blob = new Blob([JSON.stringify(model.doc)], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const anchor = el("a");
  anchor.href = url;
  anchor.download = downloadFileName(model.doc);
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 0);
  return blob;
}

downloadButton.addEventListener("click", () => downloadDocument());

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
  clearSearch();
  loadedDialect = null;
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
    loadedDialect = DIALECT_LABELS.native;
    if (event.document && typeof event.document === "object") buildViewer(event.document);
    else banner("error", "The stream's document line carried no document.");
  } else if (event.type === "error") {
    banner("error", `Parse failed: ${event.message || "unknown error"}`);
  }
}

function isJsonUpload(file) {
  return /\.json$/i.test(file.name || "") || file.type === "application/json";
}

// JSON files never reach the service: they are already documents.
async function loadJsonFile(file) {
  resetRun();
  dropzone.classList.add("busy");
  try {
    loadDocumentJson(await file.text());
  } catch (error) {
    banner("error", `Could not read that file: ${error.message}`);
  } finally {
    finishRun();
  }
}

async function parseFile(file) {
  if (isJsonUpload(file)) return loadJsonFile(file);
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
// Every sample button fetches its own bundled file and feeds it through the
// same upload path a picked or dropped file takes; no parsing logic here.
sampleButtons.forEach((button) => {
  button.addEventListener("click", async (event) => {
    event.stopPropagation(); // the surrounding dropzone opens the file picker on click
    const name = button.dataset.sampleFile;
    const contentType = button.dataset.sampleContentType;
    const response = await fetch(name);
    if (!response.ok) {
      banner("error", `Could not fetch the bundled sample: HTTP ${response.status}`);
      return;
    }
    const bytes = await response.blob();
    parseFile(new File([bytes], name, { type: contentType }));
  });
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
