// What an ImageRef holds, as the page can show it.
//
// A producer that holds an image's bytes sends them in ImageRef.data; `uri`
// is for a reference, and for older producers that spell the bytes as a
// data: URI. The page shows images by URI, and JSON has no bytes, so the
// server turns held bytes into the data: URI they are before a Document
// goes to the browser.
"use strict";

// The image as one URI: its `uri` when set, else its bytes as a base64
// data: URI under the declared media type. null when the ref holds nothing.
function imageUri(image) {
  if (!image) return null;
  if (typeof image.uri === "string" && image.uri !== "") return image.uri;
  if (image.data && image.data.length > 0) {
    const declared = typeof image.mimetype === "string" ? image.mimetype : "";
    const type = declared.split(";")[0].trim().toLowerCase() || "application/octet-stream";
    return `data:${type};base64,${Buffer.from(image.data).toString("base64")}`;
  }
  return null;
}

function showable(image) {
  if (!image) return;
  const uri = imageUri(image);
  if (uri !== null) image.uri = uri;
  // The bytes are in the URI now; a Buffer would reach the page as a JSON
  // array of numbers.
  delete image.data;
}

// The Document with every image it holds (page images, pictures, code
// renders) in the form imageUri gives. Changes and returns `doc`.
function withShowableImages(doc) {
  if (!doc) return doc;
  for (const page of Object.values(doc.pages || {})) showable(page && page.image);
  for (const picture of doc.pictures || []) showable(picture && picture.image);
  for (const text of doc.texts || []) showable(text && text.code && text.code.image);
  return doc;
}

module.exports = { imageUri, withShowableImages };
