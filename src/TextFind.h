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
// - Regular expressions are the ECMAScript grammar (JavaScript's, which is
//   what VS Code's find uses), run on UTF-32 code points, so . is one
//   character and a character class can hold any letter. std::regex decides
//   what is valid. Case-insensitive matching folds with the same table as
//   plain text. \w, \b and \d are ASCII only, as in JavaScript. An empty
//   match is a match (^ finds every line's start), and the next search
//   starts a character on.
// - Most patterns run on Regex.h's engine, whose time is linear in the line
//   whatever the pattern, with JavaScript's matches. The few it refuses
//   (backreferences, lookaround, C++'s [[:alpha:]] and the like) run through
//   std::regex on a thread of the query's own; a search that takes over 2 s
//   there is left behind to finish by itself, finds nothing, and so does
//   every later search with that query (Query::tooSlow says so).
// - Whole word, as VS Code checks it: a match's first character must not
//   have a word character before it, unless that first character is not a
//   word character itself; the same at its end. Word characters are ASCII
//   letters and digits, _ and anything past ASCII that is not a space, as
//   for Cmd+D (MultiCaret).
// - Replacement text is literal for plain text. For a regex, $1 to $99 are
//   the groups (an unmatched one is empty), $0 and $& the whole match, $$ a
//   dollar sign, and \n, \t and \\ a newline, a tab and a backslash, as in
//   VS Code. Any other $ or \ is itself.
// - Preserve case (Options::preserveCase, VS Code's AB toggle by the
//   replacement): a replacement with no groups in it takes the case of the
//   text it replaces, by VS Code's buildReplaceStringWithCasePreserved:
//   all capitals, all small, a capital first or a small first; and part by
//   part when both hold the same number of "-" (or "_") separated parts.
//
// The front ends search off the main thread and poll a cancel flag between
// lines; the linear engine polls it within a long line too. std::regex
// backtracks, so for the patterns left to it a line over 2,000 characters
// is searched one start position at a time, polling the flag, and given up
// after a second or so (a minified file kept the find queue busy for many
// seconds).
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
    bool preserveCase = false;   // replacing only; matching ignores it
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

    // The same through std::regex alone (for tests, as the oracle).
    static Query makeBacktracking(const std::u16string& pattern, const Options& options);

    // False for an empty pattern or a regex that does not compile.
    bool usable() const { return impl_ != nullptr; }
    // A regex the linear engine runs (Regex.h), not std::regex.
    bool linear() const;
    // A search through std::regex took too long and was left behind: it,
    // and every later search with this query, found nothing.
    bool tooSlow() const;
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

// Characters every match of `query` holds, one after another, folded as
// foldCase folds them unless the query matches case: the plain text itself
// (whole word too), or the literal run a regex on the linear engine
// requires. "" when there is none: a regex left to std::regex, or one such
// as a* or \d+. Find in Folder looks for them in a file first and matches
// only the lines that hold them.
std::u32string requiredText(const Query& query);

// Case folding used for case-insensitive matching (one character to one).
// It changes nothing at or past U+0500 (Regex.cpp relies on that).
char32_t foldCase(char32_t c);

// How long a search through std::regex may take before it is left behind
// (2 s; tests shorten it).
void setSlowPatternSeconds(double seconds);

// How far past a start position std::regex reads on a long line, which is
// the longest match it finds there (8,192 code points; tests shorten it).
// Returns the old value.
std::size_t setRegexReach(std::size_t codePoints);

// A word character for whole-word matching.
bool isWordChar(char32_t c);

// The capital of a small letter (the inverse of foldCase's table), else c.
char32_t upperCase(char32_t c);

// `replacement` in the case of `matched`, as preserve case makes it.
std::u16string preserveCase(const std::u16string& matched, const std::u16string& replacement);
std::string preserveCase(const std::string& matched, const std::string& replacement);

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

// The edit replacing only the match that starts at `byteColumn` (UTF-8
// bytes) of 1-based `line`: a Find in Folder row, applied to the text of a
// tab that holds the file. False when no match starts there.
bool replaceOneAt(const std::u16string& text, const Query& query,
                  const std::u16string& replacement, int line, std::size_t byteColumn,
                  Edit16* out);

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
