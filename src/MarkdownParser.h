// MarkdownParser.h — pure C++. Turns Markdown into a flat list of styled runs.
// The GUI layer converts runs into an NSAttributedString.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct MdRun {
    std::string text;
    int  heading   = 0;      // 0 = body, 1..6 = # levels
    bool bold      = false;
    bool italic    = false;
    bool strike    = false;  // ~~struck through~~
    bool code      = false;  // inline `code` or fenced block content
    bool codeBlock = false;  // part of a fenced or indented code block
    bool quote     = false;  // inside a block quote
    bool rule      = false;  // horizontal rule (--- )
    bool table     = false;  // part of a table, laid out as padded monospace
    // Where a table run sits, for a GUI that draws real tables: tables are
    // numbered from 1 in the document, rows and columns from 0 (row 0 is the
    // header). Runs that only pad, separate or rule (tableCol == -1) exist
    // for the monospace layout and are skipped by such a GUI.
    int  tableId   = 0;
    int  tableRow  = -1;
    int  tableCol  = -1;
    int  tableCols = 0;
    int  tableAlign = 0;     // this column's alignment: 0 left, 1 center, 2 right
    int  listDepth = 0;      // 0 = not a list; >=1 nesting level
    bool ordered   = false;  // in an ordered list item
    // A list item's marker as text: two spaces per level, then the bullet
    // (•, ◦ or ▪ by level), the number as written ("3.") or a task's box
    // (☐ or ☑), then a space. A GUI with real indents can trim the spaces.
    bool marker    = false;
    bool link      = false;  // link text
    std::string url;         // populated when link == true
    int  line      = -1;     // the 0-based source line the run came from
    bool image     = false;  // ![alt](src): text is the alt text
    std::string src;         // the image's path or URL, as written
    // Vertical space. Every block ends with a "\n" run of its own; between
    // two blocks comes one more "\n" run with `gap` set, which a GUI may draw
    // shorter than a line (the items of a tight list have none between
    // them). `hardBreak` is a "\n" inside a paragraph: a line ending in two
    // spaces or a backslash, or <br>.
    bool gap       = false;
    bool hardBreak = false;
};

// How the parser read one line of the source. MarkdownEdit works from this,
// so what the preview shows and what a double-click edits cannot disagree.
struct MdLine {
    enum Kind {
        Blank, Fence, Code, Rule, TableHead, TableSep, TableRow,
        Heading,          // an ATX heading, or a line of a setext heading's text
        SetextUnderline,  // the === or --- under a setext heading
        Quote,            // a line starting with >
        ListItem,         // the line holding a list item's marker
        Text,             // a paragraph line, or a lazy continuation line
        Hidden            // a link reference definition or an HTML comment
    };
    Kind kind = Blank;
    // Lines that are edited together share a number: a paragraph, a list
    // item with its continuation lines, a whole block quote, a code block's
    // contents, a heading. -1 for a line with nothing of its own to edit.
    int block = -1;
    // Byte offsets into the source. [lineStart, lineEnd) is the whole line
    // without its newline (or a \r before it); [start, end) is its text:
    // after the indentation and any marker (the #s, the list marker; a
    // quote's start is its >), before a heading's closing #s or tag.
    size_t lineStart = 0, lineEnd = 0;
    size_t start = 0, end = 0;
};

class MarkdownParser {
public:
    static std::vector<MdRun> parse(const std::string& markdown);
    // One entry per source line, as parse() reads them.
    static std::vector<MdLine> lines(const std::string& markdown);
    // GitHub's anchor for a heading, what a "#section" link names:
    // lowercased, spaces as hyphens, and punctuation and symbols (ASCII or
    // not, § and emoji included) dropped. Letters and digits of any script,
    // '-' and '_' are kept.
    static std::string anchor(const std::string& headingText);
};
