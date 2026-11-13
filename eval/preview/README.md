# Page preview: gRParse beside Docling

A static viewer that draws each side's boxes and labels over its own page
image, for a folder of PDFs. It answers "where do we and Docling disagree,
and who is right" one page at a time.

```sh
# 1. Build: gRParse (canonical JSON with page images) and Docling for every PDF.
uv run --with grpcio --with grpcio-tools python eval/preview/build.py \
    --pdf-dir /data/acm --out /tmp/acm-preview \
    --grparse localhost:50051 --docling-url http://localhost:5001 --every 10

# 2. Serve the folder and open it.
python3 -m http.server -d /tmp/acm-preview 8090
```

Docling comes from a running docling-serve (`--docling-url`, its
`/v1/convert/file` with `image_export_mode=embedded` and
`include_page_images=true`) or from a directory of
Docling JSON files that mirrors `--pdf-dir` (`--docling-json-dir`). Without
either, the viewer shows gRParse alone.

## What lands in the output folder

- `manifest.json`: every document with each side's status, timing, item
  counts by label, and a box agreement summary.
- `docs/<id>/ours.json`, `docs/<id>/docling.json`: the two documents in the
  docling-core dialect, with each page's image moved out to
  `docs/<id>/pages/<side>-<page>.png` and the page's `image.uri` rewritten to
  that relative path. Picture and table crops are dropped from the JSON.
- `index.html`, `viewer.js`, `viewer.css`: the viewer, copied from
  `eval/preview/viewer/`.

`<id>` is the first 16 hex digits of the PDF's SHA-256. A rerun skips
documents already built; `--force` rebuilds them.

## Reading the viewer

- Each pane draws its side's boxes over its side's own page raster. The
  sides measure in different units (gRParse's CV path in pixels, Docling in
  points, origin top-left or bottom-left); both scale to the image by the
  page size, so they line up without conversion.
- Each item is one box per page: the union of its provenance boxes there.
  gRParse gives a text item one prov per word or line, Docling one per item.
- Boxes are matched one to one per page, highest IoU first, at IoU 0.5 or
  more. A dashed box has no match on the other side. Hovering a box
  highlights its match and shows both labels, the IoU and the text.
- Agreement is boxes matched with the same label over all boxes on both
  sides. "Sort: least agreement first" puts the documents worth looking at
  on top. It is a pointer to where the two differ, not a quality score:
  Docling is not ground truth.
- Keys: `j`/`k` next and previous page, `n`/`p` next and previous document.

## Relation to gRParse#306

The service still returns page images inline as data URIs; this tool splits
them out at the edge. gRParse#306 moves the split into the service (image
references in page events, bytes from a separate call), and the viewer then
reads the same `image.uri` field.
