# Review: document model, assembly, docling compat, and concurrency rework

Date: 2026-08-25
Scope: commits `2e53752..e683706` on `main` (~24 commits, ~3,100 added lines across 45 files)
Reviewed areas: `document.proto` / `parse_stream.proto` / `parse_types.proto`,
`src/document_assembly.cpp`, `src/layout_decode.cpp`, `src/docling_map.cpp`,
`src/render/*`, `src/call_executor.cpp`, `src/document_parser_service.cpp`,
plus their tests.

This document is the input for the next implementation pass. Each finding cites
code and, where useful, a suggested fix direction. Fix order is proposed at the
end.

## 1. Overall assessment

The "own model, docling-compatible" direction is executing correctly. All
divergence from docling so far is **additive**: no docling field has changed
meaning, and the extension seams are the right ones:

- `CollectorSource` (`document.proto:183-203`) carries per-item
  collector/model/version/confidence, survives the wire and scatter-gather
  merge, and is deliberately dropped from canonical JSON
  (`canonical_json_renderer.cpp:314-337`) so docling consumers never see it.
  This is the template for all future own-model per-item data.
- `custom_fields` (`= 100` on every meta message, fields 6–99 deliberately
  left free) plus the `pipestream__` rekeying (`renderer_base.cpp:291-332`) is
  the one channel that carries own-model data *through* the canonical dialect
  itself.
- `*_raw` string fallbacks on every growable enum (label, code language,
  coord origin, orientation, script) give forward-compat: renderers collapse
  raw-only states to the docling default and never leak the raw string.
- The canonical JSON renderer is byte-compatible with docling's
  import-then-dump pipeline, guarded by `scripts/validate_canonical_json.py`
  against the real bindings.
- The layout postprocessor port (confidence thresholds, title→section_header
  remap in `layout_decode.cpp`) is an exact match to docling's
  `LayoutPostprocessor`.
- The concurrency rework (CallExecutor + CallbackService) has no deadlock:
  executor workers block only on scheduler-owned threads and deadlined
  outbound collector calls, never on server event threads. Tests prove
  off-thread execution, queue-full refusal, drain-on-shutdown, worker survival
  after a throwing task, and 4-way concurrent unary parses.

## 2. Blocker

### B1. Ordered-list groups break parity with docling in canonical JSON and markdown

`load_normalization.cpp:285-291` (`is_list_group_ref`) accepts only
`GROUP_LABEL_LIST`, so list items parented to a `GROUP_LABEL_ORDERED_LIST`
group are classed as "misplaced" and migrated into a synthesized list group
with arena renumbering (`load_normalization.cpp:559-664`); the group itself
exports as `"label": "ordered_list"` (`canonical_json_renderer.cpp:100`).

The reference model never behaves this way: `ListGroup.patch_ordered` rewrites
`ordered_list`→`list` at load (docling-core `items/group.py:30-35`), so those
items sit inside a `ListGroup` and are not misplaced
(`document.py:5279`). Verified empirically against the fork checkout
(`/work/worktrees/docling-core`): the bridge emits `"label": "list"` with no
migration, and reference markdown is `- alpha\n    - one\n    - two` (4-space
nesting) while the native path produces a synthesized extra nesting level
(8-space) — which is exactly what `tests/document_render_test.cpp:240-244`
asserts while claiming it is "the reference rendering of the same document,
taken from the validation harness". That claim is stale (predates fork commit
`e721e4d` "import ordered-list groups the way the JSON union does").

Consequences: `validate_canonical_json.py` fails tree-equality and
`validate_markdown.py` fails byte-equality on any document with an
ordered-list group. gRParse's own mapper never emits `ordered_list` groups,
but the grpc-xml / grpc-markup collectors can.

Fix direction: treat `GROUP_LABEL_ORDERED_LIST` as a list group in
`is_list_group_ref`, mirror the `ordered_list`→`list` label rewrite at load
normalization, and correct the stale expectation in
`document_render_test.cpp:240-244`. Add an ordered-list-group case to
`canonical_json_test.cpp`.

## 3. Concerns

### C1. `merge_documents` silently drops the field arenas

