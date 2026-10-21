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
