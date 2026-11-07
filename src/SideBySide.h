// SideBySide.h — pure C++. Two texts lined up row by row for a side-by-side
// diff (`minicode diff`, CompareView.mm): equal lines side by side, a changed
// run's lines paired off, and lines only one side has opposite an empty
// filler row, so both panes have the same number of rows and scroll together.
// Built on LineDiff, so the runs are the ones Format Document sees.
//
// Also what the compare view needs to be a review tool: each pane's text,
// the words that changed inside a changed pair of lines, the runs of changes
// (hunks) to step through with F7, and the text made of the hunks a user
// accepted (the Claude Code IDE link's openDiff, answered per hunk).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace SideBySide {

enum class Kind { Same, Changed, Removed, Added };

// One row: a line of each side (0-based), or -1 where that side has a filler.
// Same and Changed have both; Removed only the left; Added only the right.
struct Row {
    Kind kind;
    int left;
    int right;
};

// The rows for old text `a` (left) and new text `b` (right). LineDiff splits
// a large rewrite into its runs too; only past `maxEdits` changed lines (or
// its time budget) is the middle paired off as one run (see LineDiff::diff).
std::vector<Row> align(const std::u16string& a, const std::u16string& b,
                       size_t maxEdits = 40000);

// How many lines a text has: a last line without "\n" counts, an empty
// text has none.
size_t lineCount(const std::u16string& text);

// The rows that start a run of changes (for stepping through them).
std::vector<size_t> changeStarts(const std::vector<Row>& rows);

// A run of rows that are not Same: rows [first, end).
struct Hunk {
    size_t first, end;
};
std::vector<Hunk> hunks(const std::vector<Row>& rows);

// Where to go from row `from` to the next (forward) or previous run of
// changes, wrapping around as VS Code's F7 and Shift+F7 do: the index into
// hunks(), or -1 when there are none. A run that starts at `from` counts as
// the current one, so forward moves past it and backward to the one before;
// -1 (before the first row) makes forward start at the top.
int stepHunk(const std::vector<Hunk>& hunks, long from, bool forward);

// One pane's text: row r holds that side's line (its "\n" and a "\r" before
// it left out) or nothing on a filler row, and rows are joined by "\n".
// starts[r] is where row r begins. Lines are split at "\n" only, as align
// counts them, so a lone "\r" or U+2028 never puts the panes out of step.
std::u16string paneText(const std::u16string& text, const std::vector<Row>& rows,
                        bool left, std::vector<size_t>* starts);

// A piece of text: [start, start + length), with a style the caller gives
// meaning to (a TokenStyle for syntax colors).
struct Span {
    size_t start, length;
    int style;
};

// Spans over `text` (offsets into it, in order) moved onto its pane: each
// is cut at line breaks and placed on the row that shows its line. Pieces
// on lines the pane does not show, or on a line's "\r\n", are left out.
std::vector<Span> spansOnPane(const std::u16string& text, const std::vector<Row>& rows,
                              bool left, const std::vector<size_t>& starts,
                              const std::vector<Span>& spans);

// What changed inside one changed pair of lines, word by word: the pieces
// of the old line and of the new one that are not in both (style 0). Words
// are runs of letters, digits and "_" (and anything past ASCII), blanks are
// runs of spaces and tabs, any other character is a word of its own; two
// changes with only blanks between them are one. Both lists are empty when
// the lines share nothing but blanks (the line's own tint says it all), or
// when they are equal.
struct WordChanges {
    std::vector<Span> left, right;
};
WordChanges wordChanges(const std::u16string& oldLine, const std::u16string& newLine);

// The text made of the hunks chosen: hunk i takes the new text's lines when
// accepted[i], else the old text's. Lines outside the hunks are the new
// text's (they are equal, but for the very last line's "\n"). With every
// hunk accepted this is `b`, with none `a` up to that last "\n".
std::u16string assemble(const std::u16string& a, const std::u16string& b,
                        const std::vector<Row>& rows, const std::vector<bool>& accepted);

}  // namespace SideBySide
