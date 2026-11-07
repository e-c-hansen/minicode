// Regex.h — a regular expression engine whose time is linear in the text,
// the core behind TextFind's regex search (the editor's find bar, Find in
// Folder and Replace in Folder). Pure C++17, no I/O.
//
// It is a Pike VM: the pattern compiles to a small program, and a search
// runs every way the program can go at once, one character at a time, so a
// search costs the text's length times the program's size, whatever the
// pattern. A pattern such as (a+)+b, which makes a backtracking engine try
// every way of splitting the line, costs no more than a+b. std::regex
// backtracks, and a single attempt of such a pattern could not be stopped.
//
// What it accepts is JavaScript's syntax (ECMAScript, without the u flag),
// what VS Code's find uses, less what needs backtracking:
//
// - literals and escapes (\t \n \v \f \r \0 \cX \xHH \uHHHH, and \ before
//   any character that is not a letter or digit), . (anything but a line
//   break), classes ([abc], [^a-z], with \d \w \s \D \W \S inside),
//   \d \w \s \D \W \S, ^ $ \b \B, groups (capturing and (?:...)),
//   alternation, and * + ? {m} {m,} {m,n} (at most 1,000), greedy or lazy.
// - \d, \w and \b are ASCII, as in JavaScript; \s is JavaScript's set of
//   spaces; . leaves out \n, \r, U+2028 and U+2029.
// - Matches are JavaScript's, leftmost first, with its two rules for
//   repetition: each repetition starts with the groups inside it unset, so
//   (?:(a)|b)+ on "ab" leaves group 1 unset; and a repetition past the
//   minimum that matched nothing does not count, so (a*)* on "b" leaves
//   group 1 unset and (?:|a)* on "aa" matches both letters. (libc++ and
//   libstdc++ each follow these in some cases and not others.)
//
// compile() refuses the rest (backreferences, lookahead and lookbehind,
// named groups, [[:alpha:]] and the other C++ additions, a lone ] or }, a
// program over 50,000 steps), and TextFind runs those through std::regex
// instead.
//
// Case-insensitive matching folds with TextFind::foldCase: the pattern is
// folded when compiled, and the caller folds the text (one code point for
// one, so positions hold).
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Regex {

class Program;

// The compiled `pattern`, or null when it uses something this engine does
// not do (*why, if given, says what). A pattern compile() accepts is also
// valid JavaScript; one it refuses may still be (std::regex decides).
std::shared_ptr<const Program> compile(const std::u32string& pattern, bool ignoreCase,
                                       std::string* why = nullptr);

// The number of capturing groups.
std::size_t groupCount(const Program& program);

// The number of steps the program has (for tests).
std::size_t programSize(const Program& program);

enum class Result { Found, NotFound, Stopped };

struct SearchOptions {
    // Only a match that starts at `from`.
    bool anchored = false;
    // Only a match of at least one character.
    bool notEmpty = false;
};

constexpr std::size_t npos = static_cast<std::size_t>(-1);

// The leftmost match in s[0, n) that starts at or after `from`, as
// JavaScript's exec finds it from lastIndex = from: ^ matches only at 0, $
// only at n, and \b sees the character before `from`. On Found, `caps`
// holds a start and an end per group, group 0 (the whole match) first, npos
// for a group that took no part. Stopped when `cancel` went up (it is
// polled every few thousand characters).
Result search(const Program& program, const char32_t* s, std::size_t n, std::size_t from,
              const SearchOptions& options, std::vector<std::size_t>& caps,
              const std::atomic<bool>* cancel = nullptr);

}  // namespace Regex
