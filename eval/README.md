# Oracle comparisons

Opt-in batteries that score gRParse against an external reader on a real
corpus. Nothing here runs in the CI gate; each script exits 77 (the CTest
skip code) when its inputs are not configured, so a green build never
means one of these ran.

## compare_vlm.py: an open vision-language model as oracle

Asks a running gRParse for markdown and an OpenAI-compatible vision
endpoint for markdown page by page, then scores agreement (letter
similarity, word recall/precision, heading and table-row counts) and
records timing and tokens per second. The endpoint is whatever serves the
model; `grpc-vlm-convert/serving/north-micro-vision/` serves Cohere Labs'
North Micro Vision Instruct (Apache 2.0) on NVIDIA, Intel XPU or CPU, so
the same corpus can be run once per accelerator with `EVAL_LABEL` naming
the leg.

```sh
uv run --with grpcio --with grpcio-tools \
  env GRPARSE_TARGET=localhost:50051 VLM_ENDPOINT=http://localhost:8086 \
      EVAL_CORPUS=/path/to/pdfs EVAL_LABEL=cuda \
  python eval/compare_vlm.py
```

Outputs land in `eval/out/<label>/`: the two markdowns per file,
`report.json`, and `report.md`. gRParse's gRPC must be reachable from the
host (`compose.stack.expose-grpc.yaml` publishes it); the Intel leg of
gRParse itself is `compose.stack.openvino.yaml`.

The oracle is not ground truth. High agreement says both read the page the
same way; low agreement says where to look, and the two markdowns are kept
side by side for exactly that.
