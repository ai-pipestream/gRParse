# syntax=docker/dockerfile:1.7
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
      base64-test document-assembly-test figure-classifier-test layout-engine-test page-scheduler-test pdf-source-test \
      reading-order-test resource-pool-test streaming-service-test table-structure-engine-test table-structure-test text-geometry-test --parallel 4 \
 && ctest --test-dir /build --output-on-failure -L grparse \
 && mkdir -p /out \
 && cp /build/grparse-server /out/grparse-server \
 && cp /build/grparse-stream-client /out/grparse-stream-client \
 && cp -a /build/_deps/onnxruntime-src/lib /out/onnxruntime-lib

FROM nvidia/cuda:13.3.0-runtime-ubuntu26.04
ENV GRPARSE_LISTEN_ADDRESS=0.0.0.0:50051 GRPARSE_MODELS_DIR=/models GRPARSE_PAGE_WORKERS=2 GRPARSE_CUDA_DEVICE=0
RUN apt-get update && apt-get install -y --no-install-recommends libcudnn9-cuda-13 libpoppler-cpp3 libopencv-core-dev libopencv-imgcodecs-dev libopencv-imgproc-dev \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/grparse-server /usr/local/bin/grparse-server
COPY --from=build /out/grparse-stream-client /usr/local/bin/grparse-stream-client
COPY --from=build /out/onnxruntime-lib/ /usr/local/lib/
RUN ldconfig
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/grparse-server"]
