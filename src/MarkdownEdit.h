// MarkdownEdit.h — pure C++. The piece of Markdown source behind a spot in
// the rendered preview, and the edits the preview can make to it: replace
// that piece, or add a list item after it. Like LatexDoc for LaTeX, an edit
// replaces exactly one byte range, so nothing else in the file changes.
#pragma once
#include <string>
#include <utility>

namespace MarkdownEdit {

struct Block {
    enum Kind { None, Paragraph, Heading, ListItem, Quote, Code, TableCell, TableRow, Math, Html };
    Kind kind = None;
    size_t start = 0, end = 0;   // the editable bytes of the source
    int firstLine = -1, lastLine = -1;
    // For a list item: what starts a new item after this one, indentation
    // and marker included ("  - ", or "4. " after a "3. ").
    std::string nextItemPrefix;
};

// The block on 0-based source `line`, as MarkdownParser::lines reads it.
// For a table row, `column` picks one cell (0-based; -1, or a column the row
// lacks, gives the whole row). What the block covers:
//   heading    the text after the #s and before any closing #s, or all the
//              text lines of a setext (underlined) heading
//   list item  the text after the marker, with its continuation lines
//   paragraph  every line of it, lazy continuation lines included
//   quote      every line of the whole quote, the > markers included
//   code       the lines between the fences, or an indented block's lines
//              (a ```math fence's formula too)
//   math       a $$ (or \[) display formula on lines of its own, all of
//              it, the delimiters included
//   table cell the cell's text, trimmed
//   html       an HTML block's lines, as written (a heading written as a
//              line of HTML in one is a heading of its own)
// Blank lines, rules, fences, a table's separator row, a setext underline,
// link reference definitions and HTML comments give None.
Block blockAt(const std::string& source, int line, int column = -1);

// The source with `block`'s bytes replaced by `text`.
std::string replace(const std::string& source, const Block& block,
                    const std::string& text);

// The source with a new list item holding `text` on the line after
// `block`, which must be a list item. `newStart` gets where `text` begins.
std::string addItem(const std::string& source, const Block& block,
                    const std::string& text, size_t* newStart = nullptr);

// Where a word picked on a rendered page (a double-click's) sits in the
// Markdown `source` of its block: [first, second) bytes of `source`, or
// both std::string::npos when it is not there. `before` and `after` are the
// page's text on either side of it, which choose between the places the
// word occurs ("the" twice in a paragraph). Both sides are compared as
// lowercase letters and digits with every run of anything else one gap,
// so emphasis, links, inline code and escapes in the source are looked
// through; a whole word is preferred to a piece of a longer one.
std::pair<size_t, size_t> sourceOffsetFor(const std::string& source, const std::string& word,
                                          const std::string& before = "",
                                          const std::string& after = "");

// Where bytes [start, end) of `base`, the text an edit box opened on, are
// in `now`, the text when its edit is applied (an agent, a reload from
// disk or Cmd+Z may have changed it meanwhile): the same offsets when
// nothing changed, or the change lies wholly after them; shifted when it
// lies wholly before them; else the one place in `now` holding exactly
// those bytes. Both std::string::npos when the span's own text changed,
// is empty, or is in `now` more than once: the caller refuses the edit
// then, rather than splice at a guess. Either way only bytes equal to the
// ones the box showed are ever replaced, so applying never reverts what
// someone else wrote (the Markdown page's and the LaTeX preview's boxes,
// round 10's documents defect 3).
std::pair<size_t, size_t> rebaseSpan(const std::string& base, size_t start, size_t end,
                                     const std::string& now);

}  // namespace MarkdownEdit
