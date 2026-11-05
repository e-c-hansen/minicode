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

// ---- whole lines (Cmd+L, Ctrl+Shift+K, and Copy, Cut and Paste with
// nothing selected, as in VS Code)

// Expand Line Selection (Cmd+L): each caret selects its whole line, line
// break included, from the line's start (the anchor) to the next line's
// start (the head). A selection already ending at the start of a line (as
// the last press left it) takes the next line too, so pressing again goes
// on down. Selections that come to overlap merge.
std::vector<Caret> expandLineSelection(const std::u16string& text,
                                       const std::vector<Caret>& carets);

// Delete Line (Ctrl+Shift+K): the whole lines each caret touches go, with
// their line break. Each caret lands on the line that took their place at
// the column it had (or that line's end); lines at the end of the text take
// the break before them instead, and the caret goes to the line above.
// Carets on the same or neighbouring lines make one block, with one caret.
Edit deleteLines(const std::u16string& text, const std::vector<Caret>& carets);

// What Copy and Cut take when nothing is selected: the lines the carets are
// on, each once, in order, each with its line break ("\n" added after a
// last line that has none).
std::u16string lineCopy(const std::u16string& text, const std::vector<Caret>& carets);

// Paste of such a copy with nothing selected: the text goes in at the start
// of each caret's line, so above it, and the carets stay on their
// characters. When the copy has one line per line holding a caret, each of
// those lines gets one, in order; otherwise each gets all of it.
Edit pasteLinesAbove(const std::u16string& text, const std::vector<Caret>& carets,
                     const std::u16string& clip);

// ---- the selected text, found again (Cmd+D, Shift+Cmd+L, as in VS Code)

// The word at p: the run of word characters (letters, digits, _ and anything
// past ASCII) holding p or touching it on either side, selected from its
// start to its end. An empty caret at p when no word is there.
Caret wordAt(const std::u16string& text, size_t p);

// Every place needle occurs, in order, case-sensitive and never overlapping
// (each searched for after the previous one's end), selected from start to
// end. With wholeWord, only places with no word character running on into
// them on either side.
std::vector<Caret> occurrences(const std::u16string& text, const std::u16string& needle,
                               bool wholeWord);

struct Found {
    std::vector<Caret> carets;   // in normalize's order
    Caret added;                 // the selection this press made (the next one goes on from it)
    bool wholeWord = false;      // what the next press should search with
    bool changed = false;        // false: nothing (more) to select
};

// Add Selection To Next Find Match (Cmd+D). When any caret is empty, each
// empty caret selects the word at it and later presses match whole words
// only (wholeWord comes back true); that is all this press does. Otherwise
// the text of `last` (the selection the previous press added) is looked
// for after it, wrapping round to the start, skipping places already
// selected, and the first one found is added.
Found addNextOccurrence(const std::u16string& text, const std::vector<Caret>& carets,
                        Caret last, bool wholeWord);

// Select All Occurrences (Shift+Cmd+L): a selection at every place the text
// of `last` occurs, or, when `last` is empty, every whole-word occurrence of
// the word at it.
Found selectAllOccurrences(const std::u16string& text, Caret last, bool wholeWord);

// Applies an edit to a string (the tests use it; the GUI goes through the
// text view so undo sees the change).
std::u16string applyEdit(const std::u16string& text, const Edit& e);

}  // namespace MultiCaret
