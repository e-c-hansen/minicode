// LineDiff.h — pure C++. A line diff between two versions of a text, and
// moving a position in the old text to the same place in the new one.
//
// Format Document replaces the whole buffer with a formatter's output. Doing
// that as one big replacement would throw the caret, the scroll position and
// the language server's view of the file away, so the edit is made as the
// smallest set of changed line runs (hunks) instead: lines the formatter left
// alone are never touched, and a position in them keeps its text.
//
// Offsets are UTF-16 code units, so they line up with NSString/NSRange.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace LineDiff {

// Lines [oldLine, oldLine + oldLines) of the old text became lines
// [newLine, newLine + newLines) of the new one. The same run in UTF-16
// offsets: [oldStart, oldStart + oldLength) -> [newStart, newStart + newLength).
// A line includes its "\n".
struct Hunk {
    size_t oldLine, oldLines, newLine, newLines;
    size_t oldStart, oldLength, newStart, newLength;
};

// The changed runs, in order, none overlapping or touching, and never two
// with only blank lines between them (those are joined). Empty when the
// texts are equal. Uses Myers' algorithm on lines after trimming the common
// head and tail; when the texts differ in more than `maxEdits` lines, the
// whole middle becomes one hunk (still correct, just coarser).
std::vector<Hunk> diff(const std::u16string& a, const std::u16string& b,
                       size_t maxEdits = 1000);

// Apply hunks to `a` (tests; the editor applies them one by one).
std::u16string apply(const std::u16string& a, const std::u16string& b,
                     const std::vector<Hunk>& hunks);

// Where offset `pos` of `a` is in `b`. Outside the hunks it moves by the
// length the hunks before it gained or lost. Inside a hunk, formatters
// mostly move whitespace, so the characters that are not whitespace are
// counted up to it and the same count is found in the new run: a caret
// before a word stays before that word, one after it stays after it, and
// one in a line's indentation stays at the start of its line.
size_t mapOffset(const std::u16string& a, const std::u16string& b,
                 const std::vector<Hunk>& hunks, size_t pos);

}  // namespace LineDiff
