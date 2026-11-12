# compose/ — front-door proxy and exposure options for the demo stack

`compose.stack.yaml` keeps every service on one private compose network.
This directory holds the single public entry point and the knobs for
opening more of the stack to the outside.

## What runs by default

- `proxy` (nginx) is the only published port: `http://<host>:8080/` serves
  the demo shell, which reverse-proxies each service's web UI under
  `/ui/<name>/`. Everything else — gRParse, the parser services, the UI
  bridges — is reachable only inside the compose network.
- nginx resolves upstreams lazily through Docker's embedded DNS, so the
  proxy starts before the parsers and self-heals as they come up. Upload
  bodies and SSE progress streams pass through unbuffered, so multi-hundred
  MiB documents work through the proxy exactly as they do direct.

## Running without an NVIDIA GPU (macOS, plain Linux)

Every published image in the stack is multi-arch (amd64 + arm64) except
the three PDF backend services (see the ARM64 note below). That includes
`pipestreamai/grparse:latest-cpu`, the Dockerfile.cpu build of gRParse
against ONNX Runtime's plain CPU package (each architecture built and
tested natively in CI, with provenance and SBOM attestations attached;
see `docs/RELEASING.md`). The CPU overlay makes the stack run natively
anywhere Docker does - Apple Silicon included, with no emulation:

```sh
./compose/clone-siblings.sh   # fresh machine: fetch the sibling checkouts
                              # the build: lines expect (published images need none)
# Or bootstrap the entire grpc-services workspace (gRParse + siblings) on a new machine:
# curl -fsSL https://raw.githubusercontent.com/ai-pipestream/gRParse/main/compose/clone-siblings.sh | sh -s -- --full ./grpc-services
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml --profile parsers up
```

The overlay swaps gRParse to the CPU image, drops the `gpus: all`
reservation, and sets `GRPARSE_ORT_EP=cpu` (a deliberate provider choice;
the server never falls back silently). Inference is slower on CPU than on
a GPU, but the whole demo works.

### ARM64 hosts and the PDF backends

gRParse links no PDF engine; it reads every PDF through a PDF backend
service, and the core stack starts `pdfium` for that (`qparse` rides in the
opt-in `pdf-backends` profile, the GPL `poppler` in its own `poppler`
profile). All three backend images are amd64 + arm64 manifest lists, so
an arm64 host needs only the CPU overlay:

```sh
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
```

Do not layer the openvino overlay on arm64; it is Intel-only.

gRParse's model files still need to exist in `models/` first:
`scripts/fetch-models.sh` fetches and sha256-checks them (see
`models/README.md`). Whisper weights go in `../grpc-asr/models` if the
`heavy` profile's asr tab should do real work.

## Models from an image instead of ./models

`compose.stack.models.yaml` replaces the `./models` bind mount with a named
volume that a one-shot `models` service fills from
`pipestreamai/grparse-models` (built from `Dockerfile.models`: every file
in `models/MANIFEST`, fetched, sha256-verified and patched at build time).
gRParse waits for that copy to finish (`service_completed_successfully`)
and a second `up` copies nothing. Layer it last, after the cpu or openvino
overlay, because the openvino overlay names `./models:/models` itself:

```sh
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml -f compose.stack.models.yaml up
docker compose -f compose.stack.yaml -f compose.stack.openvino.yaml -f compose.stack.models.yaml up
```

The published image carries the heron layout detector; a local
`docker compose ... build models` with `MODELS_LAYOUT=all` or
`MODELS_TOKENIZER=1` as build args (see `Dockerfile.models`) bakes in more.

## Pinning the stack to a release

`STACK_TAG` (default `latest`) is the tag of every `pipestreamai/*` image in
`compose.stack.yaml` and its overlays, variant suffixes included: the CPU
overlay renders `pipestreamai/grparse:${STACK_TAG}-cpu`, the OpenVINO
overlay `${STACK_TAG}-openvino`, asr `pipestreamai/grpc-asr:${STACK_TAG}-cpu`,
everything else `${STACK_TAG}`.

```sh
STACK_TAG=0.2.0 docker compose -f compose.stack.yaml pull
STACK_TAG=0.2.0 docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
```

Unset, the stack tracks `:latest` as before; `up --build` keeps building the
sibling checkouts under the same names. The pin assumes the whole fleet cut
that version (`docs/RELEASING.md`).

## TLS for the web frontend

`nginx.conf` ships a commented-out TLS server block on 8443. To enable it:

1. Put a certificate and key in `compose/certs/` as `fullchain.pem` and
   `privkey.pem`. Any source works:
   - self-signed for a quick demo:
     `openssl req -x509 -newkey rsa:2048 -nodes -keyout compose/certs/privkey.pem -out compose/certs/fullchain.pem -days 365 -subj "/CN=demo.local"`
   - [mkcert](https://github.com/FiloSottile/mkcert) for locally-trusted
     development certificates
   - [Let's Encrypt](https://letsencrypt.org/getting-started/) via certbot
     for a publicly resolvable hostname
2. Uncomment the TLS block in `nginx.conf`, the `8443:8443` port mapping,
   and the certs volume on the `proxy` service in `compose.stack.yaml`.
3. `docker compose -f compose.stack.yaml up -d --build proxy` and browse to
   `https://<host>:8443/`.

## Opening the gRPC ports

By default no gRPC port leaves the compose network. Two ways to change
that, depending on who the clients are:

- **Plaintext, trusted network:** apply the overlay
  `compose.stack.expose-grpc.yaml`, which publishes each service's gRPC
  port (50051, 50053, 50057, 50062, …) on the host:

  ```
  docker compose -f compose.stack.yaml -f compose.stack.expose-grpc.yaml up -d --build
  ```

- **TLS-terminated gRPC:** nginx proxies gRPC natively over HTTP/2 with the
  [ngx_http_grpc_module](http://nginx.org/en/docs/http/ngx_http_grpc_module.html)
  (`grpc_pass`). Add one `server` block per service in `nginx.conf` with
  its own listen port and the same certs as above, e.g.:

  ```nginx
  server {
    listen 50057 ssl http2;
    ssl_certificate     /etc/nginx/certs/fullchain.pem;
    ssl_certificate_key /etc/nginx/certs/privkey.pem;
    location / { grpc_pass grpc://lol-html:50057; }
  }
  ```

  Note nginx speaks HTTP/2 on the listen port here, so point gRPC clients
  at the TLS port directly (no path prefixing — gRPC routing is by
  fully-qualified method name, not URL path).

## Prometheus + Grafana observability overlay

`compose.stack.observability.yaml` adds Prometheus (`prom/prometheus:v3.3.0`)
and Grafana (`grafana/grafana:13.2.4`) under the `observability` profile:

```sh
docker compose -f compose.stack.yaml -f compose.stack.observability.yaml --profile observability up -d
```

Prometheus scrapes `grparse:9464` (`job_name: "grparse"`), `asr:9464`
(`job_name: "grpc-asr"` when `--profile heavy` is active), and
`host.docker.internal:9465` (`job_name: "grparse-corpus-eval"`) every `5s`
using `compose/prometheus/prometheus.yml`. Grafana publishes
`127.0.0.1:${GRAFANA_PORT:-3000}:3000` with anonymous local access and mounts
the provisioned datasource (`compose/grafana/provisioning/datasources/prometheus.yaml`)
and dashboards (`compose/grafana/dashboards/grparse-pipeline-overview.json` and
`compose/grafana/dashboards/grparse-corpus-eval.json`).


## Experimental LightOn OCR service

The `lighton` profile starts the native CPU LightOnOCR-3-0.8B service on the
compose network at `lighton-ocr:50074`. It is separate from `heavy` and uses
a local image by default. There is no shell tab or coordinator target; this
profile does not add an OCR route to gRParse. Native inference has a
committed-source Dockerfile and read-only RPC qualification in the
[service receipt](https://git.rokkon.com/ai-pipestream/grpc-lighton-ocr/src/branch/main/docs/native-proof.md).
That receipt covers CPU 0.8B, not 4B, ARM, GPU, or general OCR accuracy.

Prepare the pinned `model-bf16.gguf` and `vision-bf16.gguf` files using the
[service instructions](https://git.rokkon.com/ai-pipestream/grpc-lighton-ocr/src/branch/main/README.md).
From a sibling workspace, build and start only this service:

```sh
export LIGHTON_MODELS_DIR=/path/to/prepared-lighton-models
docker compose -f compose.stack.yaml --profile lighton up -d --build lighton-ocr
```

`LIGHTON_MODEL` defaults to `0.8b`. Production startup rejects `4b`, missing
weights, and files whose hashes do not match the pinned artifacts.
`LIGHTON_MODELS_DIR` overrides the host model directory; the bind is read-only
and a missing directory fails instead of creating a root-owned host path.
With `compose.stack.standalone.yaml`, the default becomes
`./protos/grpc-lighton-ocr/models`; populate it before starting the profile.
The image defaults to `grpc-lighton-ocr:local`. Set `LIGHTON_IMAGE` to an
image loaded from a local build or to a verified published reference. This
experimental service is outside `STACK_TAG` and the versioned release fleet;
a fleet release does not wait for LightOn publication.

The server runs with a read-only root and dropped capabilities. Page bytes
stay in memory; the model mount is not a request-data store. The empty-name
gRPC health check reports transport availability. For OCR readiness, check
health with service `ai.pipestream.lightonocr.v1.LightOnOcrService` and require
`SERVING`, plus `GetServiceInfo.engine_loaded = true`. Explicit diagnostic
mode has no engine and reports `NOT_SERVING` for that service. Inspect page
failures and completion counts; an OK stream status alone is not parse success.

The separate Unlimited-OCR 2 project is not enabled by this profile and has
no native gRParse collector route.