`src/document_merge.cpp:109-146` maps and appends
`groups/texts/pictures/tables/key_value_items/form_items` but never touches
`Document.field_regions` (`document.proto:71`) or `field_items`
(`document.proto:74`). A collector emitting form-field items loses them in a
scatter-gather merge, and refs like `#/field_regions/0` in merged children
dangle. This contradicts the merge doctrine stated on `CollectorSource`
("additive merges never collide silently", `document.proto:181`). Latent today
— no producer emits these arenas — but it becomes a blocker the day one does.
`document_merge_test.cpp` has two scenarios, neither covers the field arenas.

Fix direction: add both arenas to the merge's map/append pass, or derive the
arena list by reflection so new arenas cannot be forgotten.

### C2. Figure classifications and barcodes never survive canonical export

Assembly writes classifier output and zxing barcodes to
`PictureItem.annotations` (`document_assembly.cpp:432-451`), but the canonical
renderer always dumps `annotations: []`, declaring the wire list "a projection
of meta … ignored" (`canonical_json_renderer.cpp:861-865`). Meanwhile
`PictureMeta.classification` — which the renderer *does* export
(`canonical_json_renderer.cpp:494-509`) — is never populated by assembly.
Net effect: the 26-class figure classifier and barcode output are invisible to
every docling-dialect consumer.

Fix direction: either route classifier output into `meta.classification`, or
document the loss as deliberate. Note the two renderers also disagree on
classification tie-breaking (`renderer_base.cpp:373-394` treats missing
confidence as 0.0; `markdown_renderer.cpp:1700-1715` skips confidence-less
predictions) — pick one rule.

### C3. list/code/formula labels are emitted on the wrong oneof arm

`BaseTextItem` has dedicated `list_item`, `code`, `formula` arms
(`document.proto:590-599`), and the test helper already handles them
(`tests/document_assembly_test.cpp:32-43`), but `emit_block` only ever selects
title/section_header/text arms (`document_assembly.cpp:489-497`). A
`DOC_ITEM_LABEL_LIST_ITEM` arrives as a `TextItem`, never a `ListItem` — so
`marker`/`enumerated` can never be populated without a breaking re-arm, and a
docling round-trip (which constructs `ListItem`/`CodeItem`/`FormulaItem`
objects for these labels) changes item types. `CodeItem` additionally cannot
carry `code_language` on the text arm. This is the largest docling-compat
break in the new assembly code.

Fix direction: extend `emit_block`'s arm selection to the
list_item/code/formula labels. This is also the extension path for markers,
code language, and formula TeX.

### C4. Streaming surface never assigns heading levels

`assign_section_header_levels` clusters per-item line heights across the whole
document and is called only from the unary path
(`document_parser_service.cpp:556-558`). The bidi stream emits pages
incrementally with `level` unset (`document_parser_service.cpp:1355`), and
`DocumentComplete` (`parse_stream.proto:139-144`) carries no fix-up.
Renderers paper over it (`heading_rank(0) → 1`, `renderer_base.h:47-50`), but
the two wire surfaces now disagree on the same document: unary yields levels
1–6, streaming yields 0, which is outside docling's 1..6 semantics.

Fix direction: this is inherent to streaming (height clustering needs the
whole document), so at minimum disclose the divergence in
`parse_stream.proto`. Optionally run a buffering post-pass on the stream, or
emit levels in `DocumentComplete` as a fix-up list.

### C5. Multi-column anchor fallback places text-less floats mid-column

`region_anchor`'s geometric fallback (`document_assembly.cpp:193-197`) returns
the first block whose first line's `top >= region.top`, implicitly assuming
block order is monotone in `top`. It is not: `reading_order` is column-major
(region units emit all of column 1 before column 2). A picture or table with
no bound lines in the right column anchors after whichever left-column block
first exceeds its top — somewhere mid-left-column — instead of near its
column position. Floats that own lines anchor correctly via the first loop;
only text-less floats hit the fallback. No test exercises the fallback at all.

Fix direction: make the fallback column-aware (nearest block by geometry, not
first-in-order), and add a text-less-float multi-column test.

