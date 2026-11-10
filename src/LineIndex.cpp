// LineIndex.cpp — see LineIndex.h.
#include "LineIndex.h"

void LineIndex::reset(const char16_t* text, size_t length) {
    starts_.assign(1, 0);
    length_ = length;
    for (size_t i = 0; i < length; i++)
        if (breaksAfter(text[i], i + 1 < length ? text[i + 1] : 0)) starts_.push_back(i + 1);
    gap_ = starts_.size();
}

// Entries convert between the two forms with the same subtraction.
void LineIndex::moveGap(size_t to) {
    for (; gap_ > to; gap_--) starts_[gap_ - 1] = length_ - starts_[gap_ - 1];
    for (; gap_ < to; gap_++) starts_[gap_] = length_ - starts_[gap_];
}

size_t LineIndex::firstAfter(size_t pos) const {
    size_t lo = 0, hi = starts_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (lineStart(mid) <= pos) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

size_t LineIndex::firstAt(size_t pos) const {
    if (pos == 0) return 1;   // line 0 starts at 0 whatever the text holds
    size_t lo = 0, hi = starts_.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (lineStart(mid) < pos) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void LineIndex::edit(size_t pos, size_t oldLen, const char16_t* inserted, size_t newLen,
                     char16_t before, char16_t after) {
    if (pos > length_) pos = length_;
    if (pos + oldLen > length_) oldLen = length_ - pos;
    std::vector<size_t> fresh;
    if (pos > 0 && breaksAfter(before, newLen ? inserted[0] : after)) fresh.push_back(pos);
    for (size_t i = 0; i < newLen; i++)
        if (breaksAfter(inserted[i], i + 1 < newLen ? inserted[i + 1] : after))
            fresh.push_back(pos + i + 1);
    splice(pos, oldLen, newLen, fresh);
}

void LineIndex::splice(size_t pos, size_t oldLen, size_t newLen,
                       const std::vector<size_t>& fresh) {
    // The starts the edit can change: the one at pos (a '\r' before it ends
    // a line only if what now follows it is not '\n'), and those in
    // (pos, pos + oldLen], which followed a unit that was replaced.
    const size_t first = firstAt(pos);
    const size_t last = firstAfter(pos + oldLen);
    // Everything from `last` on is counted from the end, which the edit
    // does not move.
    moveGap(last);
    const size_t removed = last - first, added = fresh.size();
    if (added > removed)
        starts_.insert(starts_.begin() + (ptrdiff_t)last, added - removed, 0);
    else if (added < removed)
        starts_.erase(starts_.begin() + (ptrdiff_t)(first + added),
                      starts_.begin() + (ptrdiff_t)last);
    for (size_t k = 0; k < added; k++) starts_[first + k] = fresh[k];
    gap_ = first + added;
    length_ = length_ + newLen - oldLen;
}

// ---- code points from UTF-8
namespace {

// Whether byte i of s starts a code point (is not a continuation byte).
inline bool leads(const char* s, size_t i) { return ((unsigned char)s[i] & 0xC0) != 0x80; }

// Whether the code point starting at byte i ends a line: '\n', U+2029, or a
// '\r' not followed by '\n' (`nextIsNewline` when i is the last byte).
inline bool breaksAt(const char* s, size_t i, size_t n, bool nextIsNewline) {
    const unsigned char b = (unsigned char)s[i];
    if (b == '\n') return true;
    if (b == '\r') return !(i + 1 < n ? s[i + 1] == '\n' : nextIsNewline);
    return b == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
           (unsigned char)s[i + 2] == 0xA9;
}

inline bool codePointBreaksAfter(char32_t c, char32_t next) {
    return c == U'\n' || c == 0x2029 || (c == U'\r' && next != U'\n');
}

}  // namespace

void LineIndex::resetUtf8(const char* text, size_t bytes) {
    starts_.assign(1, 0);
    size_t units = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (!leads(text, i)) continue;
        units++;
        if (breaksAt(text, i, bytes, false)) starts_.push_back(units);
    }
    length_ = units;
    gap_ = starts_.size();
}

void LineIndex::editUtf8(size_t pos, size_t oldLen, const char* inserted, size_t bytes,
                         char32_t before, char32_t after) {
    if (pos > length_) pos = length_;
    if (pos + oldLen > length_) oldLen = length_ - pos;
    std::vector<size_t> fresh;
    // Only whether it is '\n' matters to a '\r' before it.
    const char32_t first = bytes ? (inserted[0] == '\n' ? U'\n' : U'x') : after;
    if (pos > 0 && codePointBreaksAfter(before, first)) fresh.push_back(pos);
    size_t units = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (!leads(inserted, i)) continue;
        units++;
        if (breaksAt(inserted, i, bytes, after == U'\n')) fresh.push_back(pos + units);
    }
    splice(pos, oldLen, units, fresh);
}

size_t LineIndex::lineOf(size_t pos) const {
    return firstAfter(pos) - 1;
}

bool LineIndex::isLineStart(size_t pos) const {
    const size_t after = firstAfter(pos);
    return after > 0 && lineStart(after - 1) == pos;
}
