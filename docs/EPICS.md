# Epics and tasks

Public tracker: **issues on this repository** (`ai-pipestream/gRParse`).  
Vocabulary stays product-neutral: compatible document JSON, layout ONNX, geometry bridge.

| Labels | Milestones |
|---|---|
| `epic`, `task`, `area:cpp`, `area:java`, `area:ui`, `area:ops` | `M1-chassis-pipeline` |
| `perf`, `models`, `office`, `geometry`, `contract` | `M2-offsets-ui-spike` … `M6-decoration-heatmaps` |

See [ARCHITECTURE.md](ARCHITECTURE.md) for the runtime split, anti-seesaw doctrine, and **ONNX Runtime EPs (NVIDIA CUDA + Intel Arc B70 / OpenVINO)**.

---

## Epic A — Streaming parse chassis & provenance (C++ core)

**Goal:** Diskless PDF/image → streamed page JSON with complete boxes + stable offsets.

| ID | Task | Layer |
|---|---|---|
| A1 | Provenance completeness (word/line boxes, page size, coord origin, confidence) | C++ |
| A2 | Streaming offset continuity (running UTF offsets / stable `#/texts/N`) | C++ |
| A3 | Digital-text-first router | C++ |
| A4 | Selective OCR integration + merge without duplicates | C++ |
| A5 | Honest capability flags (unknown options error/ignore; JSON always valid) | C++ |
| A6 | Backpressure & limits (in-flight pages, request size, queue metrics) | C++ |

**Fast:** OCR pool + page threads; digital path skips GPU; stream releases memory per page.  
**Milestone:** M1

---

## Epic B — Keep-the-machine-busy pipeline runtime (C++ perf)

**Goal:** Multi-stage scheduler so CPU render/parse and GPU nets overlap.

| ID | Task | Layer |
|---|---|---|
| B1 | Page job state machine | C++ |
| B2 | Stage worker pools (render/parse CPU, OCR GPU, layout GPU, post CPU) | C++ |
| B3 | Session lease reuse (generalize OCR pool to all ORT models) | C++ |
| B4 | Pipeline metrics (queue depth, wait, GPU busy%, latency histogram) | C++ |
| B5 | Memory lifetime rules (drop raster after last device stage; arena-per-page) | C++ |
| B6 | ORT EP selection: CUDA + Intel OpenVINO (Arc B70) + CPU fallback | C++ / ops |

**Fast:** this epic *is* the anti-seesaw work. Model tasks must not hard-code serial page loops.  
Device pools target **NVIDIA CUDA and Intel Arc B70 (OpenVINO EP)** — not CUDA-only.  
**Milestone:** M1

---

## Epic C — Layout regions ONNX (C++)

**Goal:** Region labels (title, section, list, table, picture, caption, formula, header/footer, …).

| ID | Task | Layer |
|---|---|---|
| C1 | Obtain/export layout ONNX + label map (verify license) | C++ / ops |
| C2 | Layout ORT session pool | C++ |
| C3 | Assemble labeled items; attach words into regions | C++ |
| C4 | Reading-order rules (multi-column); stable body children | C++ (prefer) |
| C5 | Golden structure tests (not ~100% plain text labels) | C++ |

**Fast:** layout GPU on *N* while render fills *N+1*.  
**Milestone:** M3 · **Depends:** B (pools)

---

## Epic D — Tables

**Goal:** Non-empty `tables[]` with cells/spans when structure is on.

| ID | Task | Layer |
|---|---|---|
| D1 | Table crops from layout | C++ |
| D2 | Word→cell assignment v0 (geometry-only) | C++ |
| D3 | Table structure ONNX (SLANet/TATR-class) | C++ |
| D4 | Emit compatible table JSON | C++ |
| D5 | Native tables shortcut (POI when no geometry needed) | **Java** |

**Fast:** net only on table crops; CPU cell-fill overlaps next GPU page.  
**Milestone:** M4 · **Depends:** C

---

## Epic E — Pictures & figure class

**Goal:** Picture items with bboxes; optional type scores.

| ID | Task | Layer |
|---|---|---|
| E1 | Picture items from layout | C++ |
| E2 | Figure classifier ONNX | C++ |
| E3 | Barcode decode optional (ZXing/ZBar) | C++ |
| E4 | Java policy hooks (signature/PII, chart routing, RAG filter) | **Java** |

**Fast:** tiny classifier; never block stream on captions/VLMs.  
**Milestone:** M4 · **Depends:** C

---

## Epic F — Office bridge, diskless (Java + ops)

**Goal:** Office bytes → structure or PDF bytes without durable disk.

