# Go client

Streams a PDF or image to `ParseStreamingService/StreamProcessDocument` and
prints one line per page event as it arrives.

Prerequisites: Go 1.22+, `protoc`, and the Go codegen plugins:

```bash
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
```

Generate stubs from the repository's protos (they carry no `go_package`
option, so the script maps them into this module), then run:

```bash
cd examples/clients/go
./generate.sh
go mod tidy
go run . /path/to/document.pdf              # default localhost:50051
go run . /path/to/scan.png other-host:50051
```

Nothing generated is committed.