### C6. No cancellation/deadline propagation across the executor boundary

`run_remote_collector` (`document_parser_service.cpp:179-241`) builds a fresh
`ClientContext` per collector with a static 5/30/10-minute deadline and never
looks at the inbound context. A client that cancels (or whose deadline passed
while the call sat in the executor queue) still occupies its worker until
every remote leg answers — up to 30 min for ASR. The streaming side documents
this trade-off (`:1293-1296`); the unary side does not, and it is the unary
side that now multiplexes all calls onto 16 workers
(`GRPARSE_UNARY_WORKERS`, `src/main.cpp:551-553`).

Related: `parse_source` (`:446-460`) does no `IsCancelled()` pre-check after
dequeue — it base64-decodes up to ~390 MiB and dials collectors even if the
call died while queued.

Related: shutdown drain is unbounded in wall time. The 10 s server `Shutdown`
deadline (`src/main.cpp:622`) force-cancels inbound calls, but the executor
drain in the service destructor joins workers that may still be inside a
5–30 min collector deadline. SIGTERM can hold the process for many minutes —
past any orchestrator grace period.

Fix direction: propagate the inbound context's cancellation/deadline to
collector `ClientContext`s (or cap outbound contexts by a shutdown deadline),
add a cheap `IsCancelled()` check after dequeue, and document the drain bound.

### C7. `body_order` — the headline streaming addition — has no test or consumer

`PageData.body_order` (`parse_stream.proto:113-119`) was added by 101a485 to
give streaming consumers the same reading order as unary. No test asserts it,
no in-repo consumer reads it. A regression that empties or reorders it passes
the suite. Also the proto comment says refs reference "entries of
texts/tables/pictures above"; actually they are document-global JSON pointers
(`#/texts/N`) from a cursor spanning the whole stream
(`document_parser_service.cpp:1491`, `document_assembly.cpp:484`) — clients
must stitch across pages. Fix the comment; add a test that pins ordering.

### C8. Schema-version stamp is hardcoded twice, with no drift detection

`"1.10.0"` appears at `docling_map.cpp:24` and `canonical_json_renderer.cpp:43`;
the bridge uses the bindings' `CURRENT_VERSION` instead
(`document_json_bridge.py:213`). The native renderer never inspects the
input's `document.version()` — it claims 1.10.0 for any input. Nothing in CI
cross-checks against the fork (`ci.yml` runs in-image ctest only; the oracle
scripts require the docling-core checkout and are dev-only). A fork bump to
1.11.0 drifts silently. The load-normalization mirror is also hand-maintained
against upstream `model_validator`s — finding B1 is exactly this failure mode:
upstream semantics changed, the C++ mirror and its test expectations didn't,
and nothing failed.

Fix direction: single source for the version constant (generated header or
CMake-configured constant), plus a CI step that runs
`validate_canonical_json.py --require-bytes` against fixtures.

### C9. `docling_integrity_errors` ignores four arenas

`docling_map.cpp:1641-1670` validates only groups/texts/pictures/tables.
`key_value_items`, `form_items`, `field_regions`, `field_items` self_refs,
parent links, and graph-cell `item_ref`s are never validated, even though
merge and the renderers handle those arenas.

### C10. Signed-overflow UB on adversarial table dimensions

`fold_table` computes `table.rows() * table.columns() <= kMaxGridCells` in
`int` (`docling_map.cpp:484-485`); both are int32 wire fields, so a hostile
stream with e.g. rows=50000, columns=50000 overflows before the comparison.
Widen to int64 before multiplying.

### C11. Unresolvable provenance is stamped as if valid

When `page_local=false` but the page rect is unknown (DocumentInfo
missing/incomplete), `add_prov` skips the origin subtraction yet still emits
the box as page-local TOPLEFT with `page_no = index+1`
(`docling_map.cpp:353-369`) — a document-absolute box labeled page-local,
possibly pointing at a page absent from `pages`. Documented in
`docling_map.h:95-98`, but consumers cannot distinguish it. Same for the
zero-area `(0,0,0,0)` boxes stamped for sheets/charts/pivots
(`docling_map.cpp:1250, 1359, 1393`). Also: nothing validates
`ProvenanceItem.page_no >= 1`; proto3 int32 defaults to 0 and the dialect's
pages are 1-based.

