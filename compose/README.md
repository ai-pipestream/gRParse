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
