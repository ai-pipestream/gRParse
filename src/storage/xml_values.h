// Reading values out of the XML listings object stores reply with (raw
// external data): Azure List Blobs and S3 ListObjectsV2.
#ifndef GRPARSE_STORAGE_XML_VALUES_H
#define GRPARSE_STORAGE_XML_VALUES_H

#include <string>
#include <string_view>
#include <vector>

namespace grparse::storage {

// Unescapes the XML character entities a blob name can carry in a listing.
inline std::string xml_unescape(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out += text[i];
      continue;
    }
    const size_t end = text.find(';', i);
    if (end == std::string_view::npos) {
      out += text.substr(i);
      break;
    }
    const std::string_view entity = text.substr(i + 1, end - i - 1);
    if (entity == "amp") out += '&';
    else if (entity == "lt") out += '<';
    else if (entity == "gt") out += '>';
    else if (entity == "quot") out += '"';
    else if (entity == "apos") out += '\'';
    else out += std::string(text.substr(i, end - i + 1));
    i = end;
  }
  return out;
}

// Every <tag>value</tag> in an XML document, in order (raw reply data from
// the store; the listing schemas used here nest no element of the same name).
inline std::vector<std::string> xml_values(const std::string& xml, const std::string& tag) {
  std::vector<std::string> values;
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  size_t at = 0;
  while ((at = xml.find(open, at)) != std::string::npos) {
    const size_t start = at + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) break;
    values.push_back(xml_unescape(std::string_view(xml).substr(start, end - start)));
    at = end + close.size();
  }
  return values;
}

}  // namespace grparse::storage

#endif
