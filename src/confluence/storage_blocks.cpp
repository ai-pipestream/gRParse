#include "storage_blocks.h"

#include <algorithm>
#include <string_view>

#include "storage_node.h"

namespace grparse::confluence {
namespace {

// The block constructs of the dialect, plus the XHTML block elements a page
// body can carry. Anything else is inline until it turns out to contain a
// block, at which point it is a transparent container.
bool named_block(const Node& node) {
  if (node.text_node) return false;
  if (node.prefix == "ac") {
    return node.name == "structured-macro" || node.name == "task-list" ||
           node.name == "image" || node.name == "layout" ||
           node.name == "layout-section" || node.name == "layout-cell" ||
           node.name == "adf-extension" || node.name == "adf-node";
  }
  if (!node.prefix.empty()) return false;
  // A line break is deliberately absent: it belongs to the text around it,
  // not beside it, so it stays inline and folds into the item as a newline.
  static constexpr std::string_view kBlocks[] = {
      "h1",    "h2",     "h3",     "h4",   "h5",  "h6",     "p",
      "ul",    "ol",     "li",     "table", "thead", "tbody", "tfoot",
      "tr",    "td",     "th",     "div",  "pre", "hr",     "blockquote",
      "section", "article", "aside", "header", "footer", "main", "nav",
      "figure", "figcaption", "dl", "dt", "dd"};
  return std::ranges::find(kBlocks, node.name) != std::end(kBlocks);
}

}  // namespace

bool is_block(const Node& node) {
  if (named_block(node)) return true;
  if (node.text_node) return false;
  return std::ranges::any_of(node.children,
                             [](const Node& child) { return is_block(child); });
}

bool contains_block(const Node& node) {
  return std::ranges::any_of(node.children,
                             [](const Node& child) { return is_block(child); });
}

}  // namespace grparse::confluence
