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
// A row without a fixture (ebcdic: the corpus has no EBCDIC file) streams
// the UI's own bundled sample instead.
export interface ServiceUi {
  name: string;
  title: RegExp;
  flow: "stream" | "render" | "load-only";
  fixture?: string;
  // Stat counter that must read non-zero after the stream (stream flow).
  stat?: string;
  statLabel?: string;
  // Present only under an opt-in compose profile; skipped when /api/uis
  // does not list it.
  optional?: boolean;
}

export const SERVICE_UIS: ServiceUi[] = [
  { name: "pdf", title: /grpc-pdf-inspector/, flow: "stream", fixture: "hello-text.pdf", stat: "#sPages", statLabel: "pages" },
  { name: "libreoffice", title: /grlibre/, flow: "render", fixture: "sample3.docx" },
  { name: "lol-html", title: /grpc-lol-html/, flow: "stream", fixture: "deep_nesting.html", stat: "#sMatches", statLabel: "matches" },
  { name: "markup", title: /grpc-markup/, flow: "stream", fixture: "streaming-markup.md", stat: "#sBlocks", statLabel: "blocks" },
  { name: "epub", title: /grpc-epub/, flow: "stream", fixture: "two-chapters.epub", stat: "#sChapters", statLabel: "chapters" },
  { name: "xml", title: /grpc-xml/, flow: "stream", fixture: "jats-article.xml", stat: "#sTexts", statLabel: "texts" },
  { name: "email", title: /grpc-email/, flow: "stream", fixture: "multipart_with_attachments.eml", stat: "#sParts", statLabel: "parts" },
  { name: "ebcdic", title: /grpc-ebcdic/, flow: "stream", stat: "#sRows", statLabel: "rows" },
  { name: "calamine", title: /calamine/i, flow: "load-only", optional: true },
];

export function uiPath(ui: ServiceUi): string {
  return `/ui/${ui.name}/`;
}

export function describeUi(ui: ServiceUi): string {
  return `[${ui.name} UI ${uiPath(ui)}${ui.fixture ? ` | ${ui.fixture}` : " | bundled sample"}]`;
}
