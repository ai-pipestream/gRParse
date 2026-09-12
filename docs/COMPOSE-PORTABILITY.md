# Published-image Compose portability

This is the supported path for running the published parser stack without
sibling checkouts. It uses the existing `compose.stack.yaml` base and its
overlays; do not use `--build`. The `image:` entries are the published
`pipestreamai/*` images, and the demo-shell image already contains the peer
protobufs it needs. `compose.stack.standalone.yaml` only replaces the ASR
weights bind path for a checkout-free host.

Run `scripts/compose-preflight.sh` first. It is config-only: it validates the
overlay combinations below and does not pull, start, stop, or inspect a
container. Runtime checks are intentionally separate and require an existing
gRPC endpoint:

```sh
scripts/compose-preflight.sh
scripts/compose-preflight.sh --runtime --target localhost:50051
```

The runtime form uses the checked-in protobuf contract to call `Health`,
`GetServiceInfo`, both synchronous chunk endpoints, and the existing
`grparse-stream-client` for a streamed fixture. It requires `grpcurl`, the
client binary (or `GRPARSE_STREAM_CLIENT`), and `timeout` (`gtimeout` from
coreutils on macOS). It neither creates nor restarts anything. The current
published `ParseService` contract has no embedding RPC, so the script reports
embedding checks as skipped. This lane does not make a claim about the pending
`local-embeddings` work.

## Common setup

Use a single fleet release only after every service has published that tag.
`STACK_TAG=0.2.0` is a fleet-wide assumption, not a gRParse-only version pin;
leave it unset to use `latest`. Models are required. The model overlay avoids
a host `models/` directory by using the verified `pipestreamai/grparse-models`
init image.

```sh
export STACK_TAG=0.2.0             # optional; only a coordinated fleet tag
docker compose -f compose.stack.yaml \
  -f compose.stack.cpu.yaml \
  -f compose.stack.standalone.yaml \
  -f compose.stack.models.yaml up
```

The last overlay must be `compose.stack.models.yaml`; it replaces the base
`./models:/models` bind mount. Add `compose.stack.expose-grpc.yaml` only when
an external client needs plaintext gRPC on trusted networks. Core operation
needs no sibling checkout. The optional `heavy` ASR profile additionally needs
Whisper weights at `./protos/grpc-asr/models`; the `parsers`, `calamine`, and
`pdf-backends` profiles add their named collectors/backends. The base's
collector target variables are deliberately wired to profile services, so
requesting a format whose profile is down fails loudly.

## Platform recipes

| Host | Overlay order and expected accelerator | Status |
| --- | --- | --- |
| macOS, Apple Silicon | `base`, `cpu`, `arm64`, `standalone`, `models` | Native arm64 CPU. Do not add OpenVINO. `pdf-backends` is optional amd64 emulation. |
| macOS, Intel | `base`, `cpu`, `standalone`, `models` | CPU only. Docker Desktop supports both Mac architectures, but this stack has no macOS GPU overlay. |
| Linux amd64, CPU | `base`, `cpu`, `standalone`, `models` | CPU. |
| Linux amd64, NVIDIA | `base`, `standalone`, `models` | CUDA image, existing `gpus: all` setting. Validate the host NVIDIA container runtime before startup. |
| Linux amd64, Intel GPU | `base`, `openvino`, `standalone`, `models` | OpenVINO through `/dev/dri`; set `GRPARSE_RENDER_GID` to the render-node group if it is not `990`. |
| Windows Docker Desktop/WSL2, CPU | Run from a WSL2 distribution: `base`, `cpu`, `standalone`, `models` | CPU. |
| Windows Docker Desktop/WSL2, NVIDIA | Run from WSL2: `base`, `standalone`, `models` | CUDA when Docker's NVIDIA GPU-PV prerequisites pass. |
| Windows Docker Desktop/WSL2, Intel GPU | No recipe | **Not run and unsupported by this Compose overlay.** It passes `/dev/dri`, while Intel's WSL2 container guidance uses `/dev/dxg` and `/usr/lib/wsl`; do not infer support from Linux OpenVINO settings. |

For macOS arm64, leave `pdf-backends` disabled unless it is needed. If it is,
add the `pdf-backends` profile after the arm64 overlay; those three private
images are amd64-only and can start more slowly under emulation. For a cold
backend startup, bring that profile up before gRParse or restart gRParse once
the workers are ready, because its initial backend probe is intentionally
one-shot.

The Windows NVIDIA path is limited to Docker Desktop's WSL2 backend and an
NVIDIA GPU with a WSL-capable driver. Docker documents that GPU support there
as NVIDIA GPU-PV, while its macOS installation documentation only describes
the Intel and Apple-silicon desktop variants. OpenVINO documents a distinct
WSL2 GPU container path using `/dev/dxg` rather than this repository's Linux
`/dev/dri` overlay. These are preparation notes, not machine test results:
macOS and Windows runtime tests were **not run** for this change.

Sources: [Docker Desktop GPU support on Windows](https://docs.docker.com/desktop/features/gpu/), [Docker Desktop for Mac](https://docs.docker.com/desktop/setup/install/mac-install/), [OpenVINO Docker installation](https://docs.openvino.ai/2026/get-started/install-openvino/install-openvino-docker-linux.html), and [OpenVINO WSL2 GPU device guidance](https://docs.openvino.ai/2023.3/ovms_docs_target_devices.html).

## Scope and validation

`scripts/compose-preflight.sh` validates base, CPU, OpenVINO, arm64 CPU,
model-init, standalone, and gRPC-exposure combinations with `docker compose
config --quiet`. It does not validate image availability, GPU drivers, model
downloads, a Compose lifecycle, or an endpoint. Those are runtime concerns;
record their command and exit status separately when a designated machine is
available.
