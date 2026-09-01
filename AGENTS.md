# AGENTS.md: gRParse

gRParse is the coordinator of the ai-pipestream parser fleet: a diskless C++
gRPC service that turns documents into a page-streamed
`ai.pipestream.document.v1.Document`, parsing PDFs and rasters itself (the CV
path) and scattering every other format to sister services it dials over
gRPC. It owns the fleet-wide `document.proto` and the demo shell that ties the
fleet together. This file is about the *family*: which repositories belong to
it, what gRParse takes from each, and how to lay a workspace out so that the
build, the tests and the compose stack find them.

## Read this first, in order

1. This file
2. `README.md`, sections "Collector scatter-gather" (the routing table and
   the folding rules) and "Development" (build, CTest labels, sanitizers)
3. `collectors/README.md`: the vendored wire contracts and the byte-identical
   rule
4. `docs/COLLECTORS.md`: the collector strategy (gRPC-first, coordination,
   stream joining)
5. The header comment of `compose.stack.yaml`: profiles and what each one
   brings up
6. `eval/README.md` only when touching parse quality (the VLM oracle harness)

## The family

All repositories live under the GitHub organisation
`https://github.com/ai-pipestream/` and are mirrored one-to-one on the
Forgejo instance `https://git.rokkon.com/ai-pipestream/`. The GitHub URL is
`https://github.com/ai-pipestream/<repo>.git`; the Forgejo URL is
`https://git.rokkon.com/ai-pipestream/<repo>.git`. Every checkout carries
both remotes (`origin` = Forgejo, `github` = GitHub) and every push goes to
both.

### Collectors gRParse dials

gRParse reaches each through `GRPARSE_<NAME>_TARGET=<host:port>`; unset means
the collector does not exist in that deployment. The "vendored" column is the
file under `collectors/` copied byte-identical from the sister repo.

| Repo | Default branch | Language | Port | Vendored in `collectors/` | Routed by default for |
|---|---|---|---|---|---|
| `grpc-libreoffice` | `main` | C++ (CMake, LibreOfficeKit) | 50053 | none (contract in `office_service.proto` here; gRParse folds its typed events itself) | office formats |
| `grpc-pdf-inspector` | `main` | Rust (Cargo, buf) | 50067 | `pdf_types.proto`, `pdf_service.proto` | PDF routing oracle: classifies, and text PDFs take its fast path |
| `grpc-email` | `main` | Java (Gradle, buf) | 50054 | `email_service.proto` | `.eml`, `.msg`, `message/rfc822` |
| `grpc-xml` | `main` | Rust (Cargo, buf) | 50066 | `xml.proto`, `xml_service.proto` | `.xml`, `.nxml`, `.xbrl`, METS/GBS archives |
| `grpc-epub` | `main` | Rust (Cargo, buf) | 50064 | `epub_types.proto`, `epub_service.proto` | `.epub`; returns a skeleton by contract, gRParse folds the chapters through `grpc-markup` and inlines the images (`src/epub_book.cpp`), so a working epub parse needs both targets |
| `grpc-markup` | `main` | Rust (Cargo, buf) | 50065 | `markup.proto`, `markup_service.proto` | `.md`, `.html`, `.adoc`, `.tex`, `.vtt`, `.boxnote`, `.json` |
| `grpc-ebcdic` | `main` | Rust (Cargo, buf) | 50063 | `ebcdic.proto`, `ebcdic_service.proto` | never by format; explicit `ebcdic_layout_json` only |
| `grpc-lol-html` | `master` | Rust (Cargo, buf) | 50057 | `lolhtml_types.proto`, `lolhtml_service.proto` | never by format; explicit `lol_html_options_json` only |
| `grpc-asr` | `main` | C++ (CMake, buf, whisper.cpp) | 50055 | `asr_service.proto` | audio and video (`GRPARSE_ASR_MODEL` required) |
| `fastwarc-grpc` | `main` | Rust (Cargo) | 50060 | `warc.proto`, `warc_service.proto` | `.warc*`; see the fastwarc caveat below |

Two collectors are compiled in and have no repo: the CV path
(`COLLECTOR_GRPARSE_CV`) and the wiki storage XHTML handler
(`COLLECTOR_CONFLUENCE`).

### Shell peers gRParse does not dial

They run in the stack and get a tab in the demo shell
(`examples/web-demo`), which dials them directly. gRParse never calls them.
The merge already ranks `poi` and `calamine` claims below gRParse's own
(`document_claim_rank` in `src/document_merge.cpp`), so wiring the first two
in as collectors is the open item, not a schema change.

