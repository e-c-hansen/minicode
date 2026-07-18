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
    int  listDepth = 0;      // 0 = not a list; >=1 indent level
    bool ordered   = false;  // ordered list item marker
    bool link      = false;  // link text
    std::string url;         // populated when link == true
};

class MarkdownParser {
public:
    static std::vector<MdRun> parse(const std::string& markdown);
};