| ID | Task | Layer |
|---|---|---|
| F1 | POI fast path (DOCX/PPTX/XLSX) | **Java** |
| F2 | Pooled Office→PDF (stream API, TMPDIR=tmpfs) | **Java** |
| F3 | PDF bytes → gRParse gRPC (chunked; no temp PDF on disk) | **Java** |
| F4 | Optional same-host memfd (only if profiling demands) | C++/Java |
| F5 | Escalation policy (`need_geometry` → LO→PDF→gRParse else POI) | **Java** |

**Fast:** warm LO pool; parallel POI for NLP-only.  
**Milestone:** M5

---

## Epic G — Geometry bridge & OpenNLP decoration (Java)

**Goal:** Parse JSON → OpenNLP Document + geometry layer.

| ID | Task | Layer |
|---|---|---|
| G1 | Adapter parse JSON → OpenNLP Document | **Java** |
| G2 | Geometry bridge records | **Java** (+ optional proto) |
| G3 | Stream-friendly append | **Java** |
| G4 | Annotator pack (embeddings, geo, PII, numeric, …) | **Java** |
| G5 | Offset integrity tests | **Java** |

**Fast:** virtual threads on page events; no pixel work in JVM.  
**Milestones:** M2 (G2/G3), M6 (G4)

---

## Epic H — Visual UI: highlights & heatmaps

**Goal:** Lexical highlight + semantic heatmap on page geometry.

| ID | Task | Layer |
|---|---|---|
| H1 | Page image channel | C++ and/or Java |
| H2 | Lexical highlight | **UI + Java** |
| H3 | Semantic heatmap | **UI + Java** |
| H4 | Stream UX (paint page N early) | **UI** |

**Fast:** UI follows stream; span-level scoring only.  
**Milestones:** M2 (H2), M6 (H3)

---

## Epic I — Contract, JSON compatibility, packaging

**Goal:** Stable schema + client story.

| ID | Task | Layer |
|---|---|---|
| I1 | Document JSON schema doc (**low priority** — gRPC toJSON is enough) | docs |
| I2 | Compatibility fixtures (light; only what tests need) | C++/Java |
| I3 | Protos home (`pipestream-protos` when ready) | ops |
| I4 | README refresh (architecture + epic links; speed thesis; CUDA + Intel EP) | docs |
| I5 | Keep docs/ARCHITECTURE.md and docs/EPICS.md in sync with issues | docs |

---

## Milestone order

1. **M1** — A + B (chassis + pipeline)
2. **M2** — G2/G3 + H2 (offsets → highlight spike)
3. **M3** — C (layout)
4. **M4** — D then E (tables, pictures)
5. **M5** — F (office)
6. **M6** — G4 + H3 (decoration + heatmaps)

## Explicit non-epics

- Torch/VLM caption stacks as default
- Embedding LibreOffice inside the OCR process
- Second ORT runtime in Java for page CV
- Universal office clone inside C++

## Issue number map

| Key | GitHub issue |
|---|---|
| Epic A | #1 |
| Epic B | #2 |
| Epic C | #3 |
| Epic D | #4 |
| Epic E | #5 |
| Epic F | #6 |
| Epic G | #7 |
| Epic H | #8 |
| Epic I | #9 |
| A1 | #10 |
| A2 | #11 |
| A3 | #12 |
| A4 | #13 |
| A5 | #14 |
| A6 | #15 |
| B1 | #16 |
| B2 | #17 |
| B3 | #18 |
| B4 | #19 |
| B5 | #20 |
| B6 | #21 |
| C1 | #22 |
| C2 | #23 |
| C3 | #24 |
| C4 | #25 |
| C5 | #26 |
| D1 | #27 |
| D2 | #28 |
| D3 | #29 |
| D4 | #30 |
| D5 | #31 |
| E1 | #32 |
| E2 | #33 |
| E3 | #34 |
| E4 | #35 |
| F1 | #36 |
| F2 | #37 |
| F3 | #38 |
| F4 | #39 |
| F5 | #40 |
| G1 | #41 |
| G2 | #42 |
| G3 | #43 |
| G4 | #44 |
| G5 | #45 |
| H1 | #46 |
| H2 | #47 |
| H3 | #48 |
| H4 | #49 |
| I1 | #50 |
| I2 | #51 |
| I3 | #52 |
| I4 | #53 |
| I5 | #54 |

**Notes**
- Public tracker: https://github.com/ai-pipestream/gRParse/issues
- **I1** low priority — prefer gRPC **toJSON** over a standalone JSON schema program.
- **B6** — ORT EPs: NVIDIA CUDA + **Intel Arc B70 (OpenVINO)** + CPU.
