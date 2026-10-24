# Releasing gRParse

How a version gets cut, what the publish workflow does with it, which tags
appear where, and how the compose stack pins one. The workflow itself is
`.github/workflows/publish.yml`; its header comment is the authoritative
description of the build-gate-name sequence.

## One version, one place

The version lives on the `project()` line of `CMakeLists.txt`:

```cmake
project(gRParse VERSION 0.3.0 LANGUAGES CXX C)
```

Everything else derives from it at build time. The server reports
`grparse-<version>-<flavor>` from `GetServiceInfo` (`cuda`, `cpu`, or
`openvino`, from the ONNX Runtime package the image links), the bundle
manifest names `grparse/<version>` as its generator, and
`streaming-service-test` asserts the served string against the same compile
definitions, so a version that drifts from CMake fails the in-build test
battery. `scripts/cmake-version.sh` prints the version; with
`--expect <version>` it exits 1 when the two disagree, and that is the gate
the publish workflow runs first.

## The fleet release train

A release is one version across the whole family: gRParse and every
repository in `release/fleet.tsv` (the parsers, their viewer UIs and
fastwarc-grpc) get the same annotated `v<version>` tag, each on its own
publish branch, so `STACK_TAG=<version>` pulls a consistent stack. The
version is gRParse's CMake version; the parser repositories keep their own
crate or Gradle versions, and the fleet tag is what names their images.

- **Versioning.** Semver, 0.x for now: bump the minor version when a
  contract changes (a `.proto` field, the output shape), the patch version
  otherwise. The release notes put every commit that touched a `.proto`
  under "Contract", so the call is easy to check.
- **Cadence.** Every two weeks, or sooner after a clean battery run. Cut a
  release only from commits whose CI is green on both forges;
  `scripts/release.sh preflight` refuses anything else.
- **Who.** One person, from krick, about 30 minutes of attention plus the
  publish runs. Nothing creates a tag on its own.
- **Order.** Forgejo first, then GitHub, for every tag: the push mirror
  force-syncs GitHub from Forgejo, so a tag that exists only on GitHub can
  vanish on the next sync.

What each piece does:

| Piece | Role |
|---|---|
| `release/fleet.tsv` | the repositories, the branch each tag goes on, and the images each one publishes |
| `scripts/release.sh` | `bump`, `preflight` and `tag` for the whole fleet, run from this checkout on krick |
| each repo's publish workflow | on a `v*` tag push, publishes `:<version>` (and variant suffixes) beside `:latest` |
| `scripts/release-stack.py` | checks that every `${STACK_TAG}` image in the compose stack has a publisher, and resolves the published tags to digests |
| `.github/workflows/release.yml` | after gRParse's tag publish succeeds, waits for the fleet's images and cuts the GitHub Release with notes and `stack-<version>.lock` |
| `scripts/release-notes.sh` | the gRParse commits since the previous tag, grouped |

Adding a parser to the stack means adding its line to `fleet.tsv` in the
same change; the `Release machinery` check fails a pull request that adds
a `${STACK_TAG}` image no fleet repository publishes.

## Cutting a release

The one-person checklist. Run it from the gRParse checkout on krick, with
the sibling checkouts next to it (the layout `compose.stack.yaml`'s
`build:` lines expect; `FLEET_ROOT` overrides), remotes `origin` (Forgejo)
and `github` in each, and `gh` logged in.

1. Land what belongs in the release on every repository's publish branch,
   Forgejo first, and let each branch's CI finish green.

2. Bump the version in one code commit (no `[ci skip]`: the push to `main`
   republishes `:latest`, which is fine, and a docs marker on the release
   commit would silence the tag build too). Nothing else needs editing.

   ```bash
   scripts/release.sh bump 0.3.0 --push
   ```

   That commits `Release 0.3.0` and pushes main to both remotes. Wait for
   that commit's CI ("C++ build and contract tests", all three image
   builds) to go green before going on.

3. Check the whole fleet, read-only:

   ```bash
   scripts/release.sh preflight 0.3.0
   ```

   For every repository it fetches both remotes and fails on a missing
   checkout, a dirty tree, the wrong branch, a branch that differs between
   local, Forgejo and GitHub, a tag that already exists, or a GitHub check
   run on the head commit that is pending or failed. It prints the commit
   each tag will point at. Fix what it names and run it again.

4. Tag and push:

   ```bash
   scripts/release.sh tag 0.3.0          # dry run: preflight plus the plan
   scripts/release.sh tag 0.3.0 --yes    # annotated tags, Forgejo then GitHub
   ```

   Every repository's publish workflow starts on GitHub as its tag lands.

