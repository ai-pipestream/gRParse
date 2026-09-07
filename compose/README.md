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

`pipestreamai/grparse:latest-cpu`, the Dockerfile.cpu build of gRParse
against ONNX Runtime's plain CPU package, is multi-arch (amd64 + arm64,
each architecture built and tested natively in CI, with provenance and
SBOM attestations attached; see `docs/RELEASING.md`), and so is every
Rust/Java image in the stack. The CPU overlay makes the stack run natively
on any amd64 host Docker runs on:

```sh
./compose/clone-siblings.sh   # fresh machine: fetch the sibling checkouts
                              # the build: lines expect (published images need none)
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml --profile parsers up
```

The overlay swaps gRParse to the CPU image, drops the `gpus: all`
reservation, and sets `GRPARSE_ORT_EP=cpu` (a deliberate provider choice;
the server never falls back silently). Inference is slower on CPU than on
a GPU, but the whole demo works.

### ARM64 hosts (Apple Silicon, ARM servers)

The private C++ peers publish amd64 only (GitHub's free arm runners do not
cover private repos and emulated C++ builds are too slow; the "Publishing
images" section of the workspace AGENTS.md), so on an arm64 host the CPU
overlay alone dies at the first `pipestreamai/grpc-libreoffice` pull. Layer
`compose.stack.arm64.yaml` on top: it repeats the CPU swap and pins every
amd64-only image (`libreoffice`, `libreoffice-ui`, `pdfium`, `qparse`,
`poppler`, `vlm-convert`, `asr`) to `linux/amd64`, which runs them under
the host's binfmt_misc QEMU handler:

```sh
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml -f compose.stack.arm64.yaml up
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml -f compose.stack.arm64.yaml --profile parsers up
```

Docker Desktop (macOS) ships the QEMU handler; a bare Linux arm64 host
needs it once (`docker run --privileged --rm tonistiigi/binfmt --install
amd64`, or the qemu-user-static package) or the pinned containers fail at
start with "exec format error". Expect emulation costs where they are
pinned: libreoffice renders slowly (and its UNO bridge is flaky under
emulation, so retry a failed render once), whisper in `asr` is 10x+ slower
and impractical for real audio, and the `pdf-backends` profile's worker
spawn can outrun gRParse's startup probe (start the profile first or
restart `grparse`). Everything else - gRParse itself, the parsers
profile, the shell, enrich - runs natively. Do not layer the openvino
overlay on arm64; it is Intel-only by nature.

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
