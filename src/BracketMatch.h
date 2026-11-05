// BracketMatch.h — pure C++. Bracket matching for the editor: the bracket
// next to the caret and its partner, highlighted together, and where Go to
// Bracket (Shift+Cmd+\) moves the caret. ( ) [ ] { } only.
//
// Brackets inside strings and comments are not brackets: the caller passes
// those stretches of the text (from the highlighter's tokens) as `skip`, and
// a bracket in one neither matches nor counts. Each kind of bracket is
// counted on its own, so an unbalanced one of another kind between two
// partners does not keep them apart. Every search gives up after `maxScan`
// code units, so a caret beside a bracket in a huge file never stalls the
// editor; a partner further away than that is not found.
//
// Offsets are UTF-16 units in the text given, which may be a window cut out
// of a larger document (the caller adds its start back).
#pragma once
#include <cstddef>
#include <vector>

namespace Brackets {

constexpr size_t npos = static_cast<size_t>(-1);
constexpr size_t kDefaultScan = 20000;

// [start, end): text that is not code, such as a string or a comment.
// Sorted by start, not overlapping.
struct Span {
    size_t start, end;
};

struct Pair {
    size_t open = npos, close = npos;
    bool found() const { return open != npos && close != npos; }
};

bool isBracket(char16_t c);
bool isOpener(char16_t c);

// Whether offset `at` is inside one of the spans.
bool skipped(const std::vector<Span>& skip, size_t at);

// The bracket the caret touches: the character before it, else the one
// after it, when that is a bracket in code. npos when neither is.
size_t touching(const char16_t* text, size_t length, size_t caret,
                const std::vector<Span>& skip);

// The partner of the bracket at `at`: forward for an opener, backward for a
// closer. npos when unmatched within maxScan units.
size_t partner(const char16_t* text, size_t length, size_t at,
               const std::vector<Span>& skip, size_t maxScan = kDefaultScan);

// The pair to highlight for a caret: the bracket it touches and its partner
// (both npos when there is no bracket, or no partner).
Pair match(const char16_t* text, size_t length, size_t caret,
           const std::vector<Span>& skip, size_t maxScan = kDefaultScan);

// The innermost pair around the caret that the caret is not touching from
// outside: the nearest unmatched opener before it and that opener's partner.
Pair enclosing(const char16_t* text, size_t length, size_t caret,
               const std::vector<Span>& skip, size_t maxScan = kDefaultScan);

// Where Go to Bracket puts the caret, as VS Code's editor.action.jumpToBracket
// does: touching a bracket, before its partner (from an opener to just before
// its closer, from a closer to just before its opener); inside a pair, before
// its closer; else before the next bracket. npos when there is none.
size_t jumpTarget(const char16_t* text, size_t length, size_t caret,
                  const std::vector<Span>& skip, size_t maxScan = kDefaultScan);

}  // namespace Brackets
