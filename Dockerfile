# syntax=docker/dockerfile:1.7

# The runtime base is swappable so a hardened mirror can slot in without a
# Dockerfile change, for example:
#   --build-arg GRPARSE_RUNTIME_IMAGE=docker.io/pipestreamai/dhi-nvidia-cuda:<tag>
# The runtime stage asks nothing of the base beyond glibc (matching or newer
# than the build stage's ubuntu26.04) and the CUDA runtime libraries on the
# loader path: no shell, no package manager, and no ldconfig run.
ARG GRPARSE_RUNTIME_IMAGE=nvidia/cuda:13.3.0-runtime-ubuntu26.04

FROM nvidia/cuda:13.3.0-devel-ubuntu26.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ git make ninja-build pkg-config libopencv-dev libpoppler-cpp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The cache id includes ABI-sensitive dependency versions. Update it whenever
# gRPC, ONNX Runtime, CUDA, the base toolchain, or a dependency patch under
# patches/ changes — a stale cache would keep an unpatched dependency tree.
RUN --mount=type=cache,id=grparse-ubuntu26-cuda13-grpc1.82.1-ort1.27.1-sessionep1,target=/build \
    cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
      -DGRPARSE_WERROR=ON \
 && cmake --build /build --target grparse-server grparse-stream-client \
      barcode-decoder-test base64-test collector-coordinator-test document-assembly-test document-merge-test figure-classifier-test layout-engine-test office-collector-test page-scheduler-test pdf-source-test \
      reading-order-test resource-pool-test streaming-service-test table-structure-engine-test table-structure-test text-geometry-test --parallel 4 \
 && ctest --test-dir /build --output-on-failure -L grparse \
 && mkdir -p /out \
 && cp /build/grparse-server /out/grparse-server \
 && cp /build/grparse-stream-client /out/grparse-stream-client \
 && cp -a /build/_deps/onnxruntime-src/lib /out/onnxruntime-lib

# Stage the runtime library closure. The runtime base is assumed minimal
# (hardened images carry no shell, no package manager, no ldconfig), so every
# shared library the binaries need ships from this stage, except two families
# the base must own: glibc (the loader and its libraries are inseparable) and
# the CUDA runtime math libraries (cublas, cufft, curand), which are the
# reason the base is a CUDA image at all. cuDNN is installed here only to be
# copied out; ONNX Runtime dlopens it, so ldd alone would never surface it.
RUN apt-get update && apt-get install -y --no-install-recommends libcudnn9-cuda-13 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /out/runtime-libs \
    && cp -a /usr/lib/x86_64-linux-gnu/libcudnn* /out/runtime-libs/ \
    && for f in /out/grparse-server /out/grparse-stream-client \
                /out/onnxruntime-lib/*.so* /usr/lib/x86_64-linux-gnu/libcudnn*.so*; do \
         LD_LIBRARY_PATH=/out/onnxruntime-lib ldd "$f" 2>/dev/null; \
       done \
       | awk '/=> \// {print $3}' | sort -u \
       | grep -v '^/usr/local/cuda' \
       | grep -v -E '/(libc|libm|libdl|libpthread|librt|libresolv|libnsl|libutil|libanl)\.so' \
       | while read -r lib; do cp -L "$lib" /out/runtime-libs/; done \
    && ls /out/runtime-libs | wc -l

# LD_LIBRARY_PATH stands in for ldconfig, and the numeric USER works with or
# without a passwd entry (65532 is the conventional nonroot uid in hardened
# images).
FROM ${GRPARSE_RUNTIME_IMAGE}
ENV GRPARSE_LISTEN_ADDRESS=0.0.0.0:50051 GRPARSE_MODELS_DIR=/models GRPARSE_PAGE_WORKERS=2 GRPARSE_CUDA_DEVICE=0 \
    LD_LIBRARY_PATH=/usr/local/lib
COPY --from=build /out/runtime-libs/ /usr/local/lib/
COPY --from=build /out/onnxruntime-lib/ /usr/local/lib/
# Fontconfig's default configuration, so poppler's font machinery starts
# quietly; the image intentionally ships no fonts (PDF text renders from
# embedded fonts, and the CV path reads pixels, not glyphs).
COPY --from=build /etc/fonts /etc/fonts
COPY --from=build /out/grparse-server /usr/local/bin/grparse-server
COPY --from=build /out/grparse-stream-client /usr/local/bin/grparse-stream-client
USER 65532:65532
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/grparse-server"]
