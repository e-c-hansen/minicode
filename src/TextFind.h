// TextFind.h — find and replace with VS Code's three options (match case,
// whole word, regular expression), the pure C++17 core behind the editor's
// find bar (UTF-16, as NSString holds text) and Find in Folder's search and
// Replace in Folder (UTF-8, as files hold it). No GUI, no I/O.
//
// What it does, so the two front ends agree:
//
// - Text is searched a line at a time. Lines end at \n, \r\n, \r and U+2029
//   (FolderSearch's line ends, so line numbers agree), and a match never
//   spans a line break; a pattern can only match
//   within one line (a regex holding \n matches nothing). ^ and $ are the
//   start and end of a line.
// - Plain text: case-insensitive by default, with FolderSearch's one-to-one
//   folding (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic).
// - Regular expressions are std::regex's ECMAScript grammar (JavaScript's,
//   which is what VS Code's find uses), run on UTF-32 code points, so . is
//   one character and a character class can hold any letter. Case-insensitive
//   matching folds with the same table as plain text. \w, \b and \d are
//   ASCII only, as in JavaScript. An empty match is a match (^ finds every
//   line's start), and the next search starts a character on.
// - Whole word, as VS Code checks it: a match's first character must not
//   have a word character before it, unless that first character is not a
//   word character itself; the same at its end. Word characters are ASCII
//   letters and digits, _ and anything past ASCII that is not a space, as
//   for Cmd+D (MultiCaret).
// - Replacement text is literal for plain text. For a regex, $1 to $99 are
//   the groups (an unmatched one is empty), $0 and $& the whole match, $$ a
//   dollar sign, and \n, \t and \\ a newline, a tab and a backslash, as in
//   VS Code. Any other $ or \ is itself.
//
// A known limit of std::regex: it backtracks, so a pattern such as (a|b)*c
// against a very long line takes time quadratic in the line's length. The
// front ends search off the main thread and poll a cancel flag between
// lines.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TextFind {

struct Options {
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
};

// One match, in the code units of the text it was found in (UTF-16 units for
// a u16string, bytes for UTF-8). groups[0] is the whole match; a group that
// took no part has start == npos.
struct Match {
    std::size_t start = 0;
    std::size_t length = 0;
    std::vector<std::pair<std::size_t, std::size_t>> groups;   // start, length
};

// One replacement: `length` units at `start` become `text`.
struct Edit16 {
    std::size_t start = 0;
    std::size_t length = 0;
    std::u16string text;
};

// A compiled query. Cheap to copy (the compiled pattern is shared).
class Query {
public:
    Query() = default;
    // The pattern as typed, in UTF-8 or UTF-16.
    static Query make(const std::string& pattern, const Options& options);
    static Query make(const std::u16string& pattern, const Options& options);

    // False for an empty pattern or a regex that does not compile.
    bool usable() const { return impl_ != nullptr; }
    // Why a regex did not compile ("Invalid regular expression: ..."), else "".
    const std::string& error() const { return error_; }
    const Options& options() const { return options_; }

    struct Impl;
    const Impl* impl() const { return impl_.get(); }

private:
    std::shared_ptr<const Impl> impl_;
    std::string error_;
    Options options_;
};

// Case folding used for case-insensitive matching (one character to one).
char32_t foldCase(char32_t c);

// A word character for whole-word matching.
bool isWordChar(char32_t c);

// ------------------------------------------------------------ UTF-16 (editor)

// Every match in `text`, in order, at most `limit` (0 = no limit). Sets
// *truncated when it stopped at the limit. `cancel`, if given, is polled
// between lines; a raised flag ends the search early with *cancelled set.
std::vector<Match> findAll(const std::u16string& text, const Query& query,
                           std::size_t limit = 0, bool* truncated = nullptr,
                           const std::atomic<bool>* cancel = nullptr,
                           bool* cancelled = nullptr);

// The replacement text for one match of `query` in `text`.
std::u16string expand(const std::u16string& replacement, const std::u16string& text,
                      const Match& match, const Query& query);

// The edits Replace All makes: every match replaced, in ascending order.
std::vector<Edit16> replaceAll(const std::u16string& text, const Query& query,
                               const std::u16string& replacement);

// `text` with the edits (ascending, not overlapping) applied.
std::u16string applyEdits(const std::u16string& text, const std::vector<Edit16>& edits);

// Which match Find Next (forward) or Find Previous goes to from a selection:
// forward, the first starting at or after `selEnd` (after `selStart` when
// the selection is itself a match, so an empty match does not stick);
// backward, the last ending at or before `selStart` and starting before it.
// Wraps around. -1 when there are none.
long nextIndex(const std::vector<Match>& matches, std::size_t selStart,
               std::size_t selEnd, bool forward);

// The index of the match exactly covering [start, start + length), or -1.
long indexOf(const std::vector<Match>& matches, std::size_t start, std::size_t length);

// ----------------------------------------------------- UTF-8 (Find in Folder)

// The first match in one line (no line breaks; valid UTF-8): byte offset and
// length. False when there is none.
bool findInLine(const std::string& line, const Query& query, std::size_t* byteColumn,
                std::size_t* byteLength);

// Every match in one line, in bytes.
std::vector<Match> findAllInLine(const std::string& line, const Query& query);

// `content` (valid UTF-8) with every match replaced. Line endings are kept
// as they were. *count is the number of matches replaced.
std::string replaceInText(const std::string& content, const Query& query,
                          const std::string& replacement, std::size_t* count);

// `content` with only the match that starts at `byteColumn` of 1-based line
// `line` replaced (the line as FolderSearch numbers lines). False, leaving
// *out alone, when no match starts there now.
bool replaceOneInText(const std::string& content, const Query& query,
                      const std::string& replacement, int line, std::size_t byteColumn,
                      std::string* out);

}  // namespace TextFind
