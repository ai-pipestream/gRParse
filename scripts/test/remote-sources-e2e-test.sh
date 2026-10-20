#!/usr/bin/env bash
# End to end receipt for the S3 and HTTP sources: starts MinIO over http and
# a second MinIO over https with a self-signed certificate (SAN 127.0.0.1 and
# localhost, so the only fault is that no trusted CA signed it), seeds both,
# starts a local http origin and a self-signed https origin
# (remote-sources-e2e/http_server.py), then runs remote-sources-e2e-test
# from the build directory against them.
#
# usage: scripts/test/remote-sources-e2e-test.sh <build-dir>
set -euo pipefail
build=${1:?build dir}
here=$(cd "$(dirname "$0")" && pwd)
id=$$
net=grparse-remote-e2e-$id
work=$(mktemp -d)
minio=grparse-remote-e2e-minio-$id
minio_tls=grparse-remote-e2e-minio-tls-$id
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  echo "== http origin log"; cat "$work/http.log" 2>/dev/null || true
  echo "== https origin log"; cat "$work/https.log" 2>/dev/null || true
  docker rm -f "$minio" "$minio_tls" >/dev/null 2>&1 || true
  docker network rm "$net" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

wait_port() {
  for _ in $(seq 1 60); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then return 0; fi
    sleep 1
  done
  echo "port $1 never opened" >&2
  return 1
}
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

printf '# Remote sources\n\nA document fetched over S3 and HTTP.\n' > "$work/sample.md"
mkdir -p "$work/certs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:minio-tls" \
  -keyout "$work/certs/private.key" -out "$work/certs/public.crt" 2>/dev/null
openssl x509 -in "$work/certs/public.crt" -noout -subject -issuer -ext subjectAltName
chmod -R a+rX "$work"

docker network create "$net" >/dev/null
docker run -d --name "$minio" --network "$net" --network-alias minio -p 127.0.0.1::9000 \
  -e MINIO_ROOT_USER=e2e-access -e MINIO_ROOT_PASSWORD=e2e-secret-key \
  quay.io/minio/minio:latest server /data >/dev/null
docker run -d --name "$minio_tls" --network "$net" --network-alias minio-tls -p 127.0.0.1::9000 \
  -e MINIO_ROOT_USER=e2e-access -e MINIO_ROOT_PASSWORD=e2e-secret-key \
  -v "$work/certs:/certs:ro" \
  quay.io/minio/minio:latest server /data --certs-dir /certs >/dev/null
minio_port=$(docker port "$minio" 9000/tcp | head -1 | sed 's/.*://')
minio_tls_port=$(docker port "$minio_tls" 9000/tcp | head -1 | sed 's/.*://')
http_port=$(free_port)
python3 "$here/remote-sources-e2e/http_server.py" "$http_port" "$work/sample.md" > "$work/http.log" 2>&1 &
pids+=($!)
https_port=$(free_port)
python3 "$here/remote-sources-e2e/http_server.py" "$https_port" "$work/sample.md" \
  "$work/certs/public.crt" "$work/certs/private.key" > "$work/https.log" 2>&1 &
pids+=($!)
for port in "$minio_port" "$minio_tls_port" "$http_port" "$https_port"; do wait_port "$port"; done
sleep 3
echo "== MinIO http 127.0.0.1:$minio_port, MinIO https (self-signed) 127.0.0.1:$minio_tls_port, http origin 127.0.0.1:$http_port, https origin (self-signed) 127.0.0.1:$https_port"

mc() { docker run --rm --network "$net" -v "$work:/work:ro" \
  -e MC_HOST_plain=http://e2e-access:e2e-secret-key@minio:9000 \
  -e MC_HOST_tls=https://e2e-access:e2e-secret-key@minio-tls:9000 \
  minio/mc:RELEASE.2025-08-13T08-35-41Z --insecure "$@"; }
for alias in plain tls; do
  mc mb "$alias/grparse-in"
  mc cp /work/sample.md "$alias/grparse-in/docs/sample.md"
  mc cp /work/sample.md "$alias/grparse-in/two/a.md"
  mc cp /work/sample.md "$alias/grparse-in/two/b.md"
  mc ls --recursive "$alias/grparse-in"
done

echo "== the self-signed certificate is refused by a verifying client (curl, no -k)"
if curl -sS --max-time 10 "https://127.0.0.1:$minio_tls_port/minio/health/live" 2>&1; then
  echo "unexpected: curl trusted the self-signed certificate" >&2; exit 1
fi

echo "== remote-sources-e2e-test"
GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1 \
GRPARSE_E2E_REMOTE=1 \
GRPARSE_E2E_SAMPLE="$work/sample.md" \
GRPARSE_E2E_S3_HTTP_ENDPOINT="http://127.0.0.1:$minio_port" \
GRPARSE_E2E_S3_HTTPS_ENDPOINT="https://127.0.0.1:$minio_tls_port" \
GRPARSE_E2E_S3_ACCESS_KEY=e2e-access \
GRPARSE_E2E_S3_SECRET_KEY=e2e-secret-key \
GRPARSE_E2E_HTTP_BASE="http://127.0.0.1:$http_port" \
GRPARSE_E2E_HTTPS_BASE="https://127.0.0.1:$https_port" \
  "$build/remote-sources-e2e-test"
echo "== remote-sources-e2e-test passed"