### C12. Overlapping structured cells duplicate line text

`table_claimed_lines` stops at the first cell containing a line's center
(`document_assembly.cpp:130-136`), but `fill_structured_table_data` appends
the line's text to *every* cell containing it
(`document_assembly.cpp:263-270`). Two overlapping model boxes → same text in
both cells, while the de-dup logic suggests single ownership was intended.
Docling's cell matcher assigns each text cell to one best cell.

## 4. Smaller items

- **`label_raw` forward-compat is half-built.** Only the four floating/graph
  items carry `label_raw` (`document.proto:888, 985, 1038, 1098`);
  `TextItemBase` (603-617), `CodeItem` (659-680), and `GroupItem` (122-130)
  lack it — yet text labels are the vocabulary docling extends most, and the
  renderer already collapses unknown text labels to `"text"`, discarding the
  raw value (`canonical_json_renderer.cpp:807-822`).
- **No `reserved` ranges** in the three core protos (only office_service.proto
  and the lolhtml collector protos have them). Nothing guards against future
  field-removal mistakes.
- **Stale header comment.** `document.proto:13-16` still claims the schema
  "mirrors the DoclingDocument v2 JSON schema field for field" while the same
  file documents acknowledged extensions. Say "mirror plus extensions".
- **`schema_name` interop comment overstates the wire field.** Producers set
  `"docling_document_v2"` (`document_parser_service.cpp:464`,
  `docling_map.cpp:23`) but docling's actual value is `"DoclingDocument"` —
  which is what the renderer emits while deliberately ignoring the wire field
  (`canonical_json_renderer.cpp:38-43`). Interop is carried by the renderer
  override; the comment should say so.
- **"body-text stream" includes furniture.** `Chunk.start_offset/end_offset`
  claim to locate chunks in "the document's concatenated body-text stream"
  (`parse_types.proto:659-664`, `chunker.h:50`), but assembly folds furniture
  items into `plain_text` and the offset table too
  (`document_assembly.cpp:518-523, 636-638`). Internally consistent (the
  chunker walks only `#/body`, `chunker.cpp:234`), but the documented contract
  says "body" and the stream is "all text".
- **Caption overlap test is asymmetric.** The 30% horizontal threshold is
  measured against caption width only (`document_assembly.cpp:578`): a narrow
  float centered under a full-width caption band fails the gate even when
  fully covered. Consider `max(overlap/caption_w, overlap/float_w)`.
- **Confidence misattribution on structured tables.** The `slanet-plus`
  collector source carries `region.confidence`
  (`document_assembly.cpp:415-416`), which is the layout detector's confidence
  in the table region, not the structure model's. Same for "geometry".
- **Divergence from the docling postprocessor beyond thresholds.** The
  reference also drops empty regular clusters (except formula), drops
  >90%-page-area pictures, and does overlap resolution between clusters;
  `decode_query_detector` does none of that (`layout_decode.cpp:93-125`).
  Worth a comment stating which postprocessing steps are deliberately skipped.
- **Dead defensive code.** `std::max(level, 1)` (`document_assembly.cpp:699`)
  can never engage: the first (tallest) entry always satisfies
  `height < 0.85 * infinity`.
- **Unknown-label policies disagree silently.** `label_for_region` maps any
  unknown string to TEXT with no log or metric
  (`document_assembly.cpp:86-87`), while `decode_query_detector` drops unknown
  class ids entirely (`layout_decode.cpp:105`). A new model label degrades
  invisibly, differently, in two places.
- **Stale label comment.** `include/grparse/ocr_types.h:54` still documents
  `label` as "text, title, list, table, figure" — `figure` was renamed to
  `picture` in 0c6467c and the 17-label set isn't mentioned.
- **Thread fan-out outside the executor's bound.** `run_collectors` spawns one
  `std::async(launch::async)` thread per collector per parse
  (`collector_coordinator.cpp:43-47`), and the streaming reactor spawns a
  detached thread per collector leg (`document_parser_service.cpp:1227, 1281,
  1312`). A request selecting all ~10 collectors multiplies threads per call;
  the "workers bound concurrent conversions" story doesn't cover this.
