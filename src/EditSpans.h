// EditSpans.h — several replacements over one text, in the shape every
// port's multi-range edit takes (linux/PARITY.md, D5): the Mac's
// replaceRanges:withStrings:actionName: (CodeTextView, Lsp.h) and the GTK
// port's MultiEdit::apply (linux/src/MultiEdit.h). Each span replaces
// `length` units at `start` with `newLength` units. They come in ascending
// order and never overlap (two may touch, and several insertions may share
// one place), and are applied last first, so that every start is still
// where it was when its turn comes. Pure C++17, no I/O.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace EditSpans {

struct Span {
    std::size_t start = 0;
    std::size_t length = 0;
    std::size_t newLength = 0;
};

// Ascending, apart (each starts at or after the end of the one before),
// and inside a text `size` units long.
bool valid(const std::vector<Span>& spans, std::size_t size);

// Where offset `at` of the text before the spans is in the text after:
// moved by what the spans before it changed; inside a span, as far into
// its new text as it was into the old, but not past its end; at an
// insertion's place, after the inserted text. The Mac's find bar puts the
// caret back the same way after Replace All (EditorFind.mm, applyEdits).
std::size_t mapThrough(std::size_t at, const std::vector<Span>& spans);

// Offsets in UTF-16 units of `text`, ascending, as code points, which is
// what GtkTextIter counts: one pass over the text. An offset inside a
// surrogate pair counts as the pair's start, one past the end as the end.
std::vector<std::size_t> codePoints(const std::u16string& text,
                                    const std::vector<std::size_t>& sortedUnits);

// The other way, for one offset: code point `point` of `text` in UTF-16
// units (the end for one past it).
std::size_t unitsAt(const std::u16string& text, std::size_t point);

}  // namespace EditSpans
