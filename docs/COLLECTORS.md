# Collector strategy

gRParse is gRPC-first: it does not grow a parser for every format it accepts.
Each format family is owned by a standalone gRPC service that already does
that format well — fastwarc for WARC archives, LibreOfficeKit for office
documents, whisper.cpp for audio, quick-xml for XML, calamine-style row
readers for EBCDIC records, and so on. Every one of those services speaks
its own typed, streaming protobuf contract and ships on its own schedule.
gRParse itself is one more service in that fleet: its in-process contribution
is the CV path (PDF and raster images through Poppler, ONNX Runtime OCR, and
layout). The rule is that a parser lives in exactly one place, and everything
else reaches it over the wire.

The coordinator's side of that deal is small and uniform. gRParse vendors
each collector's `.proto` files byte-identical into `collectors/` and never
edits them — the owning repo keeps its wire contract. A collector is
configured with one environment variable (`GRPARSE_<NAME>_TARGET=host:port`);
an unconfigured collector is simply unavailable, not an error. When a parse
request arrives, the caller either names the collectors explicitly or leaves
the selection empty, in which case gRParse routes by filename and content
type: office formats to libreoffice, WARC to fastwarc, audio/video to asr,
`.eml`/`.msg` to email, XML and its archive forms to xml, `.epub` to epub,
text markup to markup, and PDF/raster to the in-process CV path — with one
twist: a PDF routes to the pdf inspector instead when one is configured,
and its classification then decides between the collector's own fast-path
Document (text-based) and a CV run whose OCR is restricted to the pages
the inspector named. Two
collectors are never routed to — EBCDIC, because raw records carry no
trustworthy format signal and a parse needs a caller-supplied layout, and
lol-html, because it does targeted CSS-selector extraction rather than
whole-document conversion. Both are reached by explicit selection.

Joining is stream-native on both sides. gRParse streams the source bytes
into each selected collector as the request arrives, and each collector
streams typed events back as it parses — records, chapters, rows, envelope
parts. Two fold shapes exist. Most collectors can project their own event
stream into a `Document` server-side (their `emit_document` option), so
gRParse asks for that event and drains the rest: the fold happens where the
events were made, and attribution stays with the collector. Three contracts
have no document event by design — libreoffice's page events, lol-html's
selector matches, and fastwarc's record stream — so for those, gRParse folds
client-side into the same `Document` shape. Either way, every collector's
contribution arrives as one `ai.pipestream.document.v1.Document` whose items
carry a `CollectorSource` tag.

The coordinator runs the selected collectors concurrently and merges their
documents additively in plan order: item references renumber, sources never
overwrite each other, and the merged result is deterministic regardless of
finish order. Choosing a winner among sources is deliberately left to
downstream consumers. Failure degrades instead of sinking the parse: a
collector that errors becomes a failure entry naming the collector and its
gRPC status, and the parse fails only when every selected collector fails.
On the streaming RPC, each collector's document is emitted as a
`CollectorDocument` event the moment that collector finishes, interleaved
with the CV path's page events, so a client watching the stream sees
multi-format results assemble in real time.

The mechanics — the routing table, per-collector env vars, fold details, and
the merge contract — are in the README's collector scatter-gather section;
the vendored contracts are documented in `collectors/README.md`. This
document is only about why it is built this way: one coordinator, many
independent best-of-breed parsers, joined by streams rather than by code.
