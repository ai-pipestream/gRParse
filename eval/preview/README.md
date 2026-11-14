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
documents already built; `--force` rebuilds them. `--rescore` redoes the box
comparison for documents already in `--out`, with no new conversions, and
refreshes the viewer files.

## Reading the viewer

- Each pane draws its side's boxes over its side's own page raster. The
  sides measure in different units (gRParse's CV path in pixels, Docling in
  points, origin top-left or bottom-left); both scale to the image by the
  page size, so they line up without conversion.
- Provenance boxes of one item on one page are merged when they touch:
  on one line within 2% of the page width, or stacked in a column within
  1.5% of the page height. gRParse gives a text item one prov per word or
  line, so the words become the paragraph. Docling splits a paragraph that
  continues into the next column into separate provs, and those remain
  separate boxes.
- A formula is always one box per page: the union of all of its provenance
  boxes on that page. Display math stacks parts above and below one another,
  so the line and column rule would cut one equation into pieces.
- Items whose parent is a picture are left out, except captions: text found
  inside a figure (axis labels, legends) and the panel pictures of a figure
  belong to the figure, so only the figure itself and its captions are
  scored. gRParse hangs captions off the picture while Docling keeps them in
  the body, so captions count on both sides.
- Boxes are matched one to one per page, highest IoU first, at IoU 0.5 or
  more. A dashed box has no match on the other side. Hovering a box
  highlights its match and shows both labels, the IoU and the text.
- Agreement is boxes matched with the same label over all boxes on both
  sides. "Sort: least agreement first" is the default and puts the
  documents worth looking at on top; a document opened from it starts on
  the page with the most unmatched boxes. It is a pointer to where the two differ, not a quality score:
  Docling is not ground truth.
- "Table cells" (off by default) outlines every table cell each side reports
  on the page. A cell that shares its row and column slot with another is
  drawn in red. Hovering a cell shows its row, column and text. The overlay
  is for reading table structure; it is not matched or scored.
- Keys: `j`/`k` next and previous page, `n`/`p` next and previous document.

## Difference catalog

`diff_catalog.py` sorts every difference in a built preview into categories,
with counts, the papers each touches and example viewer links. It uses the
viewer's box rules but keeps text inside pictures, since that is a
difference too.

```sh
python3 eval/preview/diff_catalog.py /tmp/acm-preview --out catalog.json \
    --viewer-url http://localhost:8090
python3 eval/preview/diff_catalog.py --themes before=old.json after=catalog.json
```

`--themes` rolls the categories up into QA themes, one column per catalog,
so a fix shows up as one theme shrinking with no other theme growing. A
panel label that gRParse makes a caption of its panel while Docling keeps it
as text under the figure is counted in "7b Known structure", not as a label
disagreement. Nothing here changes the agreement score.

## Relation to gRParse#306

The service still returns page images inline as data URIs; this tool splits
them out at the edge. gRParse#306 moves the split into the service (image
references in page events, bytes from a separate call), and the viewer then
reads the same `image.uri` field.
