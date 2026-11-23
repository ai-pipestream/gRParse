#!/bin/sh
# Keeps the exact ONNX Runtime archives CMakeLists.txt pins available as
# assets of this repository's `ort-archives-1` release.
#
#   sh scripts/mirror-ort-archives.sh check    # every asset present, sha256 matches
#   sh scripts/mirror-ort-archives.sh mirror   # upload any missing asset, then check
#
# Why: github.com/microsoft/onnxruntime went offline on 2026-11-17 and every
# release download began answering 404, so no image or deps seed could be
# built from scratch (publish runs 44398650884 and 44398650887). The bytes
# survive in our own 2026-11-16 deps seeds: FetchContent keeps each
# downloaded archive under _deps/onnxruntime-subbuild/, and prune-seed.sh does
# not remove it. `mirror` streams the seed layer that holds an archive straight
# from Docker Hub (anonymous pull, pinned by manifest digest, nothing written
# to disk but the archive), refuses it unless its sha256 is the one
# CMakeLists.txt pins, and uploads it with `gh release upload`.
#
# The checksums are never changed to fit a source: an archive that cannot be
# found with its pinned sha256 is an error, not a reason to repin.
#
# The tag ort-archives-1 points at 46e84fa and lives on Forgejo too: the
# Forgejo push mirror deletes GitHub-only tags, which would turn the release
# into a draft whose assets 404. When GitHub lacks the tag, the release is
# created on that same commit. `mirror` needs GH_TOKEN with contents: write.
set -eu

tag=ort-archives-1
tag_commit=46e84fa2656d800b66d2e49eddea099218a7aa5f
repo="${GITHUB_REPOSITORY:-ai-pipestream/gRParse}"
base="https://github.com/$repo/releases/download/$tag"
deps=pipestreamai/grparse-deps
seed_dir=opt/grparse-seed/_deps/onnxruntime-subbuild/onnxruntime-populate-prefix/src

# archive  sha256 (as pinned in CMakeLists.txt)  seed it is recovered from
archives='
onnxruntime-linux-x64-1.30.0.tgz a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd cpu-amd64-f2829f2d1e2b120d@sha256:9efa9ceecee55bd6d826f0a47a189e218ef65c6f25398705abbf39a37954aab7
onnxruntime-linux-aarch64-1.30.0.tgz e16a27a8ed330bbc698df7330b0cf56e722f354e3bcc92118682c74ef3c3e3da cpu-arm64-b26815518721aa0e@sha256:96d8ad01f4bc7c6821a475407fa4ff66cfb1f318ee5d22a8133846dc94d404f4
onnxruntime-linux-x64-1.24.1.tgz 9142552248b735920f9390027e4512a2cacf8946a1ffcbe9071a5c210531026f openvino-amd64-49c9815179468adc@sha256:de61d3a886ee184efb2015d0f99251c2010d9ee05329f00081c1d848171a8246
onnxruntime-linux-x64-gpu_cuda13-1.30.0.tgz 382d79133112388cf94ce5855789b7c9bef12bef76a08b6b277e5a317213adcd cuda-amd64-d5c5a43ff5855f63@sha256:42230549197fdf643260395ea3b5b7935c333551d866f5996b8bd99f12d30424
'

mode="${1:-check}"
case "$mode" in check|mirror) ;; *) echo "usage: $0 check|mirror" >&2; exit 2 ;; esac

# The list must match CMakeLists.txt both ways, so a bumped ONNX Runtime
# cannot ship without its mirror copy and a stale entry cannot linger.
for name in $(grep -oE 'GRPARSE_ORT_MIRROR}/[^"]+' CMakeLists.txt | sed 's|.*}/||'); do
  for n in $(echo "$name" | sed 's/\${GRPARSE_ORT_CPU_ARCH}/x64/'; echo "$name" | sed 's/\${GRPARSE_ORT_CPU_ARCH}/aarch64/'); do
    echo "$archives" | grep -q "^$n " \
      || { echo "mirror-ort: CMakeLists.txt fetches $n, not listed in $0" >&2; exit 1; }
  done
done
echo "$archives" | while read -r name want ref; do
  [ -n "$name" ] || continue
  grep -q "$want" CMakeLists.txt \
    || { echo "mirror-ort: $name sha256 $want is not pinned in CMakeLists.txt" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Prints the asset's sha256, or nothing when it is not served.
remote_sum() {
  curl -fsSL --retry 3 "$base/$1" -o "$work/$1" 2>/dev/null || return 0
  sha256sum "$work/$1" | cut -d' ' -f1
}

recover() { # name sha seed-ref
  name=$1 want=$2 ref=$3
  tagname=${ref%@*} digest=${ref#*@}
  token=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$deps:pull" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
  # Largest layers first: the seed tree is the last, biggest layer.
  layers=$(curl -fsSL -H "Authorization: Bearer $token" \
      -H 'Accept: application/vnd.oci.image.manifest.v1+json,application/vnd.docker.distribution.manifest.v2+json' \
      "https://registry-1.docker.io/v2/$deps/manifests/$digest" \
    | python3 -c 'import json,sys
for l in sorted(json.load(sys.stdin)["layers"], key=lambda l: -l["size"]): print(l["digest"])')
  for layer in $layers; do
    echo "mirror-ort: looking for $name in $deps:$tagname layer $layer" >&2
    curl -fsSL -H "Authorization: Bearer $token" "https://registry-1.docker.io/v2/$deps/blobs/$layer" \
      | tar -xz -C "$work" --strip-components=6 "$seed_dir/$name" 2>/dev/null || true
    [ -f "$work/$name" ] && break
  done
  [ -f "$work/$name" ] || { echo "mirror-ort: $name not found in $deps:$tagname" >&2; return 1; }
  got=$(sha256sum "$work/$name" | cut -d' ' -f1)
  [ "$got" = "$want" ] || {
    echo "mirror-ort: $name from $deps:$tagname has sha256 $got, pinned $want; not uploading" >&2
    return 1
  }
  if ! gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    gh release create "$tag" --repo "$repo" --target "$tag_commit" --prerelease --latest=false \
      --title "ONNX Runtime archive mirror" \
      --notes "Byte-exact copies of the Microsoft ONNX Runtime release archives gRParse pins (MIT licensed, see LICENSE inside each archive). CMakeLists.txt verifies each against its pinned SHA-256. Maintained by scripts/mirror-ort-archives.sh." \
      || { echo "mirror-ort: could not create release $tag" >&2; return 1; }
  fi
  gh release upload "$tag" --repo "$repo" --clobber "$work/$name" \
    || { echo "mirror-ort: could not upload $name" >&2; return 1; }
  echo "mirror-ort: uploaded $name ($got)" >&2
}

failed=0
echo "$archives" | while read -r name want ref; do
  [ -n "$name" ] || continue
  got=$(remote_sum "$name")
  if [ "$got" = "$want" ]; then
    echo "mirror-ort: ok $name"
  elif [ "$mode" = mirror ] && [ -z "$got" ]; then
    recover "$name" "$want" "$ref" || exit 1
    for _ in 1 2 3 4 5 6; do # a fresh asset can take a moment to be served
      got=$(remote_sum "$name")
      [ "$got" = "$want" ] && break
      sleep 10
    done
    [ "$got" = "$want" ] || { echo "mirror-ort: $name still not served after upload" >&2; exit 1; }
    echo "mirror-ort: ok $name (mirrored)"
  else
    echo "mirror-ort: $base/$name ${got:+has sha256 $got, }expected $want" >&2
    exit 1
  fi
  rm -f "$work/$name"
done || failed=1
exit "$failed"
