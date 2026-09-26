// MarkdownParser.h — pure C++. Turns Markdown into a flat list of styled runs.
// The GUI layer converts runs into an NSAttributedString.
#pragma once
#include <string>
#include <vector>

struct MdRun {
    std::string text;
    int  heading   = 0;      // 0 = body, 1..6 = # levels
    bool bold      = false;
    bool italic    = false;
    bool code      = false;  // inline `code` or fenced block content
    bool codeBlock = false;  // part of a ``` fenced block
    bool quote     = false;  // blockquote line
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
    int  listDepth = 0;      // 0 = not a list; >=1 indent level
    bool ordered   = false;  // ordered list item marker
    bool link      = false;  // link text
    std::string url;         // populated when link == true
    int  line      = -1;     // the 0-based source line the run came from
    bool image     = false;  // ![alt](src): text is the alt text
    std::string src;         // the image's path or URL, as written
};

class MarkdownParser {
public:
    static std::vector<MdRun> parse(const std::string& markdown);
    // GitHub's anchor for a heading, what a "#section" link names:
    // lowercased (ASCII), spaces as hyphens, and every other ASCII character
    // but letters, digits, '-' and '_' dropped. Other scripts are kept.
    static std::string anchor(const std::string& headingText);
};
