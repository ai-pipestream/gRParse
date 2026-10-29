# Design: the grpc-unlimited-ocr collector

**Status:** design, no implementation yet. The service does not exist; this
document is the agreement to build against.
**Updated:** 2026-10-28
**Upstream studied:** `baidu/Unlimited-OCR` at `48ff432` (2026-10-28) and the
Hugging Face model repos `baidu/Unlimited-OCR-2` and `baidu/Unlimited-OCR`.

## 0. Recommendation in one paragraph

Build it as its own C++23 gRPC collector, `grpc-unlimited-ocr`, on port
50073, running the model in process on ONNX Runtime (CUDA, OpenVINO, CPU)
with no Python at build or run time. It takes a document as a stream of
page rasters and streams back typed blocks and per-page `Document`
fragments as the decoder crosses each page boundary, then a folded
`Document` with cross-page tables and paragraphs joined. gRParse dials it
like any other collector (`GRPARSE_UNLIMITED_OCR_TARGET`), explicit
selection only at first. It does **not** go into gRParse's CV path, and it
is **not** the fix for #162: that issue is a 10 MB line-recognizer swap
inside the CV path, while this is a 6.7 GB whole-document model that wants
a GPU, its own release cadence and its own container. The two land
independently; the new service then gives #162-style fixes a strong
second reading to be scored against.

## 1. What Unlimited-OCR is

A vision-language document parser in the DeepSeek-OCR lineage, published
by Baidu. Version 2 (2026-10-27) is the one worth integrating; version 1
(2026-06-22) is the fallback if the v2 attention proves hard to port.

| Part | What it is | Size |
|---|---|---|
| Vision encoder ("DeepEncoder") | SAM ViT-B (12 layers, width 768, windowed + 4 global layers) feeding CLIP-L/14 (24 layers, width 1024); features concatenated to 2048 | about 0.4 B params |
| Projector | one linear layer 2048 to 1280 | 2.6 M |
| Decoder | DeepSeek-V2 style MoE LM: 12 layers, hidden 1280, 10 heads of 128, layer 0 dense (FFN 6848), layers 1 to 11 MoE with 64 routed experts (FFN 896) top-6 plus 2 shared, vocab 129,280, YaRN RoPE (theta 5e5, factor 4) | about 2.9 B total, about 0.57 B active per token |
| v2 attention ("SR-SWA", code name DASH) | per query: an init block, 4 local blocks and the top-16 32-token blocks chosen by a one-head 128-dim indexer, restricted to the image blocks of the page being transcribed; "page2d" RoPE over (page, token-in-page) | part of the decoder |
| Weights | two safetensors shards, bf16 | 6,688,305,576 bytes (v2); 6,672,547,120 (v1) |
| Tokenizer | byte-level BPE `tokenizer.json` with the layout control tokens (`<PAGE>` id 128827, `<image>` id 128815, `<BLOCK>`, `<FORK>`, `<LEVELn>`, `<MERGE>`) | 9.98 MB |

**Input.** One prompt per document: `"<PAGE><image>" * n + "Multi page
parsing."`, BOS first. Each page becomes a 1024x1024 letterboxed global view
(gray pad, mean/std 0.5) worth 273 image tokens ("base" mode), and in
"tiled" mode also 2 to 16 640x640 tiles chosen by aspect ratio, worth
`(10w+1)*10h` more tokens. Multi-page requests default to base mode.

**Output.** Not Markdown. One token stream per document:

```text
<PAGE><BLOCK>title<LEVEL1>[x1,y1,x2,y2]<FORK>text ...
<BLOCK>text[x1,y1,x2,y2]<FORK>body ...
<BLOCK>cross_table[bbox]<FORK><table>...</table><PAGE><BLOCK>append_table[bbox]<FORK>...
```

