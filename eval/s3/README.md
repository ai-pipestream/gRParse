# S3 corpus eval: every object through gRParse, one shape battery per (parser type, file type)

`eval/s3/run.py` lists an S3-compatible bucket, fetches each object into
memory (the corpus stays in the bucket; no document bytes are written to
disk), sends it through gRParse's unary `ConvertSource` with the content
type the key's extension declares, sends a sample once more under a name
with no extension so the origin has to come from the bytes, parses each
object twice, and runs a battery of small named checks over the merged
`Document`. Results are grouped by file type (the extension) and by parser
type (which collectors' items ended up in the Document: `libreoffice`,
`pdf`, `grparse-cv`, `markup`, `epub+markup`, ...), and every failure is
kept with its key, its check and its evidence.

```sh
uv run --with boto3 --with grpcio --with grpcio-tools python eval/s3/run.py
uv run python eval/s3/tests/run_tests.py                 # the tool's own tests, no network
uv run --with boto3 python eval/s3/seed_from_workspace.py --dry-run   # what a seed would upload
```

Exit codes: `0` every check passed on every object; `1` any failure (or, with
`EVAL_REQUIRE=1`, any skipped object); `77` (the CTest skip code) when the
configuration is missing, the bucket or gRParse is unreachable, or the
selection is empty. A run never passes silently: an empty selection is a skip,
not a pass.

## Configuration (environment only)

| variable | meaning |
|---|---|
| `EVAL_S3_ENDPOINT` | any S3-compatible endpoint (`http://127.0.0.1:9000`); the fleet uses RustFS |
| `EVAL_S3_BUCKET`, `EVAL_S3_PREFIX` | the bucket and an optional key prefix |
| `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` or `EVAL_S3_ACCESS_KEY` / `EVAL_S3_SECRET_KEY` | credentials; never printed, never written into a report |
| `EVAL_S3_REGION` | default `us-east-1` |
| `GRPARSE_TARGET` | gRParse's gRPC, default `localhost:50051` |
| `EVAL_S3_MAX_OBJECTS` | cut the sorted selection at N objects |
| `EVAL_S3_INCLUDE`, `EVAL_S3_EXCLUDE` | comma-separated key globs (`*.pdf,gRParse/*`) |
| `EVAL_S3_REPEAT` | conversions per object, default 2 (the second is the byte-identity check) |
| `EVAL_S3_CONVERT_TIMEOUT` | client deadline per conversion in seconds, default 600; a parse that runs past it (or uses most of it) is that object's failure and is not repeated |
| `EVAL_S3_SNIFF_PER_EXTENSION` | objects per extension parsed once more under an extension-less name, default 1 |
| `EVAL_OUT`, `EVAL_LABEL` | output root (default `eval/out`) and run label (default `live`) |
| `EVAL_REQUIRE` | `1` makes a skipped object (an `.ebc` without a layout) a failure |

Outputs land in `EVAL_OUT/s3/<label>/report.md` and `report.json`: a per-check
table, the (parser type x file type) matrix with files/pass/fail per check,
the findings (failures grouped by check and cause with the keys, the
collectors involved and the first object's evidence), one row per object,
and the skipped objects with their reason.

## Setup: a private RustFS and a seeded bucket

The fleet's container of record is `rustfs/rustfs:1.0.0-beta.11-preview.1`
(credentials `RUSTFS_ACCESS_KEY` / `RUSTFS_SECRET_KEY`, data under `/data`,
S3 on 9000). Either run it by hand on the stack network or use the compose
overlay, whose credentials come from the environment with no default
committed:

```sh
RUSTFS_ACCESS_KEY=<choose> RUSTFS_SECRET_KEY=<choose> \
  docker compose -f compose.stack.yaml -f compose.stack.s3.yaml --profile s3 up -d rustfs
```

Seed it from the sibling repositories' own fixtures (default directory list
in `seed_from_workspace.py`, or pass directories on the command line):

```sh
EVAL_S3_ENDPOINT=http://127.0.0.1:9000 EVAL_S3_ACCESS_KEY=... EVAL_S3_SECRET_KEY=... \
  uv run --with boto3 python eval/s3/seed_from_workspace.py --bucket grparse-eval
```

Keys are `<repo>/<path relative to the repo>`. The seed takes files with a
document extension (pdf docx doc xlsx xls pptx ppt odt ods odp rtf html htm
xml md eml mbox epub png jpg jpeg tif tiff warc warc.gz wav mp3 csv txt ebc)
plus the `<stem>.layout.json` companions an `.ebc` needs, and skips
`node_modules`, `target`, `build*`, `vendor`, `.git`, `_deps`, virtualenvs,
`docs`, `models`, generated output, hidden files, README/AGENTS/CHANGELOG
and package or lock files. It prints the object count per extension and per
repository; it never reads outside the listed directories.

