// LineIndex.h — pure C++. Where each line of a text starts, kept up to date
// edit by edit, for the editor's line-number gutter: the line of a character
// and the start of a line in O(log n), and an edit costs the length of the
// text it inserts, never a rescan of the file. Offsets are UTF-16 code
// units, as NSString and NSTextStorage count.
//
// The starts are a gap buffer: those before the last edit are kept as
// offsets, those after it as distances from the end of the text, which an
// edit before them leaves alone. So typing on one line costs nothing per
// line of the file, and moving to another line converts only the starts in
// between.
//
// A line ends after '\n', after a '\r' not followed by '\n' (an old Mac
// file's breaks), and after U+2029, so "\r\n" is one break: the lines
// TextFind and FolderSearch number, and NSTextView draws. A text ending in
// a break has an empty last line, which the gutter numbers too.
#pragma once
#include <cstddef>
#include <vector>

class LineIndex {
public:
    LineIndex() : starts_{0} {}

    // Index a whole text.
    void reset(const char16_t* text, size_t length);

    // The units [pos, pos + oldLen) were replaced by `newLen` units, which are
    // `inserted` (the new text of that range, newLen units long). `before`
    // and `after` are the units on either side of it in the new text (0 at
    // either end), which decide whether a '\r' there still ends a line.
    void edit(size_t pos, size_t oldLen, const char16_t* inserted, size_t newLen,
              char16_t before, char16_t after);

    // Whether the unit at i ends a line, given the unit after it (0 at the end).
    static bool breaksAfter(char16_t c, char16_t next) {
        return c == u'\n' || c == 0x2029 || (c == u'\r' && next != u'\n');
    }

    // The 0-based line holding offset pos (the last line for pos >= length).
    size_t lineOf(size_t pos) const;
    size_t lineCount() const { return starts_.size(); }
    size_t lineStart(size_t line) const {
        return line < gap_ ? starts_[line] : length_ - starts_[line];
    }
    // The end of a line, its break included (the text's length for the last).
    size_t lineEnd(size_t line) const {
        return line + 1 < starts_.size() ? lineStart(line + 1) : length_;
    }
    size_t length() const { return length_; }
    // Whether offset pos is the first unit of a line.
    bool isLineStart(size_t pos) const;

private:
    // Each line's first unit: an offset before gap_, the distance from the
    // end of the text from gap_ on. starts_[0] is line 0, at 0.
    std::vector<size_t> starts_;
    size_t gap_ = 1;
    size_t length_ = 0;

    void moveGap(size_t to);
    size_t firstAfter(size_t pos) const;   // the first line starting after pos
    size_t firstAt(size_t pos) const;      // the first line starting at or after pos
};
