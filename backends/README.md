# backends/

The `PdfBackendService` contract (`ai.pipestream.parse.pdf.v1`), vendored
from the pipestream-protos release (source of truth: the `pdf-backend`
module there, first released in v0.15.0). `GRPARSE_PDF_BACKEND=<host:port>`
switches the PDF layer from the in-process poppler path to a backend
speaking this contract (grpc-pdfium, grpc-qparse, grpc-poppler); unset or
`inprocess` keeps the in-process path. See `docs/pdf-backend-services.md`
for the program these services belong to.

Refresh by copying the two files from a newer release tarball
(`pipestream-protos-<version>.tgz`, path
`pdf-backend/proto/ai/pipestream/parse/pdf/v1/`). The contract is
additive-only, so a refresh never breaks this client.
