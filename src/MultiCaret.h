// MultiCaret.h — editing at several places at once: the carets a column
// selection (Cmd+Shift+drag, Option+drag) or Option+click leaves, and what
// typing, deleting, pasting and the arrow keys do to all of them.
//
// Pure C++, no I/O, in UTF-16 code units like LineComments and AutoIndent,
// so offsets map straight onto NSRange. Where the carets are on screen
// (which character a column falls on, the line above) is the GUI's job;
// everything here works on the text alone.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace MultiCaret {

// A caret, or a selection when anchor != head. head is where the caret is
// drawn and where the arrow keys move it from.
struct Caret {
    size_t anchor = 0, head = 0;
    size_t lo() const { return anchor < head ? anchor : head; }
    size_t hi() const { return anchor < head ? head : anchor; }
    bool empty() const { return anchor == head; }
    bool operator==(const Caret& o) const { return anchor == o.anchor && head == o.head; }
};

// One replacement, in the text as it was before the edit.
struct Change {
    size_t start = 0, length = 0;
    std::u16string text;
};

// An edit: changes in ascending order, never overlapping (apply them last to
// first), and the carets afterwards, in the new text.
struct Edit {
    std::vector<Change> changes;
    std::vector<Caret> carets;
};

// Sorted by position, clamped to the text, with overlapping selections and
// equal carets merged. The order of the result is the text's order.
std::vector<Caret> normalize(std::vector<Caret> carets, size_t textLength);

// Each caret's selection replaced by text: texts has one entry for all of
// them, or one per caret (in normalize's order). The carets end up after
// what they inserted.
Edit insert(const std::vector<Caret>& carets, const std::vector<std::u16string>& texts);

// Backspace: a selection goes; a caret deletes the character before it (a
// surrogate pair whole), but never across the start of its line, so a
// column of carets at the left edge stays a column.
Edit backspace(const std::u16string& text, const std::vector<Caret>& carets);

// Forward delete, the same way, never across the end of the line.
Edit deleteForward(const std::u16string& text, const std::vector<Caret>& carets);

enum class Move { Left, Right, WordLeft, WordRight, LineStart, LineEnd };

// Every caret moved. Without extend a selection collapses: Left and Right
// to its edge on that side, the others from its head. LineStart goes to the
// first non-blank character, or to column 0 from there (as Home does in
// most code editors). Moves never cross a line break, except Left at a
// line's start and Right at its end.
std::vector<Caret> move(const std::u16string& text, const std::vector<Caret>& carets, Move m,
                        bool extend);

// What Paste puts at each caret: when the clipboard holds exactly as many
// lines as there are carets (a final newline ignored), a line each, as VS
// Code and Sublime do; otherwise the whole text at every caret.
std::vector<std::u16string> pieces(const std::u16string& clip, size_t carets);

// What Copy takes: the selections in order, joined with newlines. Empty
// when every caret is empty.
std::u16string copy(const std::u16string& text, const std::vector<Caret>& carets);

// The character that closes `c` when it is typed over a selection: ( [ {
// and the quotes " ' `. 0 for anything else.
char16_t closerFor(char16_t c);

// Typing an opener over selections: each selection gets the opener before
// it and its closer after it, and stays selected (in its direction), so
// pressing another opener wraps again. Empty carets are left alone; the
// editor only calls this when every caret has a selection.
Edit surround(const std::vector<Caret>& carets, char16_t opener);

// Copy Line Down / Up: the whole lines each caret touches (a selection
// ending at the start of a line leaves that line out) are inserted again
// below or above themselves, and the carets go with the copy below, or stay
// on the copy above. Carets sharing a line make one block. Line breaks
// copy the file's own ("\r\n" where the block ends in one).
Edit duplicateLines(const std::u16string& text, const std::vector<Caret>& carets, bool down);

// Move Line Down / Up: the whole lines each caret touches swap with the
// line below (or above), carets and selections going with them. Carets on
// neighbouring lines move as one block. Nothing moves if any block is
// already at the end (or start) of the text. Every change keeps its
// length, so the carets of one block never shift another's.
Edit moveLines(const std::u16string& text, const std::vector<Caret>& carets, bool down);

// Applies an edit to a string (the tests use it; the GUI goes through the
// text view so undo sees the change).
std::u16string applyEdit(const std::u16string& text, const Edit& e);

}  // namespace MultiCaret
