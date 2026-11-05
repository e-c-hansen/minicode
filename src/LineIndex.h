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
// A line ends after '\n' (so "\r\n" is one break), as the highlighter and
// the language servers count lines. A text ending in '\n' has an empty last
// line, which the gutter numbers too.
#pragma once
#include <cstddef>
#include <vector>

class LineIndex {
public:
    LineIndex() : starts_{0} {}

    // Index a whole text.
    void reset(const char16_t* text, size_t length);

    // The units [pos, pos + oldLen) were replaced by `newLen` units, which are
    // `inserted` (the new text of that range, newLen units long).
    void edit(size_t pos, size_t oldLen, const char16_t* inserted, size_t newLen);

    // The 0-based line holding offset pos (the last line for pos >= length).
    size_t lineOf(size_t pos) const;
    size_t lineCount() const { return starts_.size(); }
    size_t lineStart(size_t line) const {
        return line < gap_ ? starts_[line] : length_ - starts_[line];
    }
    // The end of a line, its '\n' included (the text's length for the last).
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
};
