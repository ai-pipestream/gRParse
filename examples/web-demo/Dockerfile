# Build from the repository root so the contract protos are in context:
#   docker build -f examples/web-demo/Dockerfile -t grparse-web-demo .
# Or just: docker compose --profile demo up --build
FROM node:26-alpine
WORKDIR /app
COPY examples/web-demo/package.json ./
RUN npm install --omit=dev
COPY examples/web-demo/server.js examples/web-demo/peers.js ./
COPY examples/web-demo/public ./public
COPY document.proto parse_types.proto parse.proto parse_stream.proto /protos/
# The peers' contracts ride in the image: examples/web-demo/peer-protos holds
# a byte-identical copy of every sibling's proto include tree (written by
# sync-peer-protos.sh), copied here to the layout the resolver probes for a
# sibling checkout ("/<repo>/<include-root>/..."). A bind mount at the same
# path still overrides a baked tree for local contract work.
COPY examples/web-demo/peer-protos/ /
ENV GRPARSE_PROTO_DIR=/protos GRPARSE_TARGET=grparse:50051 PORT=8080
EXPOSE 8080
USER node
# Build gate: every contract in peers.js must load from this filesystem, with
# no network, or the image does not exist.
RUN node server.js --check-protos
CMD ["node", "server.js"]
