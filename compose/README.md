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

Every published image in the stack is multi-arch (amd64 + arm64),
including `pipestreamai/grparse:latest-cpu`, the Dockerfile.cpu build of
gRParse against ONNX Runtime's plain CPU package. The CPU overlay makes
the stack run natively anywhere Docker does - Apple Silicon included,
with no emulation:

```sh
./compose/clone-siblings.sh   # fresh machine: fetch the sibling checkouts
                              # the shell's proto bind mounts expect
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml up
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml --profile parsers up
```

The overlay swaps gRParse to the CPU image, drops the `gpus: all`
reservation, and sets `GRPARSE_ORT_EP=cpu` (a deliberate provider choice;
the server never falls back silently). Inference is slower on CPU than on
a GPU, but the whole demo works.

gRParse's four ONNX models still need to exist in `models/` first (see
`models/README.md`), and whisper weights go in `../grpc-asr/models` if the
`heavy` profile's asr tab should do real work.

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