| Repo | Default branch | Language | Port | Shell env | Role |
|---|---|---|---|---|---|
| `grPOIc` | `main` | Java/Kotlin (Gradle) | 50052 | `POIC_TARGET` | Apache POI over office documents |
| `grpc-calamine` | `main` on GitHub (`development` was the working branch; check) | Rust (Cargo, buf) | 50062 | opt-in `calamine` compose profile | spreadsheets; meant to be hosted by an external project, so nothing in that repo may mention gRParse or the shell |
| `grpc-enrich` | `main` | Java (Gradle, buf) | 50056 gRPC, 50068 HTTP | `ENRICH_TARGET` | `Document` in, stream of `ItemAnnotation` out; needs `ENRICH_VLM_URL` |
| `grpc-vlm-convert` | `main` | C++ (CMake, buf) | 50058 gRPC, 50059 HTTP | `VLM_CONVERT_TARGET` | calls an external VLM server; also hosts the open VLM serving stack under `serving/` used by `eval/` |

### Adjacent, not family

- `chatnoir-resiliparse` (upstream, branch `develop`) and the
  `ai-pipestream/chatnoir-resiliparse` fork (branch `develop`, gRPC server on
  its `fastwarc-grpc` branch, port 50061): the WARC parser `fastwarc-grpc`
  wraps. Not run by the stack.
- `grpc-confluence`: a Confluence connector (Cloud sync, MCP runtime), not a
  parser. Not part of the family yet; no port, no tab, no collector.
- The `calamine` library fork and `distributed-search`: libraries and
  experiments, not services gRParse depends on.

### The fastwarc caveat

fastwarc is a collector on paper and a shell peer in the stack. The
`fastwarc.v1` dialect vendored here is not wire-compatible with the published
`pipestreamai/fastwarc-grpc` image (same proto package, different field
numbers), so `compose.stack.yaml` deliberately leaves
`GRPARSE_FASTWARC_TARGET` unset and the shell dials `FASTWARC_TARGET` itself.
Reconciling the two contracts is the fix; wiring the target without it fails
every WARC upload.

## Setting up the workspace

### 1. Lay the repos out as flat siblings

`compose.stack.yaml` builds every sister from `../<repo>` (for example
`build: ../grpc-libreoffice`), and the worktree workflow below puts feature
checkouts in `../worktrees/`. So the family must be checked out side by side
under one directory, with gRParse one of the siblings:

```
<workspace>/
  gRParse/                 this repo
  grpc-libreoffice/
  grpc-pdf-inspector/
  grpc-email/  grpc-xml/  grpc-epub/  grpc-markup/  grpc-ebcdic/
  grpc-lol-html/  grpc-asr/  fastwarc-grpc/
  grPOIc/  grpc-calamine/  grpc-enrich/  grpc-vlm-convert/
  worktrees/               feature worktrees, one per repo-feature
```

The directory above is a plain directory, not a git repository. There is no
top-level build; build and test inside each repo with its own toolchain.

```bash
WS=/work/main/grpc-services            # any path; the layout is what matters
mkdir -p "$WS/worktrees" && cd "$WS"
for repo in gRParse grpc-libreoffice grpc-pdf-inspector grpc-email grpc-xml \
            grpc-epub grpc-markup grpc-ebcdic grpc-lol-html grpc-asr fastwarc-grpc \
            grPOIc grpc-calamine grpc-enrich grpc-vlm-convert; do
  [ -d "$repo" ] || git clone "https://git.rokkon.com/ai-pipestream/$repo.git"
  git -C "$repo" remote get-url github >/dev/null 2>&1 || \
    git -C "$repo" remote add github "https://github.com/ai-pipestream/$repo.git"
done
```

Without Forgejo access, clone from GitHub and add the Forgejo URL as
`origin` later; the names matter because the push rule below uses them.

### 2. Check the branch before basing work

Default branches are `main` except `grpc-lol-html` (`master`) and the
chatnoir repos (`develop`); `grpc-calamine` moved its GitHub `HEAD` to `main`
but has carried long-lived branches. A checkout may be sitting on a feature
branch: run `git -C <repo> branch --show-current` first, never assume.

### 3. Verify the wire contracts agree

`document.proto` in this repo is the fleet's source of truth. Every collector
and shell peer that speaks the Document plane vendors a byte-identical copy at
`proto/ai/pipestream/document/v1/document.proto`. Check the family before
touching any proto and after every schema change:

```bash
cd gRParse
md5sum document.proto ../grpc-*/proto/ai/pipestream/document/v1/document.proto \
  | awk '{print $1}' | sort -u | wc -l          # must print 1
```

A schema change is a fleet sweep: edit `document.proto` here, re-copy it into
every vendoring repo (the eight collectors whose contracts import it, so all
but grpc-lol-html and fastwarc-grpc, plus grpc-enrich and grpc-vlm-convert),
run each repo's own gate (`buf format`/`buf lint` are CI gates in the Rust
repos), commit and push each one to both remotes. The same rule runs the
other way for `collectors/`: each collector owns its service contract, the
copy here is never edited, and a copy that drifts is worse than none because
the tests dial fakes built from the same stubs.

### 4. Build and test gRParse

The container is the supported build environment (Ubuntu with CUDA, ONNX
Runtime GPU, gRPC; see README "Development"). Three images exist:
`Dockerfile` (CUDA, tag `latest`), `Dockerfile.cpu` (multi-arch,
`latest-cpu`), `Dockerfile.openvino` (Intel GPUs, `latest-openvino`).

