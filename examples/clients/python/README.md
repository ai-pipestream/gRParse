# Python client

Streams a PDF or image to `ParseStreamingService/StreamProcessDocument` and
prints one line per page event as it arrives, mirroring the bundled C++
`grparse-stream-client`.

```bash
cd examples/clients/python
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./generate.sh                      # generates stubs from the repo's protos into gen/
python client.py /path/to/document.pdf            # default localhost:50051
python client.py /path/to/scan.png other-host:50051
```

`generate.sh` stages the four contract files from the repository root into the
`ai/pipestream/...` layout their imports use and runs `grpc_tools.protoc`, so
the stubs always match the checked-out contract. Nothing generated is
committed.
