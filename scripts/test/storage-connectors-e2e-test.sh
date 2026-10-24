#!/usr/bin/env bash
# End to end receipt for the storage connectors: starts fake-gcs-server,
# Azurite and MinIO on a private docker network and the mock Google token
# endpoint and Drive v3 API on the host, seeds the inputs, then runs
# storage-connectors-e2e-test from the build directory, or from an image
# built with `docker build -f Dockerfile.cpu --target e2e`, against them and
# checks the S3 (MinIO) objects it wrote with mc.
#
# usage: scripts/test/storage-connectors-e2e-test.sh <build-dir|image>
set -euo pipefail
build=${1:?build dir or image}
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/test/e2e-binary.sh
. "$here/e2e-binary.sh"
id=$$
net=grparse-storage-e2e-$id
work=$(mktemp -d)
gcs=grparse-storage-e2e-gcs-$id
azurite=grparse-storage-e2e-azurite-$id
minio=grparse-storage-e2e-minio-$id
mock_pid=
cleanup() {
  if [ -n "$mock_pid" ]; then kill "$mock_pid" 2>/dev/null || true; fi
  echo "== mock Google log"; cat "$work/mock_google.log" 2>/dev/null || true
  docker rm -f "$gcs" "$azurite" "$minio" >/dev/null 2>&1 || true
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

printf '# Storage connectors\n\nA document fetched from a store.\n' > "$work/sample.md"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$work/sa.pem" 2>/dev/null
openssl pkey -in "$work/sa.pem" -pubout -out "$work/sa.pub.pem"
mkdir -p "$work/gcs/grparse-in/docs" "$work/gcs/grparse-out"
cp "$work/sample.md" "$work/gcs/grparse-in/docs/sample.md"
chmod -R a+rX "$work"

docker network create "$net" >/dev/null
docker run -d --name "$gcs" --network "$net" -p 127.0.0.1::4443 -v "$work/gcs:/data:ro" \
  fsouza/fake-gcs-server:1.52.2 -scheme http -port 4443 -backend memory -data /data >/dev/null
docker run -d --name "$azurite" --network "$net" -p 127.0.0.1::10000 \
  mcr.microsoft.com/azure-storage/azurite:3.34.0 azurite-blob --blobHost 0.0.0.0 --loose >/dev/null
docker run -d --name "$minio" --network "$net" --network-alias minio -p 127.0.0.1::9000 \
  -e MINIO_ROOT_USER=e2e-access -e MINIO_ROOT_PASSWORD=e2e-secret-key \
  quay.io/minio/minio:latest server /data >/dev/null
gcs_port=$(docker port "$gcs" 4443/tcp | head -1 | sed 's/.*://')
azure_port=$(docker port "$azurite" 10000/tcp | head -1 | sed 's/.*://')
minio_port=$(docker port "$minio" 9000/tcp | head -1 | sed 's/.*://')
mock_port=$(free_port)
python3 "$here/storage-connectors-e2e/mock_google.py" "$mock_port" "$work/sa.pub.pem" \
  "$work/sample.md" file-sample > "$work/mock_google.log" 2>&1 &
mock_pid=$!
for port in "$gcs_port" "$azure_port" "$minio_port" "$mock_port"; do wait_port "$port"; done
sleep 2
echo "== fake-gcs-server 127.0.0.1:$gcs_port, Azurite 127.0.0.1:$azure_port, MinIO 127.0.0.1:$minio_port, mock Google 127.0.0.1:$mock_port"

azure_endpoint="http://127.0.0.1:$azure_port/devstoreaccount1"
python3 "$here/storage-connectors-e2e/seed_azure.py" "$azure_endpoint" "$work/sample.md"
mc() { docker run --rm --network "$net" -e MC_HOST_e2e=http://e2e-access:e2e-secret-key@minio:9000 \
  minio/mc:RELEASE.2025-08-13T08-35-41Z "$@"; }
mc mb e2e/grparse-out

mkdir -p "$work/out"
echo "== storage-connectors-e2e-test"
run_e2e_binary "$build" "$work" storage-connectors-e2e-test \
  GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1 \
  GRPARSE_E2E_STORAGE=1 \
  GRPARSE_E2E_SAMPLE="$work/sample.md" \
  GRPARSE_E2E_SA_KEY="$work/sa.pem" \
  GRPARSE_E2E_GCS_ENDPOINT="http://127.0.0.1:$gcs_port" \
  GRPARSE_E2E_AZURE_CONNECTION_STRING="DefaultEndpointsProtocol=http;AccountName=devstoreaccount1;AccountKey=Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==;BlobEndpoint=$azure_endpoint" \
  GRPARSE_E2E_GOOGLE="http://127.0.0.1:$mock_port" \
  GRPARSE_E2E_DRIVE_FILE_ID=file-sample \
  GRPARSE_E2E_S3_ENDPOINT="http://127.0.0.1:$minio_port" \
  GRPARSE_E2E_S3_ACCESS_KEY=e2e-access \
  GRPARSE_E2E_S3_SECRET_KEY=e2e-secret-key \
  GRPARSE_E2E_OUT_DIR="$work/out"

echo "== MinIO objects (mc ls --recursive e2e/grparse-out)"
mc ls --recursive e2e/grparse-out | tee "$work/mc-ls.txt"
sed 's/.* //' "$work/mc-ls.txt" | sed 's|^|run/|;s|^run/run/|run/|' | sort > "$work/mc-keys.txt"
sort "$work/out/s3-keys.txt" > "$work/test-keys.txt"
if ! diff -u "$work/test-keys.txt" "$work/mc-keys.txt"; then
  echo "MinIO holds different keys than the S3 target reported" >&2
  exit 1
fi
echo "== MinIO holds exactly the $(wc -l < "$work/test-keys.txt") keys the S3 target reported"
echo "storage-connectors-e2e: ok"