```bash
docker compose build                                   # CUDA image + tests
ctest --test-dir /build --output-on-failure -L grparse # inside the build; the label scopes it
scripts/smoke-test.sh pipestreamai/grparse:latest      # boot-proof a built image
```

Optional test tiers that skip silently without their prerequisite: the
layout golden (needs the model file), the VLM oracle eval (test `vlm-oracle-eval`, label `eval`,
needs `VLM_ENDPOINT`), the structural scorecard (test `structural-scorecard`, label
`eval`, needs a running gRParse at `GRPARSE_TARGET`; see `eval/README.md`), the
vlm-convert golden. A green run proves nothing about a tier that skipped; read
the skip counts. The scorecard is the gate for any change to parsing output:
run it twice after a deploy (the first run carries warm-up latency), read the
Changes section, and re-record a legitimate move in the same change with
`--record --repeat 2 --only <docs> --reason "<commit>: why"`. It scores
against hand-derived truth files (`eval/scorecard/truth/`) whose floors only
ratchet up, gates latency (`EVAL_LATENCY=off` on CPU instances) and
stability (`--repeat 2`), and `EVAL_REQUIRE=1` makes a skip a failure for
release runs.

The build tree lives in a BuildKit cache mount keyed by toolchain. Two builders
on one host that share it (a developer build and a local CI run of the same
Dockerfile) can leave each other's objects newer than the sources, and ninja
then skips a recompile while the in-build ctest passes on the old binary. Check
that the build log compiled the file you changed (`grep 'Building CXX'`), and
give a second builder its own tree with
`--build-arg GRPARSE_BUILD_CACHE_SCOPE=ci`. Every worktree that builds gets
its own scope (`-<feature>`), and an acceptance build uses a fresh scope: even
inside one scope an object compiled after a header was edited keeps its newer
mtime and ninja keeps it.

### 5. Run the stack

```bash
cd gRParse
docker compose -f compose.stack.yaml up --build                             # core: grparse, libreoffice, lol-html, pdf-inspector, shell
docker compose -f compose.stack.yaml --profile parsers --profile heavy up   # + every collector and the shell peers
# overlays, stackable:
#   -f compose.stack.expose-grpc.yaml   publish gRParse gRPC on the host (50051)
#   -f compose.stack.cpu.yaml           CPU image
#   -f compose.stack.openvino.yaml      Intel GPU image (OpenVINO, /dev/dri)
```

Only the nginx proxy publishes a port (8080); services reach each other by
compose service name. The shell bakes the protos into its image: after any
schema change, `build shell` and `up -d shell`, a restart is not enough.

### 6. Work in worktrees

Non-trivial work happens in a worktree, never in the sibling checkout:

```bash
git -C gRParse fetch origin
git -C gRParse worktree add ../worktrees/gRParse-<feature> -b <feature> origin/main
```

Commit inside the worktree; merge and push from the main checkout. Remove the
worktree and the local branch when the feature lands. A private instance of a
worktree's image runs against the stack's collectors without touching the
stack: `docker run --network parse-stack_default -e GRPARSE_ORT_EP=cpu` with
the `GRPARSE_*_TARGET` values from `compose.stack.yaml` and the models
directory mounted, then `run.py --target localhost:<port>`.

## Rules that hold across the family

- **Diskless while parsing.** Document bytes never touch a filesystem in a
  service: uploads stay in memory, page renders are in-memory buffers, and a
  service that needs a path (LibreOfficeKit) uses a RAM-backed tmpfs and
  refuses to start without one. The only sanctioned sink is S3, and only when
  a request explicitly configures an `S3Target`; credentials come from the
  request, are never logged and never appear in errors. Model files and
  compiled kernel caches (`GRPARSE_MODELS_DIR`, `GRPARSE_OPENVINO_CACHE_DIR`)
  are not document data.
- **Push to both remotes** after every commit: `git push origin` and
  `git push github`. GitHub prints "Bypassed rule violations" for direct
  pushes; that is expected.
- **Commit messages carry no em dashes** (a commit-msg hook rejects them) and
  no tool or model attribution.
- **Exit codes are the verdict.** Never pipe a build or test through a grep
  filter to judge it; a stale binary makes `ctest` say "passed" on a failed
  build.
- **Typed claims, never keyed strings.** Conflicting collector data lives in
  `Document.claims` plus `field_sources`; there are no `poi:title`-style keys.
- **Copy from a sibling, do not invent.** New collectors copy the process of
  an existing one (diskless, health RPC, `UiInfo` block with the same shape,
  buf-managed protos, own default port from the workspace table) and own
  their contract; gRParse vendors it.
- **Images are `pipestreamai/<service>:latest`** on Docker Hub, built by each
  repo's own CI; multi-arch legs share buildx cache mounts by id, so key the
  ids on the target architecture.
