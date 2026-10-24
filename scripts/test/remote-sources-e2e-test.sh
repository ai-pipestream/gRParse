#!/usr/bin/env bash
# End to end receipt for the S3 and HTTP sources: starts RustFS over http and
# a second RustFS over https with a self-signed certificate (SAN 127.0.0.1 and
# localhost, so the only fault is that no trusted CA signed it), seeds both
# with the AWS CLI,
# starts a local http origin and a self-signed https origin
# (remote-sources-e2e/http_server.py), then runs remote-sources-e2e-test
# from the build directory, or from an image built with
# `docker build -f Dockerfile.cpu --target e2e`, against them.
#
# usage: scripts/test/remote-sources-e2e-test.sh <build-dir|image>
set -euo pipefail
build=${1:?build dir or image}
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/test/e2e-binary.sh
. "$here/e2e-binary.sh"
id=$$
net=grparse-remote-e2e-$id
work=$(mktemp -d)
rustfs=grparse-remote-e2e-rustfs-$id
rustfs_tls=grparse-remote-e2e-rustfs-tls-$id
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  echo "== http origin log"; cat "$work/http.log" 2>/dev/null || true
  echo "== https origin log"; cat "$work/https.log" 2>/dev/null || true
  docker rm -f "$rustfs" "$rustfs_tls" >/dev/null 2>&1 || true
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
# A published port opens before the server behind it listens, so wait for
# RustFS's own health answer (certificate unchecked: one of them is
# self-signed on purpose).
wait_health() {
  for _ in $(seq 1 60); do
    if curl -skf --max-time 2 -o /dev/null "$1/health"; then return 0; fi
    sleep 1
  done
  echo "$1 never answered /health" >&2
  return 1
}
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

printf '# Remote sources\n\nA document fetched over S3 and HTTP.\n' > "$work/sample.md"
mkdir -p "$work/certs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:rustfs-tls" \
  -keyout "$work/certs/rustfs_key.pem" -out "$work/certs/rustfs_cert.pem" 2>/dev/null
openssl x509 -in "$work/certs/rustfs_cert.pem" -noout -subject -issuer -ext subjectAltName
chmod -R a+rX "$work"

docker network create "$net" >/dev/null
docker run -d --name "$rustfs" --network "$net" --network-alias rustfs -p 127.0.0.1::9000 \
  -e RUSTFS_ACCESS_KEY=e2e-access -e RUSTFS_SECRET_KEY=e2e-secret-key \
  rustfs/rustfs:1.0.0-beta.11-preview.1 >/dev/null
docker run -d --name "$rustfs_tls" --network "$net" --network-alias rustfs-tls -p 127.0.0.1::9000 \
  -e RUSTFS_ACCESS_KEY=e2e-access -e RUSTFS_SECRET_KEY=e2e-secret-key \
  -e RUSTFS_TLS_PATH=/certs -v "$work/certs:/certs:ro" \
  rustfs/rustfs:1.0.0-beta.11-preview.1 >/dev/null
rustfs_port=$(docker port "$rustfs" 9000/tcp | head -1 | sed 's/.*://')
rustfs_tls_port=$(docker port "$rustfs_tls" 9000/tcp | head -1 | sed 's/.*://')
http_port=$(free_port)
python3 "$here/remote-sources-e2e/http_server.py" "$http_port" "$work/sample.md" > "$work/http.log" 2>&1 &
pids+=($!)
https_port=$(free_port)
python3 "$here/remote-sources-e2e/http_server.py" "$https_port" "$work/sample.md" \
  "$work/certs/rustfs_cert.pem" "$work/certs/rustfs_key.pem" > "$work/https.log" 2>&1 &
pids+=($!)
for port in "$rustfs_port" "$rustfs_tls_port" "$http_port" "$https_port"; do wait_port "$port"; done
wait_health "http://127.0.0.1:$rustfs_port"
wait_health "https://127.0.0.1:$rustfs_tls_port"
echo "== RustFS http 127.0.0.1:$rustfs_port, RustFS https (self-signed) 127.0.0.1:$rustfs_tls_port, http origin 127.0.0.1:$http_port, https origin (self-signed) 127.0.0.1:$https_port"

# s3 <endpoint> <aws s3 arguments>: the AWS CLI on the private network, not
# verifying the self-signed certificate.
s3() {
  local endpoint=$1
  shift
  docker run --rm --network "$net" -v "$work:/work:ro" \
    -e AWS_ACCESS_KEY_ID=e2e-access -e AWS_SECRET_ACCESS_KEY=e2e-secret-key \
    -e AWS_DEFAULT_REGION=us-east-1 \
    amazon/aws-cli:2.37.9 --endpoint-url "$endpoint" --no-verify-ssl s3 "$@"
}
for endpoint in http://rustfs:9000 https://rustfs-tls:9000; do
  s3 "$endpoint" mb s3://grparse-in
  s3 "$endpoint" cp /work/sample.md s3://grparse-in/docs/sample.md
  s3 "$endpoint" cp /work/sample.md s3://grparse-in/two/a.md
  s3 "$endpoint" cp /work/sample.md s3://grparse-in/two/b.md
  s3 "$endpoint" ls --recursive s3://grparse-in
done

echo "== the self-signed certificate is refused by a verifying client (curl, no -k)"
if curl -sS --max-time 10 "https://127.0.0.1:$rustfs_tls_port/health" 2>&1; then
  echo "unexpected: curl trusted the self-signed certificate" >&2; exit 1
fi

echo "== remote-sources-e2e-test"
run_e2e_binary "$build" "$work" remote-sources-e2e-test \
  GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1 \
  GRPARSE_E2E_REMOTE=1 \
  GRPARSE_E2E_SAMPLE="$work/sample.md" \
  GRPARSE_E2E_S3_HTTP_ENDPOINT="http://127.0.0.1:$rustfs_port" \
  GRPARSE_E2E_S3_HTTPS_ENDPOINT="https://127.0.0.1:$rustfs_tls_port" \
  GRPARSE_E2E_S3_ACCESS_KEY=e2e-access \
  GRPARSE_E2E_S3_SECRET_KEY=e2e-secret-key \
  GRPARSE_E2E_HTTP_BASE="http://127.0.0.1:$http_port" \
  GRPARSE_E2E_HTTPS_BASE="https://127.0.0.1:$https_port"
echo "== remote-sources-e2e-test passed"