5. Watch the `Publish Image` run on GitHub. It resolves the version from
   the tag (leading `v` stripped), refuses to continue if it is not the
   CMake version, builds every leg with the test suite, boot-proofs each
   one, and only then creates the tags. About 45 minutes per leg; the legs
   run in parallel. Every leg's smoke test is CPU-only by design (no GPU
   runner exists); before declaring an Intel-image release good, run the
   structural scorecard against it on a render host with the GPU exposed
   (AGENTS.md section 4; krick-1 is the fleet's), then scrape its `/metrics`:
   `grparse_ort_ep_build_retries_total` may be non-zero on a contended host
   (OpenVINO builds retry under pressure, see README "Intermittent IGC
   failures and VRAM headroom"), and `grparse_ort_ep_fallbacks_total` must
   reflect only the pre-decided table-structure retreat; the openvino flavor
   never degrades OCR to CPU, so anything beyond the table pool is a failed
   leg, not a fallback. Confirm the host has real VRAM headroom: a co-tenant
   holding the card near-full turns kernel compiles into crashes (exit 139).

6. Verify what was published:

   ```bash
   docker buildx imagetools inspect pipestreamai/grparse:0.3.0-cpu   # linux/amd64, linux/arm64, attestations
   docker run --rm -d --name grparse-check -p 50051:50051 \
     -v "$PWD/models:/models:ro" pipestreamai/grparse:0.3.0-cpu
   grpcurl -plaintext localhost:50051 ai.pipestream.parse.v1.ParseService/GetServiceInfo
   docker rm -f grparse-check
   ```

   The version field reads `grparse-0.3.0-cpu`.

7. The GitHub Release cuts itself. When the tag's `Publish Image` run
   succeeds, `.github/workflows/release.yml` checks that every fleet
   repository carries the tag, waits up to 90 minutes for every image in
   `fleet.tsv` to exist with the version tag, and publishes the Release
   with the notes, the fleet commits, the image digests and
   `stack-<version>.lock`. If something is still missing when it gives
   up, the Release stays a draft that names it and the run fails; publish
   the straggler (rerun its tag's publish run), then run the Release
   workflow by hand with the version. A failed tag publish in a parser is
   fixed forward in that repository and republished with a dispatch
   carrying the version; the tag is not moved.

A tag is never created or pushed by automation; a human cuts it.

## What the workflow publishes

| Trigger | Tags on `docker.io/pipestreamai/grparse` |
|---|---|
| push to `main` | `latest`, `latest-cpu`, `latest-openvino` |
| push of tag `v<version>` | the three above, plus `<version>`, `<version>-cpu`, `<version>-openvino` |
| `workflow_dispatch` with the `version` input | the same as a tag push (the input must equal the CMake version) |
| `workflow_dispatch` without it | the same as a `main` push |

`latest` / `<version>` is the CUDA image (`Dockerfile`, linux/amd64).
`latest-cpu` / `<version>-cpu` is a manifest list of linux/amd64 and
linux/arm64, each leg built and tested natively on its own runner
(`ubuntu-latest`, `ubuntu-24.04-arm`), no emulation. `latest-openvino` /
`<version>-openvino` is the Intel image (`Dockerfile.openvino`,
linux/amd64). The demo shell (`examples/web-demo/Dockerfile`) publishes
in the same run as `docker.io/pipestreamai/grparse-demo-shell:latest`, and
`:<version>` on a release, a linux/amd64 + linux/arm64 manifest list whose
legs each load every peer contract inside the pushed digest before it is
named. Every published image carries SLSA provenance (`mode=max`)
and an SBOM attestation, visible in `imagetools inspect` as one
attestation manifest per platform.

A `v<version>` tag push also publishes `pipestreamai/grparse-models:<version>`
(`publish-models.yml`) beside its `:latest`, so the models volume pins with
the rest of the stack.

The same tags land on the Forgejo registry,
`git.rokkon.com/ai-pipestream/grparse`, when the `CICD_TOKEN` secret exists
on the GitHub repository; without it the Forgejo login and names are
skipped and Docker Hub alone is published.

Publication order is fixed: each leg pushes its image by digest only, with
no tag pointing at it, pulls that digest back and runs
`scripts/smoke-test.sh` on it, and hands the digest to
`scripts/publish-manifests.sh`, which creates the tagged manifest lists and
fails unless the result lists exactly the platforms the run built with their
attestations. A leg whose smoke test fails never becomes a tag; a failed
variant does not block the others.

## Pinning the stack to a release

`compose.stack.yaml` and its overlays take one variable, `STACK_TAG`
(default `latest`), for every `pipestreamai/*` image, variant suffixes
included:

```bash
STACK_TAG=0.3.0 docker compose -f compose.stack.yaml pull
STACK_TAG=0.3.0 docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
```

renders `pipestreamai/grparse:0.3.0-cpu`, `pipestreamai/grpc-asr:0.3.0-cpu`,
`pipestreamai/grpc-libreoffice:0.3.0`, and so on. Unset, the stack tracks
`:latest` as before, and `up --build` still builds the sibling checkouts
under the same names. The pin is fleet-wide: every image in the stack is published with the
version tag by the repository `release/fleet.tsv` names, so a release pins
the whole stack. `stack-<version>.lock` on the GitHub Release lists the
same images by digest, for when a tag has to be checked against what was
released.
