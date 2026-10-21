#!/usr/bin/env python3
"""Local HTTP(S) origin for remote-sources-e2e-test.

usage: http_server.py <port> <sample> [<cert.pem> <key.pem>]
Routes: /sample.md, /redirect, /redirect-loop, /redirect-file, /download?id=1
(Content-Disposition), /needs-header (X-E2E-Token: e2e-token), /missing (404),
/error (500), /big (2 MiB). Every request is logged to stderr.
"""
import http.server
import ssl
import sys

port = int(sys.argv[1])
sample = open(sys.argv[2], "rb").read()


class Handler(http.server.BaseHTTPRequestHandler):
    def send_body(self, status, body, headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/sample.md":
            self.send_body(200, sample, [("Content-Type", "text/markdown")])
        elif path == "/redirect":
            self.send_body(302, b"", [("Location", "/sample.md")])
        elif path == "/redirect-loop":
            self.send_body(302, b"", [("Location", "/redirect-loop")])
        elif path == "/redirect-file":
            self.send_body(302, b"", [("Location", "file:///etc/hostname")])
        elif path == "/download":
            self.send_body(200, sample, [("Content-Disposition", 'attachment; filename="sample.md"')])
        elif path == "/needs-header":
            if self.headers.get("X-E2E-Token") == "e2e-token":
                self.send_body(200, sample)
            else:
                self.send_body(401, b"token required")
        elif path == "/error":
            self.send_body(500, b"boom")
        elif path == "/big":
            self.send_body(200, b"x" * (2 * 1024 * 1024))
        else:
            self.send_body(404, b"not found")


server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
if len(sys.argv) > 4:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(sys.argv[3], sys.argv[4])
    server.socket = context.wrap_socket(server.socket, server_side=True)
print(f"serving {'https' if len(sys.argv) > 4 else 'http'} on 127.0.0.1:{port}", file=sys.stderr, flush=True)
server.serve_forever()