Block labels: `text, title, header, footer, page_number, image, figure,
chart, seal, table, image_caption, image_footnote, page_footnote,
equation, ref_text, aside_text, algorithm`, each optionally prefixed
`cross_` (continues on the next page) or `append_` (continues from the
previous one). Boxes are page-normalised integers 0 to 1000. Tables are
HTML with `colspan`/`rowspan`. `<MERGE>` joins same-page columns. This is
already a typed structure; the only reason upstream turns it into
Markdown is its benchmark.

**Decode loop.** Greedy, temperature 0. Two logit processors run every
step: a no-repeat n-gram ban (n=35, window 1024 tokens for multi-page, 128
for one page) and a page-count guard (EOS banned until n `<PAGE>` markers
have been emitted, forced after; a per-page token cap forces the next
`<PAGE>`). The upstream server bounds memory by decoding in windows of 10
pages and carrying the KV tail of the previous window into the next.

### Why it is worth a service

It reads a whole document in one decode, so it joins tables and
paragraphs across page breaks, keeps a heading hierarchy across pages, and
labels page furniture, all of which the fleet currently reconstructs after
the fact. Upstream reports state-of-the-art results on multi-page and
hundred-page benchmarks. None of that is reachable through a per-page
recognizer or through grpc-vlm-convert's one-call-per-page contract.

## 2. Licences

| Item | Licence | Verdict |
|---|---|---|
| Upstream code (`baidu/Unlimited-OCR`) | MIT | compatible; we port, keep the notice in `NOTICE` |
| Model code on Hugging Face (`modeling_*.py`, `deepencoder.py`) | MIT (repo licence `mit`) | compatible; ported, not vendored |
| Weights v2 and v1 | MIT per model card | compatible. Never baked into the image; fetched by manifest with sha256 like gRParse's `models/MANIFEST` |
| Lineage: DeepSeek-OCR (MIT), SAM (Apache-2.0), CLIP (MIT) | permissive | compatible |
| Training data | not stated by upstream | no licence obligation on us; noted as a provenance gap in the README |
| Custom SGLang wheel in upstream | Apache-2.0 | not used |
| PyMuPDF in upstream's client example | **AGPL-3.0** | not used, not ported. gRParse renders pages through its PDF backend fleet and sends rasters |
| Our runtime: ONNX Runtime (MIT), OpenVINO (Apache-2.0), gRPC (Apache-2.0), protobuf (BSD-3), abseil (Apache-2.0), OpenCV (Apache-2.0), tokenizers-cpp and the `tokenizers` crate (Apache-2.0) | permissive | the same set gRParse already ships |
| CUDA and cuDNN runtime libraries | NVIDIA redistributable EULA | the same posture as gRParse's CUDA image; CPU and OpenVINO images carry none |

Nothing copyleft enters the build, the image or the request path. The
repository is Apache-2.0 like the rest of the fleet.

## 3. Where it lives: decision

**Its own repository and service, `grpc-unlimited-ocr`.** Reasons, in
order of weight:

1. **The fleet rule.** A parser lives in exactly one place and everything
   else reaches it over the wire (`docs/COLLECTORS.md`). The bar for an
   in-process exception is "a constrained format with no owner anywhere
   else"; a 3 B parameter model is the opposite of that.
2. **Footprint.** gRParse's models directory is about 0.25 GB and the
   default stack runs on a CPU box. Adding 6.7 GB of weights and a GPU
   expectation to every gRParse image would tax every deployment for an
   opt-in feature.
3. **Device contention.** The CV path keeps layout, OCR and table models
   resident and tunes its ORT sessions per page worker. A long autoregressive
   decode holding gigabytes of KV cache in the same process would fight
   that tuning. A separate container gets its own GPU memory budget,
   restarts on its own and scales on its own.
4. **Unit of work.** The CV path is per page and parallel. This model is
   per document and sequential (cross-page context is the point). That is
   a different scheduler, a different back-pressure story and a different
   failure mode, which is exactly what a collector boundary isolates.
5. **Release cadence.** The model was released yesterday; upstream will
   move. Its churn stays out of gRParse's release train.

