// Folds grPOIc's batched worksheets back into one preview per sheet.
//
// The relay asks grPOIc for sheet batches (ParseRequestChunk.sheet_batches),
// because a worksheet sent as one Sheet event is refused once that message
// would pass 256 MiB. A batched sheet arrives as consecutive Sheet events
// with the same index and name, every batch but the last setting more_rows.
// The page shows one line per worksheet, so this folder counts the rows of
// each batch and keeps the first row's text, and hands back a preview only
// when the sheet's last batch arrives. Rows are never held past the batch
// that carried them, so the relay's memory does not grow with the sheet.
"use strict";

function firstRowText(rows) {
  return rows.length > 0 ? (rows[0].cells || []).map((cell) => cell.formatted).join(" | ") : "";
}

function startFold(sheet) {
  return {
    index: sheet.index,
    name: sheet.name,
    hidden: false,
    rows: 0,
    batches: 0,
    firstRow: undefined,
  };
}

// cap shortens the first row's text to the page's preview length.
function createSheetFolder(cap) {
  let pending = null;

  const preview = (fold, incomplete) => ({
    kind: "sheet",
    index: fold.index,
    name: fold.name,
    rows: fold.rows,
    batches: fold.batches,
    ...(fold.hidden ? { hidden: true } : {}),
    ...(incomplete ? { incomplete: true } : {}),
    text: cap(fold.firstRow || ""),
  });

  // The sheet still waiting for its last batch, as an incomplete preview,
  // or null when none is. Called when the stream moves on to anything that
  // is not that sheet's next batch, and when the stream ends.
  const flush = () => {
    if (pending === null) return null;
    const value = preview(pending, true);
    pending = null;
    return value;
  };

  // Takes one Sheet event and returns the previews it completes, in order:
  // empty while the sheet has more batches, one preview for its last batch,
  // and an incomplete preview first when a different sheet interrupted one
  // that had promised more rows.
  const add = (sheet) => {
    const out = [];
    if (pending !== null && pending.index !== sheet.index) out.push(flush());
    if (pending === null) pending = startFold(sheet);
    const rows = sheet.rows || [];
    pending.hidden = pending.hidden || Boolean(sheet.hidden);
    if (pending.firstRow === undefined && rows.length > 0) pending.firstRow = firstRowText(rows);
    pending.rows += rows.length;
    pending.batches += 1;
    if (!sheet.moreRows) {
      out.push(preview(pending, false));
      pending = null;
    }
    return out;
  };

  return { add, flush };
}

module.exports = { createSheetFolder };
