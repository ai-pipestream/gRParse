// Streams one document to gRParse and prints each page event as it lands.
// Run generate.sh first; see README.md.
package main

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	parsev1 "github.com/ai-pipestream/gRParse/examples/clients/go/gen/parsev1"
)

const chunkBytes = 1024 * 1024

func contentTypeFor(path string) string {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".pdf":
		return "application/pdf"
	case ".png":
		return "image/png"
	case ".jpg", ".jpeg":
		return "image/jpeg"
	case ".tif", ".tiff":
		return "image/tiff"
	}
	return ""
}

func describePage(page *parsev1.PageData) string {
	digital, ocr := 0, 0
	for _, offset := range page.GetTextOffsets() {
		switch offset.GetSource() {
		case parsev1.TextSource_TEXT_SOURCE_DIGITAL_PDF:
			digital++
		case parsev1.TextSource_TEXT_SOURCE_OCR:
			ocr++
		}
	}
	barcodes := 0
	for _, picture := range page.GetPictures() {
		for _, annotation := range picture.GetAnnotations() {
			if annotation.GetMisc().GetKind() == "barcode" {
				barcodes++
			}
		}
	}
	return fmt.Sprintf("page=%d text_items=%d digital=%d ocr=%d tables=%d pictures=%d barcodes=%d",
		page.GetPageNumber(), len(page.GetTexts()), digital, ocr,
		len(page.GetTables()), len(page.GetPictures()), barcodes)
}

func run(documentPath, target string) error {
	source, err := os.Open(documentPath)
	if err != nil {
		return err
	}
	defer source.Close()

	connection, err := grpc.NewClient(target,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(128*1024*1024)))
	if err != nil {
		return err
	}
	defer connection.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()
	stream, err := parsev1.NewParseStreamingServiceClient(connection).StreamProcessDocument(ctx)
	if err != nil {
		return err
	}

	name := filepath.Base(documentPath)
	meta := parsev1.DocumentChunk{
		DocumentId:  name,
		Filename:    name,
		ContentType: contentTypeFor(documentPath),
	}
	buffer := make([]byte, chunkBytes)
	for {
		read, readErr := source.Read(buffer)
		if read > 0 {
			chunk := meta
			chunk.Data = buffer[:read]
			if err := stream.Send(&chunk); err != nil {
				return err
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return readErr
		}
	}
	final := meta
	final.Complete = true
	if err := stream.Send(&final); err != nil {
		return err
	}
	if err := stream.CloseSend(); err != nil {
		return err
	}

	pages := 0
	for {
		event, receiveErr := stream.Recv()
		if receiveErr == io.EOF {
			break
		}
		if receiveErr != nil {
			return receiveErr
		}
		switch payload := event.GetEvent().(type) {
		case *parsev1.DocumentStreamEvent_Page:
			pages++
			fmt.Println(describePage(payload.Page))
		case *parsev1.DocumentStreamEvent_Complete:
			fmt.Printf("complete total_pages=%d\n", event.GetTotalPages())
			for _, failure := range payload.Complete.GetCollectorFailures() {
				fmt.Fprintf(os.Stderr, "collector_failure: %s\n", failure.GetError())
			}
		case *parsev1.DocumentStreamEvent_CollectorDocument:
			document := payload.CollectorDocument.GetDocument()
			fmt.Printf("collector_document texts=%d tables=%d pictures=%d\n",
				len(document.GetTexts()), len(document.GetTables()), len(document.GetPictures()))
		}
	}
	if pages == 0 {
		return fmt.Errorf("no page events received")
	}
	return nil
}

func main() {
	if len(os.Args) != 2 && len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "Usage: %s DOCUMENT_PATH [HOST:PORT]\n", os.Args[0])
		os.Exit(64)
	}
	target := "localhost:50051"
	if len(os.Args) == 3 {
		target = os.Args[2]
	}
	if err := run(os.Args[1], target); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