**Rejected: extending grpc-vlm-convert.** That service is an HTTP client
for external VLM servers, one chat completion per page. Unlimited-OCR's
value comes from one decode over many pages, and the user's requirement is
in-process inference with no Python server. Bolting an inference engine
onto an HTTP proxy would make one service with two unrelated halves.
vlm-convert stays what it is; anyone who runs upstream's SGLang server can
already point vlm-convert at it with `VLM_PRESET_RAW`, which is a cheap
interim for people who accept Python on their own GPU box.

**Rejected: a shared contract in parser-protos.** parser-protos holds the
contract several interchangeable backends implement (`PdfBackendService`).
Until a second engine speaks this contract there is nothing to share, so
the collector owns its contract like every other collector, and gRParse
vendors it byte-identical into `collectors/`. If a second whole-document
OCR engine appears, promoting the contract into parser-protos is additive.

**Language: C++23, not Rust.** The request mentioned "our rust project";
the parts this service is made of already exist in C++ in this family:
ORT session and execution-provider plumbing (`gRParse/src/ort_session_ep.cpp`
and the three Dockerfiles), OpenVINO kernel-cache handling, the HTML
table mapper and `Document` building in grpc-vlm-convert, and the
tokenizers-cpp integration in gRParse. Copying a sibling beats inventing.
The `ort` Rust crate would work, but every EP lesson (fp16 defaults
breaking real pages, kernel cache on a real disk, Jetson ORT builds)
would have to be relearned there.

## 4. Running the model: C++ on ONNX Runtime

### 4.1 Model artifacts, built without Python

A C++ tool in the repo, `uocr-export`, reads the upstream safetensors and
writes ONNX graphs directly through the ONNX protobuf schema. It is a
graph builder, not a tracer: each module (SAM block, CLIP block, MoE
layer, sparse attention) is a function that emits nodes, so per-EP
variants and quantisation are flags instead of re-exports. Output:

| Graph | Inputs | Outputs |
|---|---|---|
| `vision.onnx` | `global_views [N,3,1024,1024]`, `tiles [M,3,640,640]`, tile grid | image embeddings `[T,1280]` |
| `decoder.onnx` | `inputs_embeds [1,q,1280]`, page and in-page position ids, per-layer K/V and indexer-key caches (bound on device), `allowed_blocks [1,nblk]` | last-position logits `[1,129280]`, the q new K/V/indexer rows per layer |
| `embed.onnx` | token ids | embeddings (or a raw tensor file the engine gathers from) |

The artifacts are published as a models image and a `models/MANIFEST`
with sha256 and size per file, the way `pipestreamai/grparse-models` and
`scripts/fetch-models.sh` already work. The service never downloads.

The single place Python touches the project is outside it: parity goldens
(the reference token ids and decoded output for a fixed set of pages) are
recorded once from upstream's own reference implementation on a GPU box
and committed as data files. No build step and no test runs Python.

### 4.2 The engine

Inside the service the model is five small units, none of them a god
class:

- `page_preprocess`: OpenCV letterbox to 1024, aspect-ratio tiling to
  640, normalise to the upstream mean and std, in memory.
- `prompt_builder`: token ids and the image-token mask for a page window,
  ported from `_uocr_build_inputs`.
- `kv_cache`: device buffers per layer, appended through ORT IOBinding;
  window rollover carries the tail, as upstream's page-window scheduler
  does.
- `page_router`: the per-block page bookkeeping that decides which image
  blocks a decode step may attend to, ported from
  `_uocr_dash_decode_allow` and fed to the graph as `allowed_blocks`. The
  top-k block choice itself stays inside the graph (TopK plus Gather).
- `logit_policy`: no-repeat n-gram and page-count guard, ported from
  `_uocr_no_repeat_ngram` and `_uocr_page_count_guard`, then argmax.

The split between C++ and graph is deliberate: bookkeeping with branches
lives in C++ where it is testable without a model; tensor math lives in
the graph where every EP can run it.

