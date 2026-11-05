// SideBySide.h — pure C++. Two texts lined up row by row for a side-by-side
// diff (`minicode diff`, CompareView.mm): equal lines side by side, a changed
// run's lines paired off, and lines only one side has opposite an empty
// filler row, so both panes have the same number of rows and scroll together.
// Built on LineDiff, so the runs are the ones Format Document sees.
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

// The rows for old text `a` (left) and new text `b` (right). Runs over
// `maxEdits` changed lines are paired off as one run (see LineDiff::diff).
std::vector<Row> align(const std::u16string& a, const std::u16string& b,
                       size_t maxEdits = 2000);

// How many lines a text has: a last line without "\n" counts, an empty
// text has none.
size_t lineCount(const std::u16string& text);

// The rows that start a run of changes (for stepping through them).
std::vector<size_t> changeStarts(const std::vector<Row>& rows);

}  // namespace SideBySide
