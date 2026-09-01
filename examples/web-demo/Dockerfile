# Build from the repository root so the contract protos are in context:
#   docker build -f examples/web-demo/Dockerfile -t grparse-web-demo .
# Or just: docker compose --profile demo up --build
FROM node:26-alpine
WORKDIR /app
COPY examples/web-demo/package.json ./
RUN npm install --omit=dev
COPY examples/web-demo/server.js ./
COPY examples/web-demo/public ./public
COPY document.proto parse_types.proto parse.proto parse_stream.proto /protos/
# The FastWARC bridge resolves the fastwarc-grpc contract like the other
# native tabs: from the sibling checkout in dev, or the compose stack's
# bind mount at /fastwarc-grpc/proto in this image.
ENV GRPARSE_PROTO_DIR=/protos GRPARSE_TARGET=grparse:50051 PORT=8080
EXPOSE 8080
USER node
CMD ["node", "server.js"]
