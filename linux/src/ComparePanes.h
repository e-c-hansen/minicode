// ComparePanes.h — what the compare view shows, worked out without GTK (the
// Mac's MCComparison prepare, src/CompareView.mm): two texts lined up row by
// row (SideBySide::align), each pane's text as UTF-8 with its syntax colors
// (by the file's name; plain past 2 MB a side) and the words that changed
// inside the changed rows (budgeted), all as byte offsets into the pane's
// text. Costly for big texts, so Editor::showComparison runs it on a thread.
// linux/PARITY.md item 3.2.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "SideBySide.h"

// What is compared: two UTF-8 texts and what each one is.
struct CompareSpec {
    std::string title;        // the tab's title
    std::string leftLabel;    // "HEAD: src/a.py"
    std::string rightLabel;   // "src/a.py"
    std::string leftText, rightText;
    std::string path;         // the file on the right, when it is one
    std::string fileName;     // whose extension picks the syntax colors; path's when empty
};

struct ComparePane {
    std::string text;                  // rows joined by "\n", a filler row empty
    std::vector<size_t> starts;        // byte offset of each row; the last is text.size() + 1
    std::vector<int> line;             // the side's 0-based line on each row, or -1
    struct Piece {
        size_t start, length;          // bytes of text
        int style;                     // a TokenStyle (syntax); 0 for a word
    };
    std::vector<Piece> syntax;         // ascending, cut at row ends
    std::vector<Piece> words;          // ascending
    size_t columns = 0;                // the widest row, tabs as 8
    // Row r's bytes.
    std::string rowText(size_t r) const;
};

struct CompareData {
    CompareSpec spec;
    std::vector<SideBySide::Row> rows;
    std::vector<SideBySide::Hunk> hunks;
    ComparePane panes[2];              // [0] left, [1] right
    size_t changedRows = 0;
};

// Lines the texts up and works out both panes. Thread-safe (no shared state);
// the caller owns the result.
CompareData* prepareComparison(const CompareSpec& spec);
