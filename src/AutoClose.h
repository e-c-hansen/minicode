// AutoClose.h — pure C++. Brackets and quotes that close themselves as they
// are typed, by VS Code's rules at its defaults: editor.autoClosingBrackets,
// autoClosingQuotes and autoClosingComments "languageDefined",
// autoClosingOvertype and autoClosingDelete "auto", and autoSurround
// "languageDefined" (cursorTypeEditOperations.ts, cursorDeleteOperations.ts
// and cursor.ts's AutoClosedAction, read for this).
//
// - Typing an opener inserts its closer after the caret when the character
//   after the caret is a space, the end of the line, or one of the
//   language's "auto-close before" characters (VS Code's ;:.,=}])> and, for
//   brackets, quotes), or a closer; never for a bracket that has an
//   unmatched closer after it already; never for a quote typed after a word
//   character (so "don't" types plainly) unless the pair's opener is longer
//   (Python's f" and the like); never where the pair says it does not
//   belong: a quote inside a string or comment, by the highlighter's
//   tokens and by what a neutral character typed there would become.
// - Typing a closer that this inserted, just before it, steps over it. An
//   inserted closer is remembered while every caret stays inside its pair
//   on one line (VS Code's "auto" overtype).
// - Backspace between such an empty pair deletes both.
// - Typing a surrounding character over selections wraps each of them.
//
// Each language's pairs are VS Code's language-configuration.json, mapped
// by file name. Two departures from them: Python's third quote of
// """ (or ''') closes the triple quote, where VS Code's configuration gives
// """ and a fourth quote; in LaTeX only brackets pair (VS Code has no LaTeX
// of its own; LaTeX Workshop's `...' and \left( pairs are left out).
//
// Everything works at every caret at once, all or nothing, as in VS Code: a
// pair closes only if it closes at every caret. Offsets are UTF-16 units,
// as MultiCaret's.
#pragma once
#include "MultiCaret.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

template <class Ch> class IncrementalHighlighter;
template <class Ch> class TextSource;

namespace AutoClose {

using MultiCaret::Caret;
using MultiCaret::Edit;
using MultiCaret::Text;

// What a character is part of, as the highlighter sees it.
enum class Scope : uint8_t { Code, String, Comment };

// A stretch of a string or a comment, [start, end) in the whole text.
struct Span {
    size_t start, end;
    Scope scope;
};

// What the editor knows of strings and comments in its text. The default
// knows nothing: everything is code (a file without a grammar).
class Context {
public:
    virtual ~Context() = default;
    // The scope of the character at i.
    virtual Scope at(size_t i) const { (void)i; return Scope::Code; }
    // The scope character c would be in if it were typed at i.
    virtual Scope ifTyped(size_t i, char16_t c) const { (void)i; (void)c; return Scope::Code; }
    // Whether the line holding i is lexed at all (VS Code's
    // isCheapToTokenize): a pair never closes on a line too long to lex.
    virtual bool lexed(size_t i) const { (void)i; return true; }
    // The strings and comments meeting [lo, hi), in order.
    virtual std::vector<Span> nonCode(size_t lo, size_t hi) const {
        (void)lo; (void)hi; return {};
    }
};

// A Context over the editor's incremental highlighter and its text (the
// highlighter must be up to date with the text). The source is the text.
class HighlighterContext : public Context {
public:
    HighlighterContext(IncrementalHighlighter<char16_t>& hl,
                       std::unique_ptr<TextSource<char16_t>> source);
    ~HighlighterContext() override;
    Scope at(size_t i) const override;
    Scope ifTyped(size_t i, char16_t c) const override;
    bool lexed(size_t i) const override;
    std::vector<Span> nonCode(size_t lo, size_t hi) const override;

private:
    IncrementalHighlighter<char16_t>& hl_;
    std::unique_ptr<TextSource<char16_t>> src_;
    // The last line's tokens (the carets of a column ask line after line).
    mutable size_t line_ = size_t(-1);
    mutable std::vector<std::pair<size_t, size_t>> tokens_;   // start, end
    mutable std::vector<Scope> scopes_;
};

// One auto-closing pair of a language: `open` (one or more characters,
// ending in the one typed) and what goes in after the caret.
struct Pair {
    std::u16string open, close;
    bool notInString = false, notInComment = false;
    // The string-or-comment check reads the character before the whole
    // opener rather than the one just before the caret (Python's triple
    // quote, whose first two quotes are a string of their own).
    bool scopeBeforeOpen = false;
};

struct Language {
    std::string id;                        // "python", "markdown", ...
    std::vector<Pair> pairs;               // autoClosingPairs
    std::vector<std::pair<char16_t, char16_t>> surround;   // surroundingPairs
    std::u16string closeBeforeQuotes;      // autoCloseBefore, for quotes
    std::u16string closeBeforeBrackets;    // and for brackets and comments
    std::u16string blockCommentStart;      // a pair holding it is a comment's
};

// The language of a file, by its name (a path is fine): VS Code's language
// for it, plain text for a name it does not know.
const Language& languageFor(const std::string& path);

// The closers auto-closing inserted that may still be typed over or deleted
// with their opener, with the pairs they close. Each keystroke that closed
// pairs is one action, as in VS Code; an action is dropped as soon as a
// caret is outside its pair, or a pair spans lines. Offsets follow edits
// (edited) the way VS Code's decorations that never grow at their edges do.
class Tracker {
public:
    // A closer at [closeStart, closeEnd) of the pair [pairStart, closeEnd),
    // one per caret, in the text after the keystroke.
    struct Closed {
        size_t pairStart, closeStart, closeEnd;
    };
    void add(const std::vector<Closed>& closed);
    // [pos, pos + oldLength) became newLength units.
    void edited(size_t pos, size_t oldLength, size_t newLength);
    // Drops the actions the carets (in normalize's order) have left.
    void validate(const Text& text, const std::vector<Caret>& carets);
    // A closer this inserted starts at `at`.
    bool closerAt(size_t at) const;
    void clear() { actions_.clear(); }
    bool empty() const { return actions_.empty(); }
    size_t actions() const { return actions_.size(); }

private:
    struct Range {
        size_t start, end;
    };
    struct Action {
        std::vector<Range> closers, pairs;
    };
    std::vector<Action> actions_;
};

// What typing one character did.
struct Typed {
    enum class Kind { None, Overtype, Close, Surround };
    Kind kind = Kind::None;
    // The edit to apply as one undo step (None: nothing; type as usual).
    // Close puts the typed character and its closer in as two changes at
    // the same place, the typed one first, so an editor applying them last
    // to first sees the typed character go in last, by itself, as a
    // keystroke (a language server's trigger character). Overtype changes
    // nothing and moves the carets.
    Edit edit;
    // For Close: what to give the Tracker once the edit is in.
    std::vector<Tracker::Closed> closed;
};

// Typing ch at the carets (normalized), in VS Code's order: over an
// inserted closer, then an opener that closes, then a surround of the
// selections. `closing` off (editor.auto-close = off) leaves only the
// surround.
Typed type(const Text& text, const std::vector<Caret>& carets, char16_t ch,
           const Language& lang, const Context& context, const Tracker& tracker,
           bool closing = true);

// Backspace at every caret between an empty pair whose closer the Tracker
// holds: both characters go. Empty when that is not so at every caret.
Edit deletePair(const Text& text, const std::vector<Caret>& carets, const Language& lang,
                const Tracker& tracker);

// Characters a pair's balance check looks at around each caret.
constexpr size_t kBalanceReach = 20000;

}  // namespace AutoClose
