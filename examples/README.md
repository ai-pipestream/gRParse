# Examples

Working, minimal consumers of the gRParse streaming contract. Each one stages
the repo-root `.proto` files itself, so nothing here drifts from the service.

| Example | What it shows |
|---|---|
| [`web-demo/`](web-demo/) | Browser page that uploads a document (or one click on the bundled sample) and paints each page event live: provenance boxes on a canvas, reading-order text, tables, picture classes, barcodes, per-page arrival times, and a JSON download of the full result. `docker compose --profile demo up --build` and open <http://localhost:8080>. |
| [`clients/python/`](clients/python/) | Terminal client: chunked bidi upload, one summary line per page event. |
| [`clients/go/`](clients/go/) | Same, in Go. |

The reference client is the C++ `grparse-stream-client` shipped in the service
image (`scripts/parse_pdf.sh` wraps it).
