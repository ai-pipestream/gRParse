"""Emulated Google OAuth token endpoint and Google Drive v3 API for the
storage connector e2e test.

  POST /token                    jwt-bearer grant: verifies the RS256
                                 assertion with the public key given on the
                                 command line and checks its claims;
                                 refresh_token grant: checks the client and
                                 refresh token.
  GET  /drive/v3/files/<id>      file metadata, or the bytes with alt=media.
  POST /upload/drive/v3/files    multipart/related upload into a folder.
  GET  /_test/uploads            what was uploaded (name, parents, content
                                 as base64), for the test to read back.

Every Drive call needs the access token the refresh grant issued.

usage: mock_google.py <port> <public-key.pem> <seed-file> <seed-file-id>
"""
import base64
import json
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

PORT = int(sys.argv[1])
with open(sys.argv[2], "rb") as handle:
    PUBLIC_KEY = serialization.load_pem_public_key(handle.read())
with open(sys.argv[3], "rb") as handle:
    SEED_BYTES = handle.read()
SEED_ID = sys.argv[4]
CLIENT_ID = "e2e-client"
CLIENT_SECRET = "e2e-client-secret"
REFRESH_TOKEN = "e2e-refresh-token"
SA_TOKEN = "e2e-sa-access-token"
DRIVE_TOKEN = "e2e-drive-access-token"

FILES = {
    SEED_ID: {"id": SEED_ID, "name": "sample.md", "mimeType": "text/markdown",
              "size": str(len(SEED_BYTES)), "bytes": SEED_BYTES},
    "folder-out": {"id": "folder-out", "name": "out",
                   "mimeType": "application/vnd.google-apps.folder"},
    "gdoc-1": {"id": "gdoc-1", "name": "Notes",
               "mimeType": "application/vnd.google-apps.document"},
}
UPLOADS = []


def b64url(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("mock_google: " + (fmt % args) + "\n")

    def reply(self, status, body, content_type="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def authorized(self):
        if self.headers.get("Authorization") != "Bearer " + DRIVE_TOKEN:
            self.reply(401, {"error": "bad bearer"})
            return False
        return True

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/token":
            form = urllib.parse.parse_qs(self.body().decode())
            grant = form.get("grant_type", [""])[0]
            if grant == "urn:ietf:params:oauth:grant-type:jwt-bearer":
                header, claims, signature = form["assertion"][0].split(".")
                try:
                    PUBLIC_KEY.verify(b64url(signature), (header + "." + claims).encode(),
                                      padding.PKCS1v15(), hashes.SHA256())
                except Exception as error:  # noqa: BLE001
                    sys.stderr.write("mock_google: bad signature %r\n" % error)
                    return self.reply(400, {"error": "invalid_grant"})
                parsed_header = json.loads(b64url(header))
                parsed = json.loads(b64url(claims))
                ok = (parsed_header.get("alg") == "RS256"
                      and parsed.get("aud") == "http://%s/token" % self.headers["Host"]
                      and "devstorage" in parsed.get("scope", "")
                      and parsed.get("exp", 0) - parsed.get("iat", 0) == 3600)
                sys.stderr.write("mock_google: jwt header=%s claims=%s ok=%s\n"
                                 % (parsed_header, parsed, ok))
                if not ok:
                    return self.reply(400, {"error": "invalid_grant"})
                return self.reply(200, {"access_token": SA_TOKEN, "token_type": "Bearer",
                                        "expires_in": 3600})
            if grant == "refresh_token":
                ok = (form.get("client_id") == [CLIENT_ID]
                      and form.get("client_secret") == [CLIENT_SECRET]
                      and form.get("refresh_token") == [REFRESH_TOKEN])
                if not ok:
                    return self.reply(400, {"error": "invalid_grant"})
                return self.reply(200, {"access_token": DRIVE_TOKEN, "token_type": "Bearer",
                                        "expires_in": 3600, "scope": "drive"})
            return self.reply(400, {"error": "unsupported_grant_type"})
        if url.path == "/upload/drive/v3/files":
            if not self.authorized():
                return None
            query = urllib.parse.parse_qs(url.query)
            if query.get("uploadType") != ["multipart"]:
                return self.reply(400, {"error": "uploadType"})
            content_type = self.headers.get("Content-Type", "")
            boundary = content_type.split("boundary=", 1)[1].encode()
            parts = self.body().split(b"--" + boundary)
            meta_part, media_part = parts[1], parts[2]
            meta = json.loads(meta_part.split(b"\r\n\r\n", 1)[1].rsplit(b"\r\n", 1)[0])
            media = media_part.split(b"\r\n\r\n", 1)[1]
            media = media[:-2] if media.endswith(b"\r\n") else media
            parents = meta.get("parents", [])
            if parents != ["folder-out"]:
                return self.reply(404, {"error": "parent folder not found"})
            file_id = "up-%d" % len(UPLOADS)
            UPLOADS.append({"id": file_id, "name": meta["name"], "parents": parents,
                            "content": base64.b64encode(media).decode()})
            return self.reply(200, {"id": file_id, "name": meta["name"], "size": str(len(media))})
        return self.reply(404, {"error": "no route"})

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/_test/uploads":
            return self.reply(200, {"uploads": UPLOADS})
        if url.path.startswith("/drive/v3/files/"):
            if not self.authorized():
                return None
            file_id = urllib.parse.unquote(url.path[len("/drive/v3/files/"):])
            entry = FILES.get(file_id)
            if entry is None:
                return self.reply(404, {"error": "file not found"})
            if urllib.parse.parse_qs(url.query).get("alt") == ["media"]:
                return self.reply(200, entry["bytes"], "application/octet-stream")
            return self.reply(200, {k: v for k, v in entry.items() if k != "bytes"})
        return self.reply(404, {"error": "no route"})


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
