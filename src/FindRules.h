// FindRules.h — what the editor's find and replace bar decides, apart from
// how it is drawn: what Ctrl+F puts in the query, the count beside it, the
// search with Find in Selection's scope, the changes Replace and Replace
// All make, and the colors of each theme. The GTK port's bar
// (linux/src/FindBar.cpp) follows it. The Mac's MCEditorFind
// (src/EditorFind.mm), which came first, keeps the same rules in its own
// code; nothing here changes it. Pure C++17 over TextFind.h, no I/O.
//
// Positions: the search runs on UTF-16, as TextFind does, and every match
// comes back twice, in UTF-16 units (for TextFind::expand, which needs the
// groups) and in code points, which is what a GtkTextIter counts. Changes
// are in code points with their new text in UTF-8, ready for
// MultiEdit::apply (linux/src/MultiEdit.h).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "TextFind.h"

namespace FindRules {

// ------------------------------------------------------------ the query

// VS Code's escapeRegExpCharacters: \ { } * + ? | ^ $ . [ ] ( ) each get a
// backslash, so the text is found as it is while Regular Expression is on.
std::u16string escapeRegex(const std::u16string& text);

// The longest selection that seeds the query (VS Code's limit).
constexpr std::size_t kMaxSeed = 524288;

// What Ctrl+F (the Mac's Cmd+F) puts in the query, as VS Code's
// editor.find.seedSearchStringFromSelection does by default ("always"): a
// selection within one line becomes the query; an empty selection gives
// the word at the caret (MultiCaret::wordAt, VS Code's word separators);
// a selection over several lines, or none and no word, gives nothing, and
// the query stays as it was. Escaped while Regular Expression is on.
// `text` may be only the line holding the selection's start: a word never
// crosses a line, and a selection that does seeds nothing.
struct Seed {
    bool found = false;
    std::u16string query;
    bool word = false;                 // the word at the caret, not a selection
    std::size_t start = 0, end = 0;    // where it was taken from, in `text`
};
Seed seed(const std::u16string& text, std::size_t selStart, std::size_t selEnd, bool regex);

// ------------------------------------------------------------ the count

// The most matches found and highlighted; past it the count says "20000+".
constexpr std::size_t kMatchLimit = 20000;

struct CountState {
    bool hasQuery = false;
    std::string queryError;     // a regex that does not compile: why
    bool searched = false;      // there are results for this query and options...
    bool current = false;       // ... and for the text as it is now
    std::size_t matches = 0;
    bool truncated = false;     // stopped at kMatchLimit
    long currentIndex = -1;     // the match the selection is, -1 for none
    bool tooSlow = false;       // TextFind::Query::tooSlow
};

struct Count {
    bool keep = false;          // leave the count as it is: a search of the
                                // edited text is on its way
    std::string text;           // "3 of 17", "? of 20000+", "No results", "…"
    bool error = false;         // in the error color, `tip` its tooltip
    bool noResults = false;     // nothing found: the query outlined in red
    bool navigable = false;     // Previous and Next can act
    std::string tip;
};

// The Mac's showCount, rule for rule.
Count count(const CountState& state);

// ------------------------------------------------------------ searching

// Find in Selection's scope from a selection [start, end) of `text`: as VS
// Code has it, a selection of more than one unit that ends just after a
// line break ends before it. *scopeEnd == start means no scope (an empty
// selection turns nothing on).
std::size_t scopeEnd(const std::u16string& text, std::size_t start, std::size_t end);
// The same from the selection's last character alone (any units).
std::size_t scopeEnd(std::size_t start, std::size_t end, char32_t last);

// Every match of `query` in `text`, at most kMatchLimit, in UTF-16 units
// and in code points (pairs, in step). With a scope (code points,
// scopeEnd > scopeStart) only the matches wholly inside it count. Polls
// `cancel` between lines (TextFind::findAll).
struct Found {
    std::vector<TextFind::Match> units;
    std::vector<TextFind::Match> points;   // the same, in code points (no groups)
    bool truncated = false;
    bool cancelled = false;
};
Found findAll(const std::u16string& text, const TextFind::Query& query,
              std::size_t scopeStart = 0, std::size_t scopeEnd = 0,
              const std::atomic<bool>* cancel = nullptr);

// ------------------------------------------------------------ replacing

// One change: `length` code points at `start` become `text` (UTF-8),
// `newLength` code points long.
struct Change {
    std::size_t start = 0;
    std::size_t length = 0;
    std::string text;
    std::size_t newLength = 0;
};

// Replace All: every match (inside the scope, when there is one, in code
// points as for findAll) replaced, in ascending order. $1 and the rest
// for a regex, and the case of what it replaces with Preserve Case
// (TextFind::replaceAll).
std::vector<Change> replaceAll(const std::u16string& text, const TextFind::Query& query,
                               const std::u16string& replacement, std::size_t scopeStart = 0,
                               std::size_t scopeEnd = 0);

// Replace: match `index` of `found` (from findAll over this very `text`)
// replaced. False when there is no such match.
bool replaceOne(const std::u16string& text, const Found& found, std::size_t index,
                const TextFind::Query& query, const std::u16string& replacement, Change* out);

// ------------------------------------------------------------ colors

// One theme's find widget colors, as its VS Code theme file sets them (or
// VS Code's defaults where it sets none), RRGGBBAA; 0 means none (no
// border). The Mac's FindWidget.mm has the same table (kPalettes), which a
// test holds this one to.
struct Palette {
    const char* theme;
    std::uint32_t widgetBg, widgetBorder, widgetFg, shadow;
    std::uint32_t inputBg, inputBorder, inputFg, placeholder, focus;
    std::uint32_t optBg, optBorder, optFg;
    std::uint32_t errorFg, badgeBg, badgeFg, sideBarBg, selection;
};
const std::vector<Palette>& palettes();
// The theme's, else Dark+'s or Light+'s.
const Palette& palette(const std::string& theme, bool dark);

// The editor's highlights, VS Code's: every match (findMatchHighlightBackground),
// the selected one (findMatchBackground), and Find in Selection's scope
// (findRangeHighlightBackground), RRGGBBAA.
std::uint32_t matchHighlight(bool dark);
std::uint32_t currentMatch(bool dark);
std::uint32_t scopeHighlight(bool dark);

}  // namespace FindRules
