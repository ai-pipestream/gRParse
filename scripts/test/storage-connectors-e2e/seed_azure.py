"""Seeds Azurite for the storage connector e2e test with its own Shared Key
signer (independent of the one under test): creates the containers and
uploads the input blobs.

usage: seed_azure.py <blob-endpoint> <sample-file>
"""
import base64
import email.utils
import hashlib
import hmac
import sys
import urllib.parse
import urllib.error
import urllib.request

ACCOUNT = "devstoreaccount1"
# Azurite's well-known development key (public, documented by Microsoft).
KEY = base64.b64decode(
    "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==")
ENDPOINT = sys.argv[1].rstrip("/")
with open(sys.argv[2], "rb") as handle:
    SAMPLE = handle.read()


def send(method, path, query, body=b"", extra=None):
    headers = {"x-ms-date": email.utils.formatdate(usegmt=True), "x-ms-version": "2021-08-06"}
    headers.update(extra or {})
    resource = urllib.parse.urlparse(ENDPOINT + path).path
    canonical_headers = "".join("%s:%s\n" % (k, headers[k]) for k in sorted(headers))
    canonical_query = "".join("\n%s:%s" % (k, v) for k, v in sorted(query.items()))
    content_type = "application/octet-stream"
    to_sign = "\n".join([method, "", "", str(len(body)) if body else "", "", content_type, "", "",
                         "", "", "", ""]) + "\n" + canonical_headers + "/" + ACCOUNT + resource + canonical_query
    signature = base64.b64encode(hmac.new(KEY, to_sign.encode(), hashlib.sha256).digest()).decode()
    headers["Content-Type"] = content_type
    headers["Authorization"] = "SharedKey %s:%s" % (ACCOUNT, signature)
    url = ENDPOINT + path + ("?" + urllib.parse.urlencode(query) if query else "")
    request = urllib.request.Request(url, data=body, method=method,
                                     headers=headers)
    try:
        with urllib.request.urlopen(request) as reply:
            print("seed_azure: %s %s -> %d" % (method, path, reply.status))
    except urllib.error.HTTPError as error:
        print("seed_azure: %s %s -> %d %s" % (method, path, error.code, error.read().decode()))
        raise


for container in ("grparse-in", "grparse-out"):
    send("PUT", "/" + container, {"restype": "container"})
send("PUT", "/grparse-in/docs/sample.md", {}, SAMPLE, {"x-ms-blob-type": "BlockBlob"})
send("PUT", "/grparse-in/two/a.md", {}, b"a", {"x-ms-blob-type": "BlockBlob"})
send("PUT", "/grparse-in/two/b.md", {}, b"b", {"x-ms-blob-type": "BlockBlob"})