Then, with the stack's gRParse published on the host:

```sh
EVAL_S3_ENDPOINT=http://127.0.0.1:9000 EVAL_S3_ACCESS_KEY=... EVAL_S3_SECRET_KEY=... \
EVAL_S3_BUCKET=grparse-eval GRPARSE_TARGET=127.0.0.1:50051 EVAL_LABEL=verdict \
  uv run --with boto3 --with grpcio --with grpcio-tools python eval/s3/run.py
```

Run it twice after a deploy; the second run is the verdict (the first
carries warm-up latency and, with a fresh stack, the first-touch caches).

EBCDIC: raw records cannot be decoded without a layout, so an `.ebc` object
is parsed with `COLLECTOR_EBCDIC` and the `<stem>.layout.json` object
beside it; without one it is reported as skipped (a failure under
`EVAL_REQUIRE=1`).

## The battery

Each check is one function in `checks.py`, registered with a name, a one-line
rule and an applicability predicate; a check that does not apply to an object
is reported as `n/a`, never as a pass. `formats.py` holds the extension table
(a mirror of `src/content_sniff.cpp`), the file-type families and the known
collector names; `integrity.py` is a port of `docling_integrity_errors`;
`sourcefacts.py` reads what a check compares against from the object bytes
themselves (an EPUB's spine, a page's `<title>` and heading levels, a mail's
attachment parts, a deck's slide count, a docx's inline drawings, a
workbook's sheets, a CSV's grid).

| check | applies to | rule |
|---|---|---|
| `parse_succeeds` | every object | the unary parse returns a Document with `CONVERSION_STATUS_SUCCESS` and no RPC error |
| `integrity` | every Document | `docling_integrity_errors` is empty: references resolve, parents list their children, page-plane provenance names a 1-based page, comment/span/change/anchor targets exist |
| `placement` | every Document | every arena item is reachable from exactly one of `#/body` or `#/furniture` (through children, captions and footnotes), is listed once, and every captions/footnotes/references entry resolves |
| `custom_field_keys` | every Document | no `custom_fields` key contains `:` except the pinned `cell:?` |
| `warnings_typed` | every Document | collector warnings ride a typed slot, never a `collector_warnings:<name>` custom field |
| `claims_resolve` | every Document | every `claims[].source` and every `field_sources[].source` names a known collector, and each `field_sources[].field` exists on the message that lists it |
| `collector_sources` | every Document | every placed text, table, picture and form item carries a `CollectorSource` naming a known collector |
| `origin_mimetype` | known extensions | `origin.mimetype` agrees with the extension's declared type (aliases: `text/xml` for `.xml`, `application/gzip` for `.warc.gz`; any `text/*` for `.txt`) and carries `mimetype_evidence` |
| `sniff_route` | the sniff sample, minus formats whose bytes do not name them (OLE2 `doc`/`xls`/`ppt`/`msg`, compressed WARC) | under an extension-less name the object still parses (routed by its bytes), its origin type matches the extension's and its evidence is `magic` |
| `page_count` | paged families or any Document with pages | pages are numbered `1..N` with positive sizes, paged families have at least one, no provenance names a page the document lacks |
| `boxes_in_page` | paged | every provenance box lies inside its page (2% tolerance; zero-area placeholders exempt) |
| `provenance_present` | pdf, image, word, sheet, deck | every placed text, table and picture outside the notes layer has provenance with a 1-based page |
| `text_present` | word, deck, html, markdown, xml, email, epub, txt, pdf with a text layer | at least one non-empty text item in the body (an html or markdown source with no visible text is exempt) |
| `empty_text_items` | every Document | no whitespace-only text item is placed in the body |
| `table_grids` | Documents with tables | cells fit `num_rows x num_cols`, spans equal their offsets, no two cells overlap, a materialised `grid` is `num_rows` rows of `num_cols` cells |
| `sheet_tables` | sheet family | every `SHEET` group carries exactly one table, every non-empty source sheet has a group, a CSV's table matches the source grid, a label row over numeric rows is marked `column_header` |
| `slides` | deck family | at least one `SLIDE` group, one per source slide, in page order, the deck title exactly once and on the first slide |
| `docx_pictures` | word family | every placed picture has a page, follows an item on the same or an earlier page, pictures come in page order, a docx yields at least as many pictures as inline drawings |
| `headings` | html, markdown | at most one title; an html `<title>` becomes the title item once with the same text; the section-header level sequence equals the source's heading sequence |
| `email_shape` | email | a typed `EmailMeta` with a sender, the subject as `source_meta.title` or the title item, a non-empty body, one `attachments` entry per attachment part with a name, a media type and a resolving `item_ref` |
| `epub_spine` | epub | one `CHAPTER` group per XHTML spine item, in spine order, each with content |
| `ocr_text` | image family, pdf without a text layer | recognition yields text (or, for a picture-only raster, a picture) and every text box lies inside its page |
| `reading_order` | pdf, image, word, deck with pages | on single-column pages body order is monotone in (page, top), half a line of overlap allowed; captions, footnotes, furniture and notes exempt; multi-column pages skipped |
| `chart_composite` | Documents with pictures | a `CHART` picture carries exactly one bound table (its child, parented to it) and at most one caption; a raster chart the derender leg answered carries a non-empty `tabular_chart` table |
| `repeat_identical` | `EVAL_S3_REPEAT` >= 2 | a second parse yields the same Document leaf for leaf (the derendered chart title, the one descriptive leaf, masked) and the same canonical JSON |

