import type { Page } from "@playwright/test";

// The tab model of the demo shell (examples/web-demo/README.md).
//
// Registry tabs come from DEMO_UIS in compose.stack.yaml: the shell proxies
// each service's own frontend under /ui/<name>/ and GET /api/uis lists them.
// Adding a parser to the stack is one row here.
export interface RegistryTab {
  name: string;
  title: string;
  // What the proxied frontend's own <title> looks like.
  uiTitle: RegExp;
}

export const REGISTRY_TABS: RegistryTab[] = [
  { name: "lol-html", title: "LOL HTML", uiTitle: /grpc-lol-html/ },
  { name: "libreoffice", title: "LibreOffice", uiTitle: /grlibre/ },
  { name: "pdf", title: "PDF Inspector", uiTitle: /grpc-pdf-inspector/ },
  { name: "epub", title: "EPUB", uiTitle: /grpc-epub/ },
  { name: "xml", title: "XML", uiTitle: /grpc-xml/ },
  { name: "markup", title: "Markup", uiTitle: /grpc-markup/ },
  { name: "ebcdic", title: "EBCDIC", uiTitle: /grpc-ebcdic/ },
  { name: "email", title: "Email", uiTitle: /grpc-email/ },
];

// Tabs the stack can carry under an opt-in profile. Their presence in
// /api/uis is allowed, never required.
export const OPTIONAL_REGISTRY_TABS: RegistryTab[] = [
  { name: "calamine", title: "Calamine", uiTitle: /calamine/i },
];

// Native tabs are pages the shell serves itself, each backed by a bridge
// status endpoint (shell.js NATIVE_TABS).
export interface NativeTab {
  name: string;
  title: string;
  page: string;
  status: string;
}

export const NATIVE_TABS: NativeTab[] = [
  { name: "document", title: "Document", page: "/document.html", status: "/api/document/status" },
  { name: "fastwarc", title: "FastWARC", page: "/fastwarc.html", status: "/api/fastwarc/status" },
  { name: "poic", title: "POI", page: "/poic.html", status: "/api/poic/status" },
  { name: "asr", title: "ASR", page: "/asr.html", status: "/api/asr/status" },
  { name: "enrich", title: "Enrich", page: "/enrich.html", status: "/api/enrich/status" },
  { name: "vlm-convert", title: "VLM Convert", page: "/vlm-convert.html", status: "/api/vlm-convert/status" },
];

export interface UiEntry {
  name: string;
  title: string;
  path: string;
  description: string;
  reachable: boolean;
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// The tab strip button whose label is exactly `title` (the status dot span
// carries no text, so the button's text is the label alone).
export function tabButton(page: Page, title: string) {
  return page
    .locator("nav.tab-strip button.tab")
    .filter({ hasText: new RegExp(`^\\s*${escapeRegExp(title)}\\s*$`) });
}

// Opens the shell (the gRParse tab is the landing pane) on domcontentloaded:
// the page keeps status polls and parse streams open, so a networkidle wait
// would never settle.
export async function openShell(page: Page) {
  await page.goto("/", { waitUntil: "domcontentloaded" });
}
