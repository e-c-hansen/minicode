// MarkdownEdit.h — pure C++. The piece of Markdown source behind a spot in
// the rendered preview, and the edits the preview can make to it: replace
// that piece, or add a list item after it. Like LatexDoc for LaTeX, an edit
// replaces exactly one byte range, so nothing else in the file changes.
#pragma once
#include <string>

namespace MarkdownEdit {

struct Block {
    enum Kind { None, Paragraph, Heading, ListItem, Quote, Code, TableCell, TableRow };
    Kind kind = None;
    size_t start = 0, end = 0;   // the editable bytes of the source
    int firstLine = -1, lastLine = -1;
    // For a list item: what starts a new item after this one, indentation
    // and marker included ("  - ", or "4. " after a "3. ").
    std::string nextItemPrefix;
};

// The block on 0-based source `line`, classified the way MarkdownParser
// reads it. For a table row, `column` picks one cell (0-based; -1, or a
// column the row lacks, gives the whole row). What the block covers:
//   heading    the text after the #s
//   list item  the text after the marker, on its own line
//   paragraph  every line of it, blank line to blank line
//   quote      every line of it, the > markers included
//   code       the lines between the fences
//   table cell the cell's text, trimmed
// Blank lines, rules, fences and a table's separator row give None.
Block blockAt(const std::string& source, int line, int column = -1);

// The source with `block`'s bytes replaced by `text`.
std::string replace(const std::string& source, const Block& block,
                    const std::string& text);

// The source with a new list item holding `text` on the line after
// `block`, which must be a list item. `newStart` gets where `text` begins.
std::string addItem(const std::string& source, const Block& block,
                    const std::string& text, size_t* newStart = nullptr);

}  // namespace MarkdownEdit
