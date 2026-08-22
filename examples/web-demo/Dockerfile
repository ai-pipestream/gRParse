# Build from the repository root so the contract protos are in context:
#   docker build -f examples/web-demo/Dockerfile -t grparse-web-demo .
# Or just: docker compose --profile demo up --build
FROM node:24-alpine
WORKDIR /app
COPY examples/web-demo/package.json ./
RUN npm install --omit=dev
COPY examples/web-demo/server.js ./
COPY examples/web-demo/public ./public
COPY document.proto parse_types.proto parse.proto parse_stream.proto /protos/
# The FastWARC bridge stages the vendored collector contract from
# $GRPARSE_PROTO_DIR/collectors; without it the fastwarc tab is dead in
# the containerized shell while working fine from a source checkout.
COPY collectors/warc.proto collectors/warc_service.proto /protos/collectors/
ENV GRPARSE_PROTO_DIR=/protos GRPARSE_TARGET=grparse:50051 PORT=8080
EXPOSE 8080
USER node
CMD ["node", "server.js"]
