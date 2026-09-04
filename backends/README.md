# backends/

The `PdfBackendService` contract (`ai.protomolt.parse.pdf.v1`). These are
the working copies; the standalone parser-protos repo
(`git.rokkon.com/ai-pipestream/parser-protos`) publishes the same bytes for
the backend services to pin. `GRPARSE_PDF_BACKEND=<host:port>`
switches the PDF layer from the in-process poppler path to a backend
speaking this contract (grpc-pdfium, grpc-qparse, grpc-poppler); unset or
`inprocess` keeps the in-process path. See `docs/pdf-backend-services.md`
for the program these services belong to.

A contract change lands here and in parser-protos
(`proto/ai/protomolt/parse/pdf/v1/`) with identical bytes, then the three
backend repos advance their `PDF_PROTOS_COMMIT` pins. The contract is
additive-only, so a refresh never breaks this client.
