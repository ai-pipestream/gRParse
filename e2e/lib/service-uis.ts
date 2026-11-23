// The service frontends the shell proxies under /ui/<name>/. Two UI
// families exist in the fleet:
//
// - "stream": the Rust and Java demos share one page shape: a `#pick`
//   file input, a `#run` button that disables itself for the duration of
//   the stream, a `#verdict` line that drops its `idle` class once the
//   first content event lands, and per-kind stat counters.
// - "render": the libreoffice frontend renders on file selection and
//   reveals `#doc-card` plus one `#page-grid` cell per page.
//
// A row without a fixture streams the UI's own bundled sample instead.
export interface ServiceUi {
  name: string;
  // Tells two rows of one frontend apart in the test title.
  variant?: string;
  title: RegExp;
  flow: "stream" | "render" | "load-only";
  fixture?: string;
  // Stat counter that must read non-zero after the stream (stream flow).
  stat?: string;
  statLabel?: string;
  // Present only under an opt-in compose profile; skipped when /api/uis
  // does not list it.
  optional?: boolean;
  // Stream flow, ebcdic frontend: the layout typed into its `#layout` box
  // (COBOL copybook source), the code page picked in `#encoding`, and
  // whether `#emitDoc` asks the service to fold a Document too, which the
  // page logs as "folded Document received".
  layout?: string;
  encoding?: RegExp;
  emitDocument?: boolean;
}

// The copybook of tests/golden/corpus/formats/record.ebcdic: one 80-byte
// record in code page 037 (tests/golden/build_format_fixtures.py,
// ebcdic_record).
const GOLDEN_RECORD_COPYBOOK = [
  "000100* Golden corpus record, tests/golden/build_format_fixtures.py.",
  "000200 01  GOLDEN-RECORD.",
  "000300     05  REC-ID               PIC 9(5).",
  "000400     05  REC-NAME             PIC X(25).",
  "000500     05  REC-DATE             PIC X(10).",
  "000600     05  REC-AMOUNT           PIC 9(8)V99.",
  "000700     05  REC-CURRENCY         PIC X(3).",
  "000800     05  FILLER               PIC X(27).",
  "",
].join("\n");

export const SERVICE_UIS: ServiceUi[] = [
  { name: "pdf", title: /grpc-pdf-inspector/, flow: "stream", fixture: "hello-text.pdf", stat: "#sPages", statLabel: "pages" },
  { name: "libreoffice", title: /grlibre/, flow: "render", fixture: "sample3.docx" },
  { name: "lol-html", title: /grpc-lol-html/, flow: "stream", fixture: "deep_nesting.html", stat: "#sMatches", statLabel: "matches" },
  { name: "markup", title: /grpc-markup/, flow: "stream", fixture: "streaming-markup.md", stat: "#sBlocks", statLabel: "blocks" },
  { name: "epub", title: /grpc-epub/, flow: "stream", fixture: "two-chapters.epub", stat: "#sChapters", statLabel: "chapters" },
  { name: "xml", title: /grpc-xml/, flow: "stream", fixture: "jats-article.xml", stat: "#sTexts", statLabel: "texts" },
  { name: "email", title: /grpc-email/, flow: "stream", fixture: "multipart_with_attachments.eml", stat: "#sParts", statLabel: "parts" },
  { name: "ebcdic", title: /grpc-ebcdic/, flow: "stream", stat: "#sRows", statLabel: "rows" },
  // The corpus record through the same bidirectional Parse, with the
  // Document fold gRParse's ebcdic leg asks for (emit_document).
  {
    name: "ebcdic",
    variant: "corpus record",
    title: /grpc-ebcdic/,
    flow: "stream",
    fixture: "formats/record.ebcdic",
    stat: "#sRows",
    statLabel: "rows",
    layout: GOLDEN_RECORD_COPYBOOK,
    encoding: /037/,
    emitDocument: true,
  },
  { name: "calamine", title: /calamine/i, flow: "load-only", optional: true },
];

export function uiPath(ui: ServiceUi): string {
  return `/ui/${ui.name}/`;
}

export function describeUi(ui: ServiceUi): string {
  return `[${ui.name} UI ${uiPath(ui)}${ui.fixture ? ` | ${ui.fixture}` : " | bundled sample"}]`;
}
