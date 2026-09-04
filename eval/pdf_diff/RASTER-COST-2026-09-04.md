# Raster-over-the-wire cost

Model DPI 200.0; times are one full-document Render stream, wall clock, BGR8 requested; the reference column is the in-process poppler render alone (no text pass, no image IO). Payload is the raster bytes crossing the wire; every request also re-sends the document bytes (stateless contract).

| doc | poppler in-process s | grpc-pdfium s / pages / payload MB | grpc-poppler s / pages / payload MB | grpc-qparse s / pages / payload MB |
|---|---|---|---|---|
| pdf-dummy | 0.01 | 0.06 / 1 / 11.06 | 0.05 / 1 / 11.06 | 0.06 / 1 / 14.75 |
| pdf-hello-text | 0.03 | 0.05 / 2 / 21.4 | 0.03 / 2 / 21.4 | 0.04 / 2 / 28.53 |
| pdf-long-text | 0.07 | 0.10 / 12 / 128.4 | 0.16 / 12 / 128.4 | 0.18 / 12 / 171.2 |
| pdf-mixed | 0.09 | 0.04 / 6 / 64.2 | 0.11 / 6 / 64.2 | 0.09 / 6 / 85.6 |
| pdf-scanned-image | 0.06 | 0.03 / 3 / 32.1 | 0.07 / 3 / 32.1 | 0.06 / 3 / 42.8 |
| pdf-two-column | 0.07 | 0.06 / 6 / 64.2 | 0.10 / 6 / 64.2 | 0.32 / 6 / 85.6 |
| pdf-rotated-scan | 0.05 | 0.02 / 1 / 10.7 | 0.05 / 1 / 10.7 | 0.05 / 1 / 14.27 |
| pdf-form | 0.01 | 0.02 / 1 / 10.7 | 0.02 / 1 / 10.7 | 0.02 / 1 / 14.27 |
| pdf-rotated-scan-180 | 0.05 | 0.02 / 1 / 10.7 | 0.05 / 1 / 10.7 | 0.04 / 1 / 14.27 |
| pdf-rotated-scan-mixed | 0.14 | 0.07 / 3 / 32.1 | 0.14 / 3 / 32.1 | 0.11 / 3 / 42.8 |