- **Layout-session thread-split reasoning is off by a factor of the worker
  count.** The comment at `src/main.cpp:422-426` exempts the shared layout
  session because "there is only one of it", but that one session is `Run`
  concurrently by all inference workers, each run spawning an all-cores
  intra-op pool — the same oversubscription 140e884 fixed for pooled sessions.
- **Version string lies about the build flavor.** `Health`/`GetServiceInfo`
  hardcode `"grparse-0.1.0-cuda"` (`document_parser_service.cpp:905, 914`) in
  every image, including the OpenVINO/CPU builds.
- **Unary/streaming size limits are asymmetric.** Streaming enforces a 500 MiB
  *binary* limit (`document_parser_service.cpp:924, 1028-1031`); unary is
  bounded by the 520 MB max receive size (`src/main.cpp:562`) on *base64*,
  i.e. ~390 MiB binary. A document legal on the stream is rejected on the
  unary surface by transport, not by a named error.
- **`mimetype_for` defaults unknown extensions to `image/png`**
  (`document_parser_service.cpp:94`) rather than `application/octet-stream`;
  only PDF is content-sniffed.
- **`GRPC_ARG_MAX_CONCURRENT_STREAMS=32`** (`src/main.cpp:567-568`) is
  per-connection, so one client connection caps at 32 concurrent RPCs while
  the executor admits 80; worth a comment that this is deliberate.
- **Docling-interop divergence by policy:** `validate_options`
  (`document_parser_service.cpp:354-383`) rejects any populated but
  unimplemented option where docling's REST API silently accepts several.
  Fail-loud is defensible and the message is good, but it is a behavioral
  break for docling clients that send `document_timeout` routinely — flag it
  in interop docs.
- **`list_level` wire contract is a proto3 footgun:** default 0 means
  "top-level list item" and -1 means "not a list"
  (`office_service.proto:690-692`), so a producer that forgets the field
  silently emits list items (`docling_map.cpp:716`).
- **`emit_level_member` passes negative wire levels straight through**
  (`canonical_json_renderer.cpp:746-749`); the model constrains
  `LevelNumber >= 1`, so hostile input produces canonical JSON the model
  refuses to load.
- **Degradation of own-only concepts is inconsistent across renderers:**
  doctags silently drops form/field refs (`doctags_renderer.cpp:161-166`),
  doclang emits `<!-- form item omitted -->` (`doclang_renderer.cpp:67-74`),
  HTML emits `<!-- missing-form-item -->`. No shared policy.
- **Two live copies of the mapper.** `docling_map.{h,cpp}` are "ported from
  grpc-libreoffice ... keep in sync" copies (`docling_map.h:1-2`) — 1,700
  lines with no sync mechanism.
- **Round-trip loss surface is only comment-documented.** Canonical JSON
  irreversibly drops collector sources, annotations, `*_raw` strings, the wire
  `grid`, and rewrites custom-field names. Each loss is documented at its site
  but there is no consolidated docling-compat page under `docs/`.

## 5. Test gaps

- Ordered-list-group case missing from `canonical_json_test.cpp` (B1);
  no `form_items` or field-arena clamping cases either.
- `document_merge_test.cpp`: no field-arena scenario (C1).
- `body_order`: never asserted anywhere (C7).
- Caption edge cases untested: caption *above* a float; caption overlapping a
  float (gap 0); two floats at equal gap (tie → first-emitted wins);
  degenerate caption box; two captions claiming one float; caption in reach of
  a float on a different page (correctly impossible, but unpinned).
- `region_anchor` fallback: no text-less-float test, no multi-column float
  placement test (C5).
- Unknown region label → TEXT fallback: untested.
- Heading levels: no saturation test (>6 clusters), no mixed-scale pages, no
  test that streamed headers stay level 0 (pin the divergence either way, C4).
- `verify_section_header_levels` (document_assembly_test.cpp:499-524) only
  covers 2 distinct heights; the infinity-founding first iteration works by
  accident and deserves a two-heading test (40px vs 20px → levels 1 and 2).