Expectations the battery deliberately encodes differently from a first
reading of the schema, each verified against the fleet's output:

- Mail headers are the typed `EmailMeta` (and the subject the
  `source_meta.title`), not key-value items; attachments are
  `Document.attachments` entries whose `item_ref` points at the list item that
  names them, not nested Documents.
- Sheet tables carry `table_cells` without a materialised `grid`; the
  canonical renderer derives the grid from the flat cells, so `table_grids`
  checks a grid only when one is present.
- Slide groups are named by the slide title and ordered by arrival; the
  check uses the page numbers of their children.

## Adding a check

Add one function to `checks.py` under the `@check(name, doc, applies=...)`
decorator: `applies` decides which objects it runs on (default: any object
with a Document), the body returns a list of `Failure(check, cause,
evidence)` where `cause` is a short stable string (use `normalize_cause` to
strip item numbers so equal causes group into one finding) and `evidence`
holds the refs, snippets and numbers a reader needs. Add a row to the table
above, a passing and a failing case to `tests/test_checks.py`, and run
`uv run python eval/s3/tests/run_tests.py`. A fact the check needs from the
source bytes goes into `sourcefacts.py`.

## Findings

A failure has one of three owners: gRParse's merge, assembly, mapping, repair
or reading-order code (a failing C++ test in `tests/` plus the fix), a
collector (recorded in the run report's findings with the repo, the key and
the evidence; the check keeps it red), or the battery itself (the check is
corrected and the change says so). `owners.py` names the findings that stay
red on purpose so every report says whose they are:

| check | owner | what |
|---|---|---|
| `warnings_typed` | gRParse, schema follow-on | collector warnings ride `body.meta.custom_fields[collector_warnings:<name>]`; a typed `Document.warnings` extension is a fleet-wide schema sweep |
| `parse_succeeds` (WARC) | fastwarc-grpc | the stack leaves `GRPARSE_FASTWARC_TARGET` unset until the vendored `fastwarc.v1` dialect and the published image agree |
| `parse_succeeds` (`corners.xlsx`) | grpc-libreoffice | a sheet with cells at the far corners of the grid runs past the office core's per-document timeout |
| `table_grids` (`streaming-markup.html`, `html-spec.html`) | grpc-markup | a header cell spanning columns yields a ragged grid (the 15 MB page parses since the collector channels took the server's message limit; 23 of its tables are ragged) |

The gRParse-side findings the first runs produced are fixed and pinned by
the tests in `tests/` registered under `GRPARSE_S3_EVAL_TESTS` in
`CMakeLists.txt`: the merge attributing a Timestamp as `created.seconds`
(`document_merge_value_fields_test`), the integrity walk rejecting
line-addressed provenance and `#/pages/N` destinations
(`docling_integrity_destinations_test`), empty sheets folded as 1 x 1 tables
and label rows over numeric formulas left unmarked
(`docling_map_sheet_shape_test`), Writer pictures trailing the body
(`docling_map_trailing_pictures_test`), extension-less names never routed by
their bytes and `.txt` failing on the CV path (`collector_route_by_bytes_test`),
a `.csv` origin reading `text/plain` (`content_sniff_precedence_test`), a
failed leg reporting an empty status (`document_collectors_status_test`), and
collector channels capped at gRPC's 4 MB default while the server accepts
520 MB (`collector_channel_limits_test`).
Expectations the evidence corrected: mail headers are the typed `EmailMeta`,
sheet tables carry no materialised grid, a first-level heading or the source
title may be the title item, a blank scan yields no text, a markup source
with no visible text yields none, and OLE2 or compressed-WARC bytes cannot
name their own format.
