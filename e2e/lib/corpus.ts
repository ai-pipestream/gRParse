import { readFileSync } from "node:fs";
import path from "node:path";

// The golden corpus under tests/golden/corpus is the only fixture source.
// From a checkout it is resolved relative to this file; inside the compose
// runner it is bind-mounted read-only and named by E2E_CORPUS_DIR.
export const CORPUS_DIR =
  process.env.E2E_CORPUS_DIR ?? path.resolve(__dirname, "..", "..", "tests", "golden", "corpus");

const MIME_BY_EXTENSION: Record<string, string> = {
  pdf: "application/pdf",
  docx: "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
  xlsx: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
  pptx: "application/vnd.openxmlformats-officedocument.presentationml.presentation",
  html: "text/html",
  md: "text/markdown",
  xml: "application/xml",
  eml: "message/rfc822",
  epub: "application/epub+zip",
  png: "image/png",
};

export interface UploadFile {
  name: string;
  mimeType: string;
  buffer: Buffer;
}

export function mimeTypeFor(fileName: string): string {
  const extension = fileName.toLowerCase().split(".").pop() ?? "";
  const mimeType = MIME_BY_EXTENSION[extension];
  if (!mimeType) throw new Error(`no MIME type registered for corpus file ${fileName}; add it to corpus.ts`);
  return mimeType;
}

// Loads a corpus file as the payload Playwright's setInputFiles accepts. The
// MIME type is set explicitly so the browser's own extension guess never
// decides how the bridge routes the upload.
export function corpusFile(fileName: string): UploadFile {
  const filePath = path.join(CORPUS_DIR, fileName);
  return { name: fileName, mimeType: mimeTypeFor(fileName), buffer: readFileSync(filePath) };
}
