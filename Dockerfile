# syntax=docker/dockerfile:1.26

# The runtime base is swappable so a hardened mirror can slot in without a
# Dockerfile change, for example:
#   --build-arg GRPARSE_RUNTIME_IMAGE=docker.io/pipestreamai/dhi-nvidia-cuda:<tag>
# The runtime stage asks nothing of the base beyond glibc (matching or newer
# than the build stage's ubuntu26.04) and the CUDA runtime libraries on the
# loader path: no shell, no package manager, and no ldconfig run.
ARG GRPARSE_RUNTIME_IMAGE=nvidia/cuda:13.3.1-runtime-ubuntu26.04

FROM nvidia/cuda:13.3.1-devel-ubuntu26.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake curl g++ git make ninja-build pkg-config xz-utils \
    libopencv-dev libfreetype-dev libfontconfig-dev libjpeg-dev libopenjp2-7-dev \
    liblcms2-dev libboost-dev \
    && rm -rf /var/lib/apt/lists/*

# Poppler is vendored from source instead of taken from the distro: ubuntu
# 26.04 ships 26.01.0, which predates the 26.06 thread-safety fixes in annots
# loading (upstream 4aca25d6, 2f10803d) that bit this server on arm64. Only
# the cpp frontend and the splash renderer are built.
ARG POPPLER_VERSION=26.08.0
ARG POPPLER_SHA256=dc906e68cea698109706ac6aa3d2c9d4512fcfcac42d90b8afcda486d1b9abd0
RUN curl -fsSL -o /tmp/poppler.tar.xz "https://poppler.freedesktop.org/poppler-${POPPLER_VERSION}.tar.xz" \
 && echo "${POPPLER_SHA256}  /tmp/poppler.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/poppler.tar.xz -C /tmp \
 && cmake -S "/tmp/poppler-${POPPLER_VERSION}" -B /tmp/poppler-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/poppler -DCMAKE_INSTALL_LIBDIR=lib \
      -DENABLE_CPP=ON -DENABLE_QT5=OFF -DENABLE_QT6=OFF -DENABLE_GLIB=OFF -DENABLE_UTILS=OFF \
      -DENABLE_BOOST=ON -DENABLE_NSS3=OFF -DENABLE_GPGME=OFF -DENABLE_LIBCURL=OFF \
      -DENABLE_LIBTIFF=OFF -DENABLE_LIBOPENJPEG=openjpeg2 -DBUILD_CPP_TESTS=OFF \
      -DBUILD_GTK_TESTS=OFF -DBUILD_QT5_TESTS=OFF -DBUILD_QT6_TESTS=OFF -DBUILD_MANUAL_TESTS=OFF \
 && cmake --build /tmp/poppler-build --parallel 4 \
 && cmake --install /tmp/poppler-build \
 && rm -rf /tmp/poppler.tar.xz "/tmp/poppler-${POPPLER_VERSION}" /tmp/poppler-build

WORKDIR /src
COPY . .
# The cache id includes ABI-sensitive dependency versions. Update it whenever
# gRPC, ONNX Runtime, CUDA, the base toolchain, or a dependency patch under
# patches/ changes — a stale cache would keep an unpatched dependency tree.
# Cache-mounted build trees outlive proto changes, and ninja cannot see a
# COPY-ed proto as newer than a cached generated header, so a content stamp
# decides: any proto change discards the staged and generated trees, which
# forces regeneration; everything else stays warm.
RUN --mount=type=cache,id=grparse-ubuntu26-cuda13-grpc1.83.0-ort1.29.0-poppler26.08-cxx23-sessionep2-static1-simdutf9,sharing=locked,target=/build \
    export PKG_CONFIG_PATH=/opt/poppler/lib/pkgconfig \
 && PROTO_SUM=$(cat *.proto collectors/*.proto | sha256sum | cut -d' ' -f1) \
 && if [ "$(cat /build/.proto-sum 2>/dev/null)" != "$PROTO_SUM" ]; then \
      rm -rf /build/proto /build/generated && printf '%s' "$PROTO_SUM" > /build/.proto-sum; \
    fi \
 && cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
      -DGRPARSE_WERROR=ON \
 && cmake --build /build --target grparse-server grparse-stream-client grparse-tests --parallel 4 \
 && LD_LIBRARY_PATH=/opt/poppler/lib ctest --test-dir /build --output-on-failure -L grparse \
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
# The Liberation fonts are poppler's base-14 substitutes: a PDF that uses
# Helvetica/Times/Courier without embedding them renders blank text without
# a metric-compatible substitute, which starves layout detection and page
# previews of pixels. fc-cache prebuilds the fontconfig cache so the
# read-only runtime never tries to write one.
# The font set is pinned rather than inherited: fontconfig substitutes
# whatever it happens to find, so an image that carries a different set
# rasterizes non-embedded text to different pixels than its siblings, and a
# page the images disagree about cannot be compared between them. The CUDA
# image used to inherit DejaVu from its base while the others had Liberation
# alone; all three now ask for the same fonts explicitly.
RUN apt-get update && apt-get install -y --no-install-recommends libcudnn9-cuda-13 \
    fonts-liberation fonts-dejavu-core fontconfig \
    && rm -rf /var/lib/apt/lists/* \
    && fc-cache -f \
    && mkdir -p /out/runtime-libs \
    && cp -a /usr/lib/x86_64-linux-gnu/libcudnn* /out/runtime-libs/ \
    && for f in /out/grparse-server /out/grparse-stream-client \
                /out/onnxruntime-lib/*.so* /usr/lib/x86_64-linux-gnu/libcudnn*.so*; do \
         LD_LIBRARY_PATH=/out/onnxruntime-lib:/opt/poppler/lib ldd "$f" 2>/dev/null; \
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
# Fontconfig's configuration, the Liberation base-14 substitutes, and the
# prebuilt font cache: PDFs with embedded fonts never needed any of this,
# but non-embedded Helvetica/Times/Courier text would otherwise rasterize
# blank — invisible to layout detection and page previews.
COPY --from=build /etc/fonts /etc/fonts
COPY --from=build /usr/share/fonts /usr/share/fonts
COPY --from=build /var/cache/fontconfig /var/cache/fontconfig
COPY --from=build /out/grparse-server /usr/local/bin/grparse-server
COPY --from=build /out/grparse-stream-client /usr/local/bin/grparse-stream-client
USER 65532:65532
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/grparse-server"]
