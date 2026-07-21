# syntax=docker/dockerfile:1.7
FROM nvidia/cuda:13.3.0-devel-ubuntu26.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ git make ninja-build pkg-config libopencv-dev libpoppler-cpp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN --mount=type=cache,target=/build cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build --target grparse-server grparse-stream-client --parallel 4 \
 && mkdir -p /out \
 && cp /build/grparse-server /out/grparse-server \
 && cp /build/grparse-stream-client /out/grparse-stream-client \
 && cp -a /build/_deps/onnxruntime-src/lib /out/onnxruntime-lib

FROM nvidia/cuda:13.3.0-runtime-ubuntu26.04
ENV GRPARSE_LISTEN_ADDRESS=0.0.0.0:50051 GRPARSE_MODELS_DIR=/models GRPARSE_PAGE_WORKERS=2
RUN apt-get update && apt-get install -y --no-install-recommends libcudnn9-cuda-13 libpoppler-cpp3 libopencv-core-dev libopencv-imgcodecs-dev libopencv-imgproc-dev \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/grparse-server /usr/local/bin/grparse-server
COPY --from=build /out/grparse-stream-client /usr/local/bin/grparse-stream-client
COPY --from=build /out/onnxruntime-lib/ /usr/local/lib/
RUN ldconfig
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/grparse-server"]