- Service-level executor tests missing: (a) server shutdown *while* a unary
  parse is blocked on the executor (drain is only proven at pool level);
  (b) the queue-full → `RESOURCE_EXHAUSTED` path at
  `document_parser_service.cpp:762-766` (the concurrency test never saturates).
- `docling_map_test.cpp` exercises roughly half the `on_*` handlers — slides,
  slide shapes, draw shapes, embedded objects/charts, sheet charts/pivots,
  comments, tracked changes, bookmarks, form fields, page styles, named ranges
  are all untested.
- No oracle harness for doctags, doclang, HTML, split-page, or VTT renderers —
  only hand-copied expected strings. Only canonical JSON and markdown have
  reference oracles, and neither runs in CI.
- `validate_canonical_json.py:15-20` overstates the byte-diff class: the fork
  now sorts custom fields on import (commit `8ebefaa`), so top-level custom
  fields byte-match; only nested multi-key `Struct` payloads can still differ.

## 6. Guidance for growing the own model

What helps (build on these):

- **`CollectorSource` is the provenance seam, already proven.** Per-item
  collector/model/version/confidence rides the wire and protobuf-JSON but is
  dropped from canonical JSON, so own attribution never disturbs docling
  consumers.
- **`custom_fields` + `pipestream__` rekeying** is the only channel that
  carries own-model data *through* the canonical dialect. Anything that must
  reach docling-side consumers goes here, not in annotations.
- **`*_raw` fallbacks** let own-vocabulary enum values degrade safely for
  docling consumers while staying intact on the wire.
