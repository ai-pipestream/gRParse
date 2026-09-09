# Releasing gRParse

How a version gets cut, what the publish workflow does with it, which tags
appear where, and how the compose stack pins one. The workflow itself is
`.github/workflows/publish.yml`; its header comment is the authoritative
description of the build-gate-name sequence.

## One version, one place

The version lives on the `project()` line of `CMakeLists.txt`:

```cmake
project(gRParse VERSION 0.2.0 LANGUAGES CXX C)
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

## Cutting a release

1. Bump the version in one code commit (no `[ci skip]`: the push to `main`
   republishes `:latest`, which is fine, and a docs marker on the release
   commit would silence the tag build too). Nothing else needs editing.

   ```bash
   sed -i 's/^project(gRParse VERSION [0-9.]*/project(gRParse VERSION 0.2.0/' CMakeLists.txt
   git commit -am "Release 0.2.0"
   git push origin main && git push github main
   ```

2. Tag that commit `v<version>` and push the tag to both remotes. GitHub
   runs the workflow (the org secrets live there); Forgejo is the source of
   truth and its push mirror force-syncs GitHub, so a tag that exists only
   on GitHub can vanish on the next mirror sync.

   ```bash
   version=$(scripts/cmake-version.sh)
   git tag -a "v$version" -m "gRParse $version"
   git push origin "v$version" && git push github "v$version"
   ```

3. Watch the `Publish Image` run on GitHub. It resolves the version from
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

4. Verify what was published:

   ```bash
   docker buildx imagetools inspect pipestreamai/grparse:0.2.0-cpu   # linux/amd64, linux/arm64, attestations
   docker run --rm -d --name grparse-check -p 50051:50051 \
     -v "$PWD/models:/models:ro" pipestreamai/grparse:0.2.0-cpu
   grpcurl -plaintext localhost:50051 ai.pipestream.parse.v1.ParseService/GetServiceInfo
   docker rm -f grparse-check
   ```

   The version field reads `grparse-0.2.0-cpu`.

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
linux/amd64). Every published image carries SLSA provenance (`mode=max`)
and an SBOM attestation, visible in `imagetools inspect` as one
attestation manifest per platform.

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
STACK_TAG=0.2.0 docker compose -f compose.stack.yaml pull
STACK_TAG=0.2.0 docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
```

renders `pipestreamai/grparse:0.2.0-cpu`, `pipestreamai/grpc-asr:0.2.0-cpu`,
`pipestreamai/grpc-libreoffice:0.2.0`, and so on. Unset, the stack tracks
`:latest` as before, and `up --build` still builds the sibling checkouts
under the same names. The pin is fleet-wide: it assumes every sister repo
cut the same version (each has its own `publish.yml` with a
`workflow_dispatch` version input; gRParse alone takes the version from a
git tag). Until the fleet releases in step, pin the stack to a version only
after each sibling has that tag on Docker Hub, or override single services
on the command line.
