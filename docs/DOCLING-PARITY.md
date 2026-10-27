# Docling parity notes

gRParse mirrors the Docling and docling-serve option surface in typed
protobuf. Where gRParse does something different, or does not do something
upstream does, the reason is written down here so the gap is a decision and
not an accident.

## docling-serve 1.37.0

| Upstream change | gRParse | Why |
|---|---|---|
| `InputFormat.IWORK_NUMBERS` | `INPUT_FORMAT_IWORK_NUMBERS = 36`; `.numbers`, `application/vnd.apple.numbers` and `application/x-iwork-numbers-sffnumbers` route to the libreoffice collector | LibreOffice's Calc import reads Numbers packages, so each sheet comes back as a typed table through the same path as ODS and XLSX. calamine does not read Numbers, so there is no calamine fan-out for it. |
| `CodeFormulaVlmOptions.expansion_factor`, `stop_on_repetition` | Typed optional fields 7 and 8, validated (finite, >= 0) and sent to grpc-enrich as `EnrichOptions.code_formula_expansion_factor` / `code_formula_stop_on_repetition` | The code and formula crops and the model calls happen in grpc-enrich. A cancelled, repeating generation comes back with `truncated_on_repetition`, and gRParse records it on the item as a `GenerationSource` with `truncation = GENERATION_TRUNCATION_REPETITION`. |
| `ChartExtractionOutputFormat.GRANITE_VISION_CHART2CSV` | `CHART_EXTRACTION_OUTPUT_FORMAT_GRANITE_VISION_CHART2CSV = 2`, also accepted as the raw name `granite_vision_chart2csv` | Same validator as Docling: CSV output only. |
| Default code/formula preset (#721) | `code_formula_preset` resolves to the typed `CodeFormulaPresetId`; unset, empty and `default` are `CODEFORMULAV2`, the Docling default; an unregistered name is `INVALID_ARGUMENT` | The preset travels to grpc-enrich as its typed `CodeFormulaPreset`, never as a raw model name. |
| Heading `numbering_schemes` | Typed `repeated HeadingNumberingScheme numbering_schemes = 10`; Docling's string list is accepted as `numbering_schemes_raw` (field 5) and mapped by name | Unknown or unspecified values are rejected by value. A non-empty list replaces the built-in vocabulary: the listed order is the precedence. |
| Ray `ray_task_timeout` / `ray_job_timeout` (#725) | Not implemented | gRParse has no Ray orchestrator. Every request runs in process on the server's own executor, bounded by `document_timeout` and the call deadline, so there is no Ray task or job to time out. |

## docling-core 2.103.0 and docling-slim 2.140.0

| Upstream change | gRParse | Why |
|---|---|---|
| `ConfidenceScores.pages: dict[int, ConfidenceScores]` | `map<int32, ConfidenceScores> pages = 11` on `ConfidenceScores`, keyed by 1-based page number like Docling. Every folded page has an entry; a page with nothing measured has no scores and `QUALITY_GRADE_UNSPECIFIED` grades. Entries are flat: a page entry never carries its own `pages`. | Same shape as docling's `ConfidenceReport.pages`, typed end to end. |
| VLM stop reason (`VlmStopReason`) | `GenerationSource.stop_reason` (field 8, enum `VlmStopReason`): `LENGTH`, `STOP_SEQUENCE`, `END_OF_SEQUENCE`, `CONTENT_FILTERED`, `INFERENCE_ERROR`. `finish_reason` stays as the raw companion. vlm-convert fragments are typed from `finish_reason` before they merge; a producer-set `stop_reason` is kept. | Docling's names map one to one, and `stop` (the OpenAI name) is `END_OF_SEQUENCE` as in docling's picture-description mapping. A value nobody defined stays `UNSPECIFIED`: docling's API image path folds unknown values into end-of-sequence, which would claim a clean stop that was never reported. |
| Table header rows (#767, `_count_column_header_rows`) | The markdown header count follows the same rule: leading rows whose every non-blank cell is a column header, and a row that mixes body text with a flagged cell ends the header. | Expected strings in `verify_markdown_header_rows_follow_docling_core_2_102` are docling-core 2.103.0's own `export_to_markdown` output (receipt: `docs/receipts/feat-parity-core-2.103-slim-2.140/header-rows-reference.txt`). |
| Contextualized chunks over `max_tokens` (#690) | The hybrid split re-measures every piece with its heading trail and shrinks the text budget by the worst overflow until each piece fits, the same convergence loop. A budget the heading trail alone fills leaves the chunk whole and logged, as before. | A tokenizer whose merges cross the heading's newline makes heading + text count more than the two apart; `verify_hf_split_converges_when_the_heading_join_is_not_additive` pins that case with a fixture tokenizer. |
| Column-wise table splitting and repeated table headers (#690) | Not ported. An oversized table chunk splits on text spans (sentence, word, codepoint) like any other chunk, and each piece still fits `max_tokens`. | Row and column aware splitting is a chunk-shape change, not a budget fix; it needs its own design against the typed table chunk output. |
| `max_tokens_override` (`LineBasedTokenChunker`) | No new option. `HybridChunkerOptions.max_tokens` (field 3) is already the typed per-request budget. | Upstream's override is an internal knob of the line-based chunker the table split calls, not a user option. |
| Local image references (`file://` and paths) | gRParse never opens an image URI from a client document. Only `data:` URIs decode to bytes (`decode_image_data_uri`, chart de-render, page CV enrichment, bundle targets); a `file://`, path, or `http(s)` URI is kept as text in rendered output. EPUB `src` values resolve only to entries inside the uploaded archive. | Docling 2.103 gates local image loading behind a setting; gRParse has no such path to gate. Pinned by `verify_image_uris_are_never_dereferenced` and the EPUB href cases. |
| Calendar MIME types (`text/calendar`, `application/calendar+json`, ...) | Deferred. | docling-core added the MIME names, but no Docling backend converts calendars yet. gRParse adds an `InputFormat` and a collector when one does, so a calendar upload is not silently accepted and dropped. |
| `ThreadedDoclingParseBackendOptions.max_concurrent_results` | Deferred. | gRParse does not run the threaded docling-parse backend; PDF pages come from the `PdfBackendService` targets, and their concurrency is bounded by the page scheduler and `GRPARSE_PDF_BACKEND` targets. |

## Heading options and typed rejections

| Option | gRParse | Notes |
|---|---|---|
| `use_bookmarks` (default true) | Implemented | A heading takes the depth of the best-matching `Document.outline` entry. The entry must be on the same page when both name one, and its title similarity must reach `bookmark_match_threshold`. A document without an outline is unaffected, which is the same as Docling on a PDF without bookmarks. |
| `bookmark_match_threshold` (default 0.8) | Implemented, validated in [0, 1] | Similarity is 1 minus the edit distance over the longer normalized title. Normalizing lower-cases the title and collapses punctuation and white space. |
| `use_font_style` (default true) | Implemented | In the style pass, among headings of one size, bold ranks above regular, upright above italic, and all-caps above mixed case. Weight and slant come from the item formatting or the majority of its runs. A heading whose runs carry no font information ranks by size alone. |

Every INVALID_ARGUMENT that turns an option down carries a typed `OptionRejection` (option path, `OptionRejectionReason`, the raw rejected value). It is packed in `google.rpc.Status.details`, so clients read it from the standard `grpc-status-details-bin` trailer.

`S3Source.verify_ssl` is now `optional bool` (same tag 6). An unset value verifies; only an explicit false skips verification. The old implicit bool read an unset field as false.

## Chart extraction: chart2summary and chart2code

| Option | gRParse | Notes |
|---|---|---|
| `ChartExtractionVlmEngineOptions.chart2summary` | Implemented | Sent to grpc-enrich as `ChartExtractionOptions.summary`. The reply lands in the picture's `meta.description` (created_by the model) with a description annotation and a `GenerationSource`, where Docling stores it. |
| `ChartExtractionVlmEngineOptions.chart2code` | Implemented | Sent as `ChartExtractionOptions.code`. grpc-enrich takes the Python out of the reply's code fence, and gRParse stores it in the picture's `meta.code` with `CODE_LANGUAGE_LABEL_PYTHON`. |
| `custom_chart_extraction_presets` | Typed `ChartExtractionPresetRegistry` of `ChartExtractionPresetSpec` | `GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS` is that message in protobuf text format; it used to be a free-form JSON object read through `google.protobuf.Struct`. The protobuf text parser rejects unknown fields, wrong types (a quoted `"yes"` for a bool, for example) and unknown enum values. `engine_type` is a `VlmEngineType` name such as `VLM_ENGINE_TYPE_API_OLLAMA`, so Docling's lowercase engine names are rejected in this variable. This changes the variable's format, and existing deployments must rewrite it. compose.stack.yaml is updated. |
| `allowed_chart_extraction_engines` | Mapped to `VlmEngineType` at startup | The variable still takes Docling's engine names, because that is the docling-serve setting. Each name maps to the enum, and an unknown name stops the process. The policy compares enums, not strings. List variables in JSON array form are read as the typed `ServerSettingNameList`. |

Each enabled output is its own VLM call per chart in grpc-enrich, so a failed summary never costs the table. grpc-enrich has implemented the three outputs since 05820b7, so this needed no change there. `scripts/test/chart-outputs-e2e-test.sh` runs a real grpc-enrich container against an emulated OpenAI-compatible VLM, and checks that the table, summary and code arrive in the Document.

## Storage sources and targets (docling-slim 2.139.0 sources.py and targets.py)

| Docling | gRParse | Notes |
|---|---|---|
| `AzureBlobSource` / `AzureBlobTarget` | `Source.azure_blob` (4) / `Target.azure_blob` (6), `AzureBlobCoordinates` | Same five coordinates. Shared Key signing from the connection string. |
| `GoogleCloudStorageSource` / `GoogleCloudStorageTarget` | `Source.google_cloud_storage` (5) / `Target.google_cloud_storage` (7), `GoogleCloudStorageCoordinates` | Same coordinates; `service_account_key` is the typed ten-field `GoogleCloudStorageServiceAccountInfo`. Adds an optional `endpoint` for emulators. |
| `GoogleDriveSource` / `GoogleDriveTarget` | `Source.google_drive` (6) / `Target.google_drive` (8), `GoogleDriveCoordinates` | `path_id`, `refresh_token` and typed `credentials` match. `token_path` and `credentials_path` (server file paths) are not accepted. A source names one file, since folders are batch-only upstream. |
| `S3Source` | `Source.s3` (2), `S3Source` | Implemented: one object under `key_prefix`, SigV4, path style. `verify_ssl` honoured (unset verifies, explicit false skips). Credentials as for `S3Target`. |
| `HttpSource` | `Source.http` (1), `HttpSource` | Implemented: GET with typed `headers`, certificate always verified, up to 5 http/https redirects with headers kept to the named origin, public addresses only unless `GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1`, body limit `GRPARSE_SOURCE_MAX_BYTES`. Filename from `Content-Disposition`, else the final URL. 4xx is `INVALID_ARGUMENT`, 5xx and transport failures `UNAVAILABLE`. |
| Connector plugins (docling-serve #652, #660) | `Source.plugin` (7) / `Target.plugin` (9): `kind` plus `google.protobuf.Any` config | The config must be the plugin's registered protobuf message. |
| `allow_external_plugins`, `allowed_source_types`, `allowed_target_types` | `GRPARSE_ALLOW_EXTERNAL_PLUGINS`, `GRPARSE_ALLOWED_SOURCE_TYPES`, `GRPARSE_ALLOWED_TARGET_TYPES` | Refusals carry `OPTION_REJECTION_REASON_NOT_ALLOWED`. |

`http` and `s3` join the built-in source kinds, allowed by default as upstream.

`StoredObject` gains `store_id` (4), the Drive file id. `OptionRejectionReason` gains `NOT_ALLOWED` (6) and `INVALID` (7).

## OCR presets (docling-serve #723, docling-jobkit OCR registry)

| Docling | gRParse | Notes |
|---|---|---|
| `default_ocr_preset` (default `auto`) | `GRPARSE_DEFAULT_OCR_PRESET` | Must be a kind this server runs (`auto`, `rapidocr`), checked at startup. |
| `default_ocr_kind` (default `auto`) | `GRPARSE_DEFAULT_OCR_KIND` | Must be a kind this server runs (`auto`, `rapidocr`), checked at startup; always allowed by the kind check. |
| `allowed_ocr_presets` | `GRPARSE_ALLOWED_OCR_PRESETS` | Narrows the built-in kinds; `default` and custom presets stay selectable (#723). |
| `custom_ocr_presets` (`{"id": {"kind": ..., "lang": [...]}}`) | `GRPARSE_CUSTOM_OCR_PRESETS`, typed `OcrPresetRegistry` of `OcrPresetSpec` (protobuf text format) | `kind` is the `OcrEngineKind` enum. Only `lang` is accepted beside it; other engine options are unknown fields and stop startup. `lang` is validated against the installed PP-OCRv3 set (Chinese and English) and selects no model, so a preset is a name and policy gate rather than an engine configuration. |
| `allowed_ocr_kinds` | `GRPARSE_ALLOWED_OCR_KINDS` | Names are checked against the known kinds. |
| Unset `ocr_preset` | `default` | Upstream's request default is the literal `auto`; here an unset preset means `default`, which resolves to `auto` unless configured. |

Refusals carry a typed `OptionRejection` on `ocr_preset`: `NOT_ALLOWED` for a name outside the registry or an excluded kind, `NOT_IMPLEMENTED` for a kind this server cannot run.

## docling 2.142.1 / docling-core 2.104.0 (2026-10-26)

### New typed fields

| Upstream change | gRParse | Notes |
|---|---|---|
| `ResponseFormat` gains `doclang`, `deepseekocr_markdown`, `unlimited_ocr_markdown`, `chandra_html`, `dots_json`, `nemotron_parse_v2`, `nemotron_parse_v3_5`, `mineru2` | `RESPONSE_FORMAT_DOCLANG = 6` through `RESPONSE_FORMAT_MINERU2 = 13`, in docling's declaration order. `optional string response_format_raw` on `VlmModelLocal` (9), `VlmModelApi` (10) and `VlmModelSpec` (14) takes docling's string name. | An unknown number or name is `UNKNOWN_VALUE`; a typed value and a raw name that disagree is `CONFLICT` on `.response_format_raw`. On the VLM conversion leg (`vlm_pipeline_model_local`, `vlm_pipeline_model_api`, `vlm_pipeline_custom_config.model_spec`) every format but `mineru2` reaches grpc-vlm-convert (vendored `vlm_convert.proto` carries 6 to 12 and `response_format_raw`; gRParse sends the typed value and the docling name). `mineru2` is `NOT_IMPLEMENTED`: its two-pass layout-then-content protocol is not driven by the collector. For `chandra_html`, `deepseekocr_markdown`, `dots_json`, `nemotron_parse_v2`, `nemotron_parse_v3_5` and `unlimited_ocr_markdown` gRParse applies docling's `normalize_section_header_levels` once per converted document (all headers shift up one level when none is at level 1); `doclang` is not normalized, as in docling. The enrichment legs' specs (`picture_description_custom_config`, `chart_extraction_custom_config`, `code_formula_custom_config`) accept every format, `mineru2` included, checked for a known and agreeing value and otherwise unread, which matches docling: only `vlm_convert` reads `response_format`. docling-serve's proto at the time of writing (bbe5ed1) has values 1 to 5 only, so 6 to 13 are numbered here first; if serve numbers them differently, the names are the contract. |
| `trust_remote_code` on VLM engine options | `BaseVlmEngineOptions.trust_remote_code = 2` | An API engine (`API`, `API_OLLAMA`, `API_LMSTUDIO`, `API_OPENAI`) with the field set is `CONFLICT`: the model runs remotely. An inline engine with `true` is `NOT_IMPLEMENTED` (gRParse loads no remote model code); `false` is accepted. Not added to `VlmModelLocal`. |
| `picture_description_custom_config` (`PictureDescriptionVlmEngineOptions`) | Existing field | Wired as docling's `PictureDescriptionVlmEngineModel` reads it: the enrich model is `model_spec.default_repo_id` (docling's `get_repo_id` with no engine override) unless `picture_description_local.repo_id` or `picture_description_preset` names one, and the prompt is the options' `prompt`, else `model_spec.prompt`. `classification_allow`, `classification_deny` and `classification_min_confidence` are forwarded as before. `scale`, `batch_size`, `picture_area_threshold`, `generation_config` and engine overrides are accepted and not forwarded (the enrich service owns rendering and batching). |
| docling-core #899 structure checks | `StructureRule` `REPEATED_REFERENCE = 8`, `RICH_CELL_NOT_OWNED = 9`, `TABLE_CELLS_OVER_LIMIT = 10`, `TOTAL_TABLE_CELLS_OVER_LIMIT = 11`. `message StructureTableCellLimits { max_table_cells = 1; max_total_table_cells = 2; }` on `ConvertDocumentOptions.structure_table_cell_limits = 66` and `DocumentChunk.structure_table_cell_limits = 14`. | Defaults are docling-core's settings (1,000,000 and 5,000,000). A limit of 0 is `OUT_OF_RANGE`; limits with validation `OFF` are `CONFLICT`. The limits are not part of the cache digest (findings are recomputed). |
| `target_document_version` (docling-serve / jobkit, from the `Accept-Docling-Document-Version` header) | None | Serve-only: docling-serve reads the header and jobkit down-projects the exported JSON with `docling_core.compat.project_to`. It is not a `ConvertDocumentsOptions` field in docling 2.142.1, so it has no gRParse option. gRParse's wire is protobuf, where an older reader ignores unknown fields. |
| `extract_form_fields` (PDF widgets) | `ConvertDocumentOptions.extract_form_fields = 65` | Unset keeps the existing fold (PDF form widgets folded when a widget backend is configured). `false` skips the fold. `true` on a PDF with no widget backend is `NOT_IMPLEMENTED` on `extract_form_fields`; `true` on a non-PDF adds a document warning. `true` also folds widgets on the VLM pipeline branch; unset does not. Deviation: docling's default is `false`; gRParse keeps its pre-existing unset behavior so current callers do not change. |

### Exporter and router checks (review action 8)

| Upstream change | gRParse | Proof |
|---|---|---|
| DocLang 0.8 keeps every caption (#880) | Already does this. Each caption is its own `<caption>` element. Captions are written as siblings before the host element, not inside its head; that placement predates 0.8 and is unchanged. | `verify_every_caption_is_kept` (doclang-renderer-test) |
| Table, picture and code footnotes (#675) | Fixed. Markdown: after the body, one paragraph each, before captions that go after. HTML: one `<div class="footnote">` per footnote (text escaped as Python's `html.escape`), after `</table>`, inside `<figure>` for a picture, and a code block with captions or footnotes becomes a `<figure>`. Chunks: footnotes join the table, picture (before its captions) and code chunk, and never chunk on their own; they are not reported as captions. | markdown-renderer-test, `verify_table_footnotes_follow_the_table` and siblings (html-renderer-test), `verify_footnotes_chunk_with_their_item` (chunking-test) |
| Hyperlink escaping and scheme filter (#920) | Fixed for Markdown and LaTeX: `is_safe_hyperlink` and `markdown_link_destination` in renderer_base. Markdown image destinations are encoded too. DocLang already escaped `&`, `"`, `<`, `>` in `href` and `uri` attributes. The HTML export renders no hyperlinks, so there is no `href` to escape. | `verify_hyperlink_schemes_and_markdown_destinations` (renderer-base-test), `verify_hyperlinks_are_filtered_and_encoded`, `verify_image_destinations_are_encoded` (markdown-renderer-test), `verify_an_unsafe_hyperlink_keeps_only_its_text` (latex-renderer-test) |
| WebVTT ignores tags that are not cue spans (#896) | Already does this, in grpc-markup (`src/vtt.rs` `open_span`): an unknown start tag opens an unstyled run, so its text is kept and the cue survives. | Code reading only; grpc-markup has no dedicated unknown-tag test. |
| `audio/flac` and `audio/ogg` (#905) | Already does this: any `audio/` or `video/` content type and the `.flac`, `.ogg`, `.oga` extensions route to `COLLECTOR_ASR`. | collector-coordinator-test (audio/ogg, .flac, .ogg cases) |

Deviations kept: HTML footnotes carry no `dir="rtl"` (this HTML export marks direction on no element). `normalized_uri` lowercases a one-letter scheme, so a hyperlink `C:/x` renders as `c:/x` (pre-existing). The entity check in `markdown_link_destination` counts any UTF-8 byte as a word character where Python's `\w` is Unicode-aware. Footnotes of key-value and form items are not rendered.

### Not taken

| Upstream change | Why |
|---|---|
| JSON down-projection of new fields for older readers | gRParse's wire is protobuf; older readers ignore unknown fields. |
| Pillow mode `I` image handling | gRParse decodes images with OpenCV, not Pillow. |
| RQ and Ray orchestrator settings | gRParse has neither orchestrator; requests run in process. |
| transformers ASR model families | ASR runs in grpc-asr, not through transformers. |
| EPUB images on by default | Already on: grpc-epub `ParseOptions.include_images = 5` (vendored `collectors/epub_types.proto`) is absent-means-true. |

### Open

- `mineru2` stays `NOT_IMPLEMENTED` on the VLM leg (two-pass protocol), here and in grpc-vlm-convert.
- grpc-vlm-convert PR #24 (formats 6 to 12, `response_format_raw`) must merge for a deployed peer to serve them; an older peer answers `UNIMPLEMENTED` or ignores the field.
- `trust_remote_code = true` on an inline engine stays `NOT_IMPLEMENTED`; docling's Nemotron Parse v2 spec defaults it to true, so a client copying that spec must send false or omit it.
- `vlm_pipeline_custom_config`: `model_spec.default_repo_id` (the model, unless `vlm_pipeline_model`, `_local`, `_api` or `vlm_pipeline_preset` names one), `model_spec.prompt`, `model_spec.response_format` and `scale` are forwarded to grpc-vlm-convert. `engine_options` beyond `engine_type` and `trust_remote_code`, the spec's `revision`, `stop_strings`, `temperature`, `max_new_tokens`, `extra_generation_config` and engine overrides, plus `max_size`, `batch_size` and `force_backend_text`, are accepted and not forwarded: `ConvertOptions` has no field for them.
- `extract_form_fields` unset folds widgets (gRParse default); docling's unset is false. Kept so existing callers keep `field_items`.
- docling-serve numbering for `ResponseFormat` 6 to 13 is not yet confirmed.