### 4.3 Execution providers

`GRPC_UOCR_ORT_EP=cuda|openvino|cpu`, the same switch gRParse uses.

- **CUDA** (`latest`): fp16 weights (bf16 converted at export), the
  contrib `MoE` op for the expert layers, about 7 GB of VRAM for weights
  plus about 61 KB of KV per token (12 layers x 2 x 1280 x fp16). A
  10-page window with about 2,700 image tokens and 15,000 output tokens is
  about 1.1 GB of cache, so a 16 GB card serves it with room to spare.
- **OpenVINO** (`latest-openvino`, Arc B70 on krick-1): the contrib MoE op
  is not available, so the export emits the portable form (router TopK,
  Gather of the six chosen experts' weights, batched MatMul), which is
  cheap at q=1 during decode and runs grouped-by-expert for prefill. FP32
  first: the fleet has already seen the GPU plugin's fp16 default break
  accuracy on real pages, so fp16 and int8 are promoted only after the
  real-input gate in 4.4 passes. Kernel cache on the named volume.
- **CPU** (`latest-cpu`, multi-arch): int4 weight-only `MatMulNBits`
  (about 2 GB) built by `uocr-export`. It works, and it is slow; it is for
  correctness runs and small documents, not throughput.

Throughput is measured in phase 0, not promised here. For scale: the
fleet's North Micro Vision numbers (2.4 B dense, transformers-served) were
79 tok/s on the 4080 and 58 on the B70; this model activates about a
quarter of that per token.

### 4.4 Correctness gates for the engine

1. Greedy token parity with the recorded goldens: identical token ids on
   the fp32 CPU path for the golden pages; fp16 and int4 paths judged on
   decoded output against the same goldens (edit distance and block-label
   agreement), never on random tensors.
2. Determinism: the same document twice gives the same token stream on
   every EP.
3. Every EP gate runs on real rendered pages from `tests/golden/corpus`.

## 5. Wire contract

Package `ai.pipestream.unlimitedocr.v1`, owned by the new repo under
`proto/`, managed by buf (`buf lint`, `buf format` as CI gates), vendored
byte-identical into `gRParse/collectors/` and into the shell's
`examples/web-demo/peer-protos/`. It imports the fleet `document.proto`
byte-identical like every other Document-plane collector.

The rule for this contract: **every fact is a typed field.** No JSON
strings, no `map<string,string>`, no `google.protobuf.Struct`, no opaque
bytes standing in for structure. Boxes are messages, labels are enums,
tables are rows of cells, errors carry an enum code. The model's raw text
appears only in an opt-in diagnostics event that nothing downstream
parses.

```proto
service UnlimitedOcrService {
  // Client sends ParseOptions first, then PageImage messages (any order,
  // keyed by page_no), then half-closes. The server streams events as
  // the decoder produces them.
  rpc ParseDocument(stream ParseDocumentRequest) returns (stream ParseDocumentResponse);
  rpc GetServiceInfo(GetServiceInfoRequest) returns (GetServiceInfoResponse);
}

message ParseDocumentRequest {
  oneof payload {
    ParseOptions options = 1;
    PageImage page = 2;
  }
}

message ParseOptions {
  ViewMode view_mode = 1;            // BASE (default for multi-page) or TILED
  uint32 page_count = 2;             // required; drives the page-count guard
  uint32 page_window = 3;            // pages per decode window, 0 = server default
  uint32 max_tokens_per_page = 4;    // 0 = server default
  bool emit_blocks = 5;              // stream Block events
  bool emit_page_documents = 6;      // stream PageDocument fragments
  bool emit_document = 7;            // send the folded Document at the end
  bool emit_diagnostics = 8;         // RawPageOutput, for debugging only
  bool abort_on_error = 9;
}

enum ViewMode {
  VIEW_MODE_UNSPECIFIED = 0;
  VIEW_MODE_BASE = 1;                // one 1024 global view per page
  VIEW_MODE_TILED = 2;               // global view plus 640 tiles
}

message PageImage {
  uint32 page_no = 1;                // 1-based
  bytes png = 2;                     // the page raster itself, not structure
  uint32 width_px = 3;
  uint32 height_px = 4;
}

message ParseDocumentResponse {
  oneof event {
    WindowStarted window_started = 1;
    Block block = 2;
    PageDocument page_document = 3;
    PageFailure page_failure = 4;
    ContinuationLink continuation = 5;
    ai.pipestream.document.v1.Document document = 6;
    RawPageOutput diagnostics = 7;
    ParseComplete complete = 8;
  }
}

message WindowStarted {
  uint32 window_index = 1;
  uint32 first_page_no = 2;
  uint32 last_page_no = 3;
  uint64 prompt_tokens = 4;
}

// One layout block, emitted the moment its content closes.
message Block {
  uint32 page_no = 1;
  uint32 ordinal = 2;                // reading order within the page
  BlockKind kind = 3;
  Continuation continuation = 4;
  optional uint32 heading_level = 5; // TITLE blocks only
  NormalizedBox box = 6;
  bool merges_with_previous = 7;     // same-page <MERGE>
  oneof content {
    TextContent text = 10;
    TableContent table = 11;
    PictureRegion picture = 12;
    FormulaContent formula = 13;
  }
}

enum BlockKind {
  BLOCK_KIND_UNSPECIFIED = 0;
  BLOCK_KIND_TEXT = 1;
  BLOCK_KIND_TITLE = 2;
  BLOCK_KIND_PAGE_HEADER = 3;
  BLOCK_KIND_PAGE_FOOTER = 4;
  BLOCK_KIND_PAGE_NUMBER = 5;
  BLOCK_KIND_IMAGE = 6;
  BLOCK_KIND_FIGURE = 7;
  BLOCK_KIND_CHART = 8;
  BLOCK_KIND_SEAL = 9;
  BLOCK_KIND_TABLE = 10;
  BLOCK_KIND_IMAGE_CAPTION = 11;
  BLOCK_KIND_IMAGE_FOOTNOTE = 12;
  BLOCK_KIND_PAGE_FOOTNOTE = 13;
  BLOCK_KIND_EQUATION = 14;
  BLOCK_KIND_REFERENCE = 15;
  BLOCK_KIND_ASIDE = 16;
  BLOCK_KIND_ALGORITHM = 17;
  BLOCK_KIND_UNRECOGNISED = 18;      // text the parser could not attach to a label
}

enum Continuation {
  CONTINUATION_UNSPECIFIED = 0;      // a whole block
  CONTINUATION_CONTINUES_ON_NEXT_PAGE = 1;   // cross_
  CONTINUATION_CONTINUED_FROM_PREVIOUS = 2;  // append_
}

message NormalizedBox {              // 0..1000 on both axes, page-relative
  uint32 x1 = 1;
  uint32 y1 = 2;
  uint32 x2 = 3;
  uint32 y2 = 4;
}

message TextContent { string text = 1; }
message FormulaContent { string latex = 1; }
message PictureRegion { }            // the box is the payload; gRParse crops

message TableContent {
  uint32 row_count = 1;
  uint32 column_count = 2;
  repeated TableCellContent cells = 3;
  uint32 repeated_header_rows_dropped = 4;  // on an append_ fragment
}

message TableCellContent {
  uint32 row = 1;
  uint32 column = 2;
  uint32 row_span = 3;
  uint32 col_span = 4;
  bool header = 5;
  string text = 6;
}

// A block on one page continues a block on another.
message ContinuationLink {
  uint32 from_page_no = 1;
  uint32 from_ordinal = 2;
  uint32 to_page_no = 3;
  uint32 to_ordinal = 4;
  BlockKind kind = 5;
}

message PageDocument {
  uint32 page_no = 1;
  ai.pipestream.document.v1.Document document = 2;
}

message PageFailure {
  uint32 page_no = 1;
  PageFailureCode code = 2;
  string detail = 3;                 // human text with numbers, never the only carrier
}

enum PageFailureCode {
  PAGE_FAILURE_CODE_UNSPECIFIED = 0;
  PAGE_FAILURE_CODE_TOKEN_CAP = 1;           // per-page cap forced the page closed
  PAGE_FAILURE_CODE_REPETITION = 2;          // the n-gram ban fired continuously
  PAGE_FAILURE_CODE_UNPARSEABLE_OUTPUT = 3;  // the block grammar did not match
  PAGE_FAILURE_CODE_IMAGE_DECODE = 4;
  PAGE_FAILURE_CODE_INFERENCE = 5;
}

message RawPageOutput {              // diagnostics only
  uint32 page_no = 1;
  string model_output = 2;
}

message ParseComplete {
  uint32 pages_ok = 1;
  uint32 pages_failed = 2;
  uint64 prompt_tokens = 3;
  uint64 completion_tokens = 4;
  ai.pipestream.document.v1.VlmStopReason stop_reason = 5;
  uint64 decode_microseconds = 6;
}
```

`GetServiceInfoResponse` carries the version, the active EP and device,
the model id and its manifest sha256, the window and token caps, and the
`UiInfo` block with the shape every service in the family uses.

### 5.1 How parts stream back

The decoder emits the document in reading order, so streaming is natural
and the protocol follows it:

1. Pages are admitted as they arrive. Window *k* prefills once its pages
   are all in; the client keeps uploading the later windows meanwhile
   (gRPC flow control holds it back at the buffer budget).
2. As tokens decode, an incremental parser (a C++ port of the rules in
   upstream's `process_unlimitedOCR2.py`: block grammar, bare-label
   repair, level and box order either way round, span-attribute repair,
   stray-token stripping) closes each block and sends a `Block`.
3. When the decoder emits the next `<PAGE>` the previous page is complete:
   the server sends its `PageDocument` fragment. Pages finish strictly in
   order, so a UI paints them top to bottom.
4. `cross_`/`append_` pairs and `<MERGE>` groups become `ContinuationLink`
   events the moment the second half appears. Fragments stay page-local
   and honest; the joining is stated, not baked in.
5. At the end, with `emit_document`, the folded `Document`: cross-page
   tables become one `TableItem` with a `ProvenanceItem` per page and the
   repeated header rows dropped, continued paragraphs become one
   `TextItem` with provenance on both pages, titles become
   `TitleItem`/`SectionHeaderItem` with the decoded level. Then
   `ParseComplete`.

### 5.2 Mapping to the fleet Document

| Block | Document item |
|---|---|
| TEXT, ASIDE, ALGORITHM | `TextItem`, label TEXT (ALGORITHM as CODE) |
| TITLE level 1 | `TitleItem`; level n > 1: `SectionHeaderItem` with level |
| PAGE_HEADER, PAGE_FOOTER, PAGE_NUMBER | `TextItem` with PAGE_HEADER or PAGE_FOOTER, furniture content layer |
| IMAGE, FIGURE, SEAL | `PictureItem`, label PICTURE |
| CHART | `PictureItem`, label CHART |
| TABLE | `TableItem` with `TableCell` row/column offsets and spans |
| IMAGE_CAPTION | `TextItem` CAPTION, linked as the picture's caption |
| IMAGE_FOOTNOTE, PAGE_FOOTNOTE | `TextItem` FOOTNOTE |
| EQUATION | `FormulaItem` |
| REFERENCE | `TextItem` REFERENCE |

Boxes convert from 0..1000 to the page's pixel size from `PageImage`.
Every item carries `CollectorSource{collector: "unlimited-ocr", model:
"Unlimited-OCR-2", version}` and a `GenerationSource` with token counts
and stop reason, so the merge keeps attribution.

## 6. gRParse integration

- **Contract.** Vendor `unlimited_ocr.proto` into `collectors/`, add it to
  the CMake proto list, and `examples/web-demo/sync-peer-protos.sh` for
  the shell.
- **Selection.** A new `COLLECTOR_UNLIMITED_OCR` in `parse_types.proto`
  (additive). Explicit selection only in the first release, like EBCDIC:
  never default-routed, because it is heavy and opt-in.
- **Feeding it.** gRParse already renders full-page PNGs through the PDF
  backend for `PROCESSING_PIPELINE_VLM` (`src/vlm_convert.cpp`). The new
  collector client reuses that page source and streams `PageImage`s in
  page order, then folds nothing itself: it asks for `emit_document` and
  takes the collector's own `Document`, the majority fold shape.
- **Merge.** A normal collector leg: additive, source-tagged, failure is a
  failure entry naming the collector and its gRPC status.
- **Later, scanned-page routing.** When grpc-pdf-inspector's typed
  `ocr_recommendation_reasons` say a page is scanned and the target is
  configured, a request option can fan those pages out to this collector
  beside the CV path, and the consensus machinery that already votes
  between PDF backends can take it as a voter. That is a separate change
  with its own scorecard evidence; it is not in the first release.
- **Shell tab.** Advertised through `UiInfo`, like every peer.

### 6.1 Relation to #162

#162 is open for one thing: page 2 of the quickstart sample is a real scan
and the shipped `ch_PP-OCRv3` recognizer reads "Figure l" at every render
scale. That is a line-recognizer problem inside the CV path and its fix is
a recognizer upgrade there (a newer PP-OCR recognizer, Apache-2.0, already
published as ONNX), measured by the scorecard document
`pdf-quickstart-sample`. It should land on its own and soon; it does not
wait for this service.

What this service adds to #162's family of problems: an independent
whole-page reading of scanned pages that can be used in `eval/` as an
oracle when judging recognizer changes, and later as a consensus voter.
It never becomes the default OCR, because the default stack must keep
running on a CPU box.

## 7. Repository and compose

Repo `grpc-unlimited-ocr`, default branch `main`, on GitHub
`ai-pipestream` and mirrored on Forgejo, both remotes pushed. Layout copied
from grpc-vlm-convert: `proto/`, `buf.yaml`, `src/`, `tests/`,
`Dockerfile` (CUDA), `Dockerfile.openvino`, `Dockerfile.cpu`,
`models/MANIFEST`, `scripts/fetch-models.sh`, `AGENTS.md`, `README.md`,
`NOTICE`. Image `pipestreamai/grpc-unlimited-ocr:latest` (`-openvino`,
`-cpu`) built by its own CI with per-arch buildx cache ids.

Configuration, all `GRPC_UOCR_*` environment variables: listen address
(default `0.0.0.0:50073`), `ORT_EP`, OpenVINO device and cache dir,
models dir, page window, per-page token cap, max pages and max page bytes
per stream, buffered-bytes budgets per stream and per server, documents
in flight.

A compose overlay keeps it out of the default stack and wires gRParse to
it only when used:

```yaml
# compose.stack.unlimited-ocr.yaml
services:
  unlimited-ocr:
    image: pipestreamai/grpc-unlimited-ocr:${STACK_TAG:-latest}
    build: ../grpc-unlimited-ocr
    expose:
      - "50073"
    environment:
      GRPC_UOCR_ORT_EP: "cuda"
      GRPC_UOCR_MODELS_DIR: "/models"
    volumes:
      - ${UOCR_MODELS_DIR:-./models/unlimited-ocr}:/models:ro
    gpus: all
    read_only: true
    tmpfs:
      - /tmp:size=64m
    cap_drop:
      - ALL
    security_opt:
      - no-new-privileges:true
  grparse:
    environment:
      GRPARSE_UNLIMITED_OCR_TARGET: "unlimited-ocr:50073"
```

The OpenVINO and CPU overlays already in the stack get matching entries
(`/dev/dri` and the kernel-cache named volume for OpenVINO). Diskless
while parsing: pages, tensors and output stay in memory; only the
read-only model mount and the OpenVINO kernel cache touch disk, and
neither is document data.

## 8. Tests

How tests run does not change: ctest inside the image build in this repo,
and the existing compose-driven gates in gRParse.

- **Unit (no model):** the incremental block parser over recorded model
  outputs and a fuzz corpus (it must never crash and must give the same
  blocks whether tokens arrive one at a time or all at once); the HTML
  table parser; `logit_policy` and `page_router` against values recorded
  from upstream's reference; the prompt builder's token layout; the
  Document fold and its cross-page joins.
- **Service (no model):** the gRPC server against an in-process fake
  engine behind the engine interface, covering ordering, back-pressure,
  caps, cancellation, `abort_on_error`, and failure codes, the way
  grpc-vlm-convert tests its server against a fake endpoint.
- **Model tier:** a golden test that runs real pages through the real
  graphs and checks parity with the committed goldens. It exits 77 (skip)
  without the model directory, like gRParse's layout golden, and a
  release run sets the require flag so a skip is a failure.
- **gRParse side:** a collector test against a fake built from the
  vendored stubs; the compose stack e2e (`scripts/stack-e2e.sh`) and the
  scorecard run with the overlay enabled; scanned scorecard documents
  scored with and without the leg. The scorecard is run twice after
  deploy, as for every parse-output change.

## 9. Phases and effort

One engineer-equivalent, in weeks. Each phase ends with a commit pushed to
both remotes and CI green.

| Phase | Work | Weeks | Exit gate |
|---|---|---|---|
| P0 spike | `uocr-export` for the vision encoder and a decoder step; parity on 5 golden pages; tok/s on 4080 CUDA, B70 OpenVINO, 9950X CPU | 1.5 | exact greedy parity on fp32 CPU; a measured tok/s row per EP. If SR-SWA cannot be expressed faithfully, fall back to Unlimited-OCR v1 (dense sliding-window attention) and re-plan |
| P1 engine | preprocess, tokenizer, KV cache with window rollover, page router, logit policy, MoE variants per EP | 2.5 | 20-page document decodes with parity to the goldens on CUDA |
| P2 output | contract under buf, incremental block parser, HTML tables, Document fold | 1.5 | unit and fuzz suites green; fold matches the goldens' structure |
| P3 service | gRPC server, budgets, `GetServiceInfo`, three Dockerfiles, CI, models manifest and image | 1.5 | service tests green; images published |
| P4 gRParse | vendored contract, collector client, `COLLECTOR_UNLIMITED_OCR`, overlay, shell tab, scorecard documents | 1.5 | stack e2e green with the overlay; scorecard runs recorded |
| P5 EP hardening | OpenVINO fp32 then fp16/int8 on real pages, CPU int4, perf baselines recorded | 1.5 | each EP passes the real-input gate or is documented as fp32-only |

About ten weeks in total; first usable (CUDA, explicit selection) at the
end of P4, about eight and a half weeks in. P0 is the only phase whose
outcome can change the plan, so nothing else starts until its gate is read.

## 10. Risks

- **Sparse attention fidelity.** SR-SWA is new, partly Triton kernels
  upstream, with a PyTorch reference path. The plan ports the reference
  path's semantics into the graph and proves them by token parity; v1 is
  the fallback.
- **MoE on OpenVINO.** No contrib op; the portable form may be slow for
  prefill. Measured in P0.
- **bf16 to fp16.** DeepSeek-family activations can overflow fp16. fp32
  is the reference on every EP until parity says otherwise.
- **Upstream churn.** Version 2 is a day old. The manifest pins exact
  weight hashes; a new upstream release is a deliberate bump with new
  goldens.
- **Training-data provenance** is unstated upstream. The licence is clear;
  the README says the provenance is not.
- **GPU sharing** with gRParse's own models on one card: separate
  processes with explicit ORT arena limits, documented in the overlay.