- **`body_order` on PageData** is a genuinely better-than-docling transport
  (docling's streaming story is weak) — it needs a consumer and a test.
- **Per-line provenance inside merged items** (one prov entry per member line
  with own charspan) exceeds docling's per-item provenance and is the right
  substrate for line-level extensions (scripts, fonts, handwriting flags).
- **`ContentLayer`** already has `NOTES`/`BACKGROUND`/`INVISIBLE` values in
  the enum, ready for footnote-as-notes or watermark-as-background. The
  centralized layer filter (`RendererBase::excluded_layer`) means a new
  own-only layer is excluded from all exports by changing nothing.
- **`OcrTuning::ocr_pages`** (page_scheduler.h:72-79) is already a
  beyond-docling primitive that could back the currently-rejected
  `page_range` option.
- **The executor's reject-on-saturation seam** is the natural implementation
  point for the unimplemented async RPCs (`ConvertSourceAsync` etc.,
  parse.proto:31-59): submit-with-ticket instead of
  submit-or-RESOURCE_EXHAUSTED. Note `CallExecutor` has no task-handle/cancel
  API, so `PollTaskStatus`/`ClearResults` would need it extended.

What hinders (fix before the model grows):

- **The annotations-vs-meta asymmetry is the main trap.** Own data in
  `annotations` dies at canonical export; own data in `meta` survives. Nothing
  in the proto comments warns about this. Choose placement deliberately per
  concept and document the rule.
- **A new own-only item kind currently means touching**: proto, assembly,
  `parse_ref` (`renderer_base.cpp:16-40`), `docling_integrity_errors`,
  `merge_documents`, `load_normalization`'s `node_fields`, and every renderer
  — and each renderer invents its own fallback (silent drop vs. placeholder
  comment). A registry mapping "own-only kind → docling-safe rendering", plus
  a default plain-text fallback, would make new kinds additive instead of
  lockstep.
- **The label vocabulary is a string→enum switchboard in three places**:
  `layout_labels` (`layout_decode.cpp:61-71`), `label_for_region`
  (`document_assembly.cpp:67-88`), `is_furniture_region`
  (`document_assembly.cpp:94-96`). Every new label or layer rule means editing
  all three, and the silent TEXT fallback means forgetting one is invisible. A
  single label table (string, enum, aggregates?, furniture?, float?) would
  make extension additive.
- **Caption claiming is hardcoded** to table/picture floats with magic
  constants (0.3, 1.5). Docling lets any `FloatingItem` carry captions;
  extending to captioned code blocks or formulas requires surgery in the
  emission loop, not configuration.
- **Heading-depth inference consumes only bbox heights.** Extending to font
  size/weight from the digital-text layer needs the interface widened —
  `assign_section_header_levels` only sees the assembled Document, where
  digital font metadata is already lost. And because clustering is
  document-global, any streaming-native heading-level feature needs a
  different algorithm by construction.
- **The offset-table gating (`document_parser_service.cpp:668-674`) is the
  main structural limitation.** Offsets are published only when the CV
  collector is the entire document because `merge_documents` renumbers arena
  refs. Any beyond-docling per-item provenance (offsets, confidence,
  page-mapping) dies the moment two collectors merge. Fixing merge to carry
  provenance through renumbering would unlock provenance-rich multi-collector
  documents.
- **Coordinate/provenance is page-number-keyed with no notes/appendix space**
  — e.g. slide-notes geometry is deliberately dropped
  (`docling_map.cpp:905-907`) because it has no page. Extending provenance
  beyond `{page_no, bbox, charspan}` (regions, polygons, notes space) has no
  current home.

## 7. Proposed fix order

1. **B1** ordered-list parity (blocker; also fix the stale test expectation).
2. **C3** oneof-arm emission for list/code/formula (docling-compat break, and
   it is the extension path for markers/code-language/formula-TeX).
3. **C1** merge field-arena omission (latent blocker for form/KV collectors).
4. **C2** route classifier/barcode output through `meta` (or document the
   loss) so the 26-class classifier is visible to docling consumers.
5. **C7** `body_order` test + proto comment fix (headline feature, currently
   unpinned).
6. **C4** heading-level divergence: disclose in parse_stream.proto at minimum.
7. **C5** multi-column anchor fallback + test.
8. **C6** cancellation propagation + post-dequeue `IsCancelled` check +
   document the shutdown drain bound.
9. **C8** single version constant + CI oracle step.
10. C9–C12 and the section-4 items as cleanup; add `reserved` ranges and the
    consolidated docling-compat doc while there.


---

# Follow-up: verification of the fix batch (2026-08-25)

The fixes landed in `e683706..fb6a94c`. Verified against the code, not the
commit messages.

## Resolution of the original findings

- **B1 FIXED** (`be9fd51`): `relabel_ordered_list_groups`
  (`src/render/load_normalization.cpp:692-704`) mirrors
  `ListGroup.patch_ordered` exactly; the stale `document_render_test.cpp`
  expectation was corrected. Residual: the canonical-JSON path of the relabel
  has no direct unit test (only markdown exercises it).
- **C1 FIXED** (`e3d5c44`): all 8 arenas merge, including `field_regions` /
  `field_items`; reflection-based `rewrite_refs` covers future ref fields.
- **C2 FIXED** (`e3d5c44`, `d939553`): classifier output routes to
  `meta.classification`; barcodes ride a typed arm plus a
  `pipestream__barcodes` custom field; the classification tie-break rule is
  unified (but now exists as two verbatim copies —
  `renderer_base.cpp:373-387` and `markdown_renderer.cpp:1700-1715`).
- **C3 FIXED** (`e3d5c44`): `emit_block` re-arms list_item/code/formula.
  Marker/enumerated population remains future work.
- **C4 FIXED** (`5bbd4a5`): `DocumentComplete.section_header_levels` carries
  the clustered fix-up; the proto discloses in-stream level 0. No streaming
  test pins the map or the level-0 behavior.
- **C5 FIXED** (`5bbd4a5`): the anchor fallback requires horizontal overlap
  with the region's column; two-column test added.
- **C6 FIXED** (`222bd93`): inbound deadlines cap every collector leg
  (`capped_collector_deadline`); post-dequeue `IsCancelled` check added.
  Mid-flight explicit cancel without a deadline still does not interrupt an
  in-flight leg — gRPC offers no more than deadline propagation here.
- **C7 FIXED** (`d939553`, `5bbd4a5`): proto comment corrected; `body_order`
  ordering test added.
- **C8 PARTIAL** (`7e58577`): the version/schema-name literals collapsed into
  `include/grparse/schema_version.h`. Still missing: a CI step running the
  oracle harness (`validate_canonical_json.py`) against the reference
  checkout, so upstream drift remains silent in CI.
- **C9 FIXED** (`7364396`): all four form arenas validated, including
  graph-cell `item_ref`s.
- **C10 FIXED** (`5bbd4a5`): 64-bit table-dimension product.
- **C11 FIXED** (`7364396`, `7e58577`): unresolved pages warn once and keep
  the box; `page_no < 1` is an integrity error, with page-less arms
  (time/byte-range/grid) correctly exempt.
- **C12 FIXED** (`d939553`): single-ownership cell text via `line_taken`.

Section-4 smaller items: the comment/doc fixes mostly landed (`label_raw` on
the remaining metas, stale header/schema_name/body-order/ocr_types comments,
build-flavor version string, octet-stream default, concurrent-streams
comment). `reserved` ranges were deliberately declined in favor of a
documented numbering policy in the document.proto header. Still open: caption
overlap asymmetry, slanet-plus confidence misattribution, silent TEXT
fallback for unknown labels, dead `std::max(level, 1)`, thread fan-out per
collector leg, layout-session oversubscription comment, unary/streaming size
asymmetry, `list_level` proto3 footgun, negative `LevelNumber` pass-through,
per-renderer degradation inconsistency, the two live `docling_map` copies,
the consolidated round-trip-loss doc, and most section-5 test-harness gaps.

## New functionality reviewed in the same batch

- `src/targets/` (zip bundle, S3 PUT delivery, hand-rolled SHA-256 + SigV4):
  the crypto matches FIPS 180-4 / RFC 2104 and passes the AWS SigV4
  known-answer vector; zip offsets and EOCD are correct. Two contract issues
  were found and fixed in `7e58577`: `verify_ssl` is now presence-tracked
  (unset means verify — was: proto3 false default silently disabled TLS
  verification), and a failed delivery now degrades to an error item plus
  `CONVERSION_STATUS_PARTIAL_SUCCESS` instead of discarding the computed
  conversion (misconfigured targets still fail the RPC).
- Still open from that review: `put`/`presigned_url` are declared but answer
  UNIMPLEMENTED (now documented on the proto); caller-supplied S3
  endpoint+credentials make ConvertSource an unrestricted egress proxy — the
  trusted-internal posture is now stated in the README; S3 client does not
  follow 301 region redirects; `document.json` in the bundle skips load
  normalization while `exports/canonical_json.json` applies it.
- Google Docs renderer (`18acad2`, `eeaaf19`): no index math exists in-repo
  (the classic UTF-16 insertText bug is not present); `7e58577` renamed the
  placeholder position to `contentOrdinal` and documents that the payload is
  a shape-faithful resource, not a create body — the integration computes
  its own UTF-16 indices.
- Model extension (`738d043`): TimeSpan/ByteSpan/GridCell/polygon provenance
  arms, `InlineSpan`, `CellValue`/`TableColumnSchema`, `Document.source_meta`
  …`media`, `DocumentOrigin.web`, `GenerationSource`, `BarcodeAnnotation`.
  Field numbers verified appended cleanly past the mirrored dialect's range;
  the lockstep surfaces (parse_ref, integrity errors, merge, load
  normalization, renderers) were all updated.
- Fastwarc re-vendor (`653cbf0`, `eca1fc0`): the vendored warc protos now
  diff zero against the shipping server — this silently repaired a real wire
  break (wrong enum value and off-by-one field numbers in the old copy).
  `tests/warc_stub_wire_test.cpp` pins the contract against hand-written wire
  bytes.
- `streaming-service-test` cancellation flake: root cause was asserting a
  best-effort behavior (dequeue-time `IsCancelled` vs. an in-flight cancel
  frame) as deterministic. Fixed by driving the queued call with a short
  deadline instead of `TryCancel` — deadline expiry fires from a local timer
  on both peers, so the no-dial assertion no longer races the transport.
  Additionally, both unary collector lambdas now re-check
  `context->IsCancelled()` immediately before dialing, closing the
  cancel-after-dequeue window.
