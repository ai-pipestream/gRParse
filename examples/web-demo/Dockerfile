# Build from the repository root so the contract protos are in context:
#   docker build -f examples/web-demo/Dockerfile -t grparse-web-demo .
# Or just: docker compose --profile demo up --build
#
# dhi.io node runtime has no npm; build deps in the -dev image, then copy
# into the runtime image pinned by digest.
FROM dhi.io/node:26-debian13-dev@sha256:c2938e3b86c1e127e0dc585f69c998a8463049dc8eab96ec622ac111cee7ba3b AS build
WORKDIR /app
COPY examples/web-demo/package.json ./
RUN npm install --omit=dev
COPY examples/web-demo/server.js examples/web-demo/peers.js ./
COPY examples/web-demo/public ./public
COPY document.proto parse_types.proto parse.proto parse_stream.proto /protos/
COPY collectors/ebcdic.proto collectors/lolhtml_types.proto collectors/lolhtml_service.proto \
  collectors/opennlp_document.proto collectors/opennlp_annotations.proto /protos/collectors/
# The peers' contracts ride in the image: examples/web-demo/peer-protos holds
# a byte-identical copy of every sibling's proto include tree (written by
# sync-peer-protos.sh), copied here to the layout the resolver probes for a
# sibling checkout ("/<repo>/<include-root>/..."). A bind mount at the same
# path still overrides a baked tree for local contract work.
COPY examples/web-demo/peer-protos/ /
ENV GRPARSE_PROTO_DIR=/protos GRPARSE_TARGET=grparse:50051 PORT=8080
# Build gate: every contract in peers.js must load from this filesystem, with
# no network, or the image does not exist.
RUN node server.js --check-protos
# Collect peer trees for the runtime stage (runtime image has no shell).
RUN mkdir -p /out/peers && \
    for d in /fastwarc-grpc /grpc-asr /grpc-calamine /grpc-ebcdic /grpc-email \
      /grpc-enrich /grpc-epub /grpc-libreoffice /grpc-lol-html /grpc-markup \
      /grpc-pdf-inspector /grpc-vlm-convert /grpc-xml; do \
      [ -d "$d" ] && cp -a "$d" /out/peers/; \
    done

FROM dhi.io/node:26-debian13@sha256:77d15c6857a104f84ae0edb3a442e25d3a9da9a34f904f23d995b4c072693582
WORKDIR /app
COPY --from=build /app /app
COPY --from=build /protos /protos
COPY --from=build /out/peers/ /
ENV GRPARSE_PROTO_DIR=/protos GRPARSE_TARGET=grparse:50051 PORT=8080
EXPOSE 8080
# Runtime image already runs as node (uid 1000).
CMD ["node", "server.js"]
