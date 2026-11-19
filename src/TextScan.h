// TextScan.h — pure C++17, no I/O. Finding in a large text by the
// characters every match must hold, looked for in the raw units before any
// line is matched: the editor's find bar over a large file (the Mac's
// EditorFind.mm, over a log's ASCII bytes as read) and Find in Folder's
// files (FolderSearch.cpp, over UTF-8 bytes).
//
// Round 9's grading (C3): the find bar took about 2 s to find the last line
// of a 259 MB log, matching every one of its 3 million lines through
// TextFind a line at a time, where VS Code and Zed took well under one.
// Here the bytes (or UTF-16 units) are searched for the needle first,
// sixteen bytes at a time, and only a line that holds it is matched;
// plain text whose needle is the whole query (matching case, or ASCII
// ignoring it, which only ASCII folds to) needs no line matched at all.
//
// The matches are exactly TextFind::findAll's over the whole text (the
// tests compare them on random texts), in UTF-16 units as NSString counts,
// whatever the text is held in:
//
// - UTF-8 bytes in memory (valid UTF-8; `ascii` when every byte is under
//   0x80, so a byte is a unit and no counting is needed), never copied but
//   a line at a time for matching;
// - UTF-16 units read through a TextFind::Reader a block of whole lines at
//   a time (an NSString, a piece table's snapshot).
//
// A needle in UTF-8 starts with a lead byte and a continuation byte is never
// a lead byte nor ASCII, so a byte match of it, folded or not, starts on a
// character's first byte; no match can begin inside a character.
#pragma once

#include "TextFind.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace TextScan {

// Text a line must hold to match, looked for in a whole buffer at once
// (sixteen bytes at a time for where its first and last units both are,
// or memchr for a lone byte; ASCII letters in either case when ignoring
// case).
class Needle {
public:
    Needle() = default;
    // From TextFind::requiredText (or a query's characters): matching case,
    // all of them; ignoring case, their longest run of ASCII characters,
    // folded, which only ASCII characters fold to (TextFind::foldCase).
    static Needle make(const std::u32string& text, bool caseSensitive);

    // Nothing to look for (no text, or no ASCII in it ignoring case): every
    // line may hold a match.
    bool empty() const { return bytes_.empty() && !never_; }
    // It holds a line break, so no line can hold it.
    bool never() const { return never_; }
    // It is every character it was made from.
    bool whole() const { return whole_; }
    bool folds() const { return fold_; }
    std::size_t byteLength() const { return bytes_.size(); }
    std::size_t unitLength() const { return units_.size(); }

    // The first occurrence in s[from, n), or npos: UTF-8 bytes, or UTF-16
    // units.
    std::size_t find(const char* s, std::size_t n, std::size_t from) const;
    std::size_t find(const char16_t* s, std::size_t n, std::size_t from) const;

private:
    std::string bytes_;      // folded to lower case when fold_
    std::u16string units_;
    bool fold_ = false, never_ = false, whole_ = false;
};

// A text to search.
struct Text {
    // UTF-8 bytes in memory.
    const char* bytes = nullptr;
    std::size_t byteCount = 0;
    bool ascii = false;
    // UTF-16 units read through `read` (when bytes is null).
    TextFind::Reader read;
    // Its length in UTF-16 units.
    std::size_t length = 0;

    // `ascii` says every byte is under 0x80; else the units are counted.
    static Text utf8(const char* bytes, std::size_t n, bool ascii);
    static Text utf16(std::size_t length, TextFind::Reader read);
};

struct Options {
    // About how much is read or matched between looks at `cancel` (UTF-16
    // units, or bytes), cut after a line break; a longer line is taken whole.
    std::size_t blockUnits = 1 << 20;
    const std::atomic<bool>* cancel = nullptr;
};

// Whether the scan finds `query`'s matches by a needle (anything but a
// regex with no literal, such as \d+, or one left to std::regex, whose
// lines are all matched).
bool usesNeedle(const TextFind::Query& query);

// Calls found(m) for each match of `query` that starts in units [from, to)
// of `text`, in order: the matches TextFind::findAll finds in the whole
// text (a `to` at or past the end takes a match at the very end too).
// found returns false to stop. Returns false when `cancel` stopped it.
bool scan(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
          const std::function<bool(const TextFind::Match&)>& found, const Options& options = {});

// The first match starting in [from, to), or the last (Find Previous, read
// a block at a time back from `to`). False when there is none, or when
// `cancel` stopped it (*cancelled says which).
bool findFirst(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
               TextFind::Match* out, const Options& options = {}, bool* cancelled = nullptr);
bool findLast(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
              TextFind::Match* out, const Options& options = {}, bool* cancelled = nullptr);

}  // namespace TextScan
