// MarkdownHtml.h — pure C++. A Markdown file as a standalone HTML page, for
// File > Export as HTML… (VS Code has no built-in export; this is minicode's
// own). It is written from the parser's runs, the same ones the preview
// draws, so the page says what the preview says: headings carry GitHub's
// anchors (MarkdownParser::anchor, repeats numbered as GitHub numbers them),
// lists nest as they do in the preview, quotes and the code and lists inside
// them keep their places, tables are tables with their columns' alignment,
// footnotes are a numbered list at the end with links both ways, task boxes
// are disabled checkboxes, a fenced block's language is its code element's
// class (language-python), and math is its TeX between \( \) or \[ \], for a
// page that loads MathJax or KaTeX to typeset (none is loaded: the page
// fetches nothing). Pictures keep their addresses as written when those are
// relative, https or data:image; any other address shows the alt text.
// Links keep relative, #, http, https and mailto addresses; any other
// scheme (javascript: and the like) leaves the text without a link. All
// text is escaped; the page carries a small stylesheet of its own that
// follows the reader's light or dark appearance.
#pragma once
#include <string>
#include <vector>

#include "MarkdownParser.h"

namespace MarkdownHtml {

// `text` with &, <, > and " escaped, for an element's text or an attribute.
std::string escape(const std::string& text);

// The page for `runs` (MarkdownParser::parse's), titled `title`.
std::string render(const std::vector<MdRun>& runs, const std::string& title);

// The same from Markdown source.
std::string page(const std::string& markdown, const std::string& title);

// The title a page gets: the first level-one heading's text, else the
// first heading's, else `fallback` (the file's name).
std::string titleFor(const std::vector<MdRun>& runs, const std::string& fallback);

}  // namespace MarkdownHtml
