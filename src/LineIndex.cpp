// LineIndex.cpp — see LineIndex.h.
#include "LineIndex.h"

void LineIndex::reset(const char16_t* text, size_t length) {
    starts_.assign(1, 0);
    length_ = length;
    for (size_t i = 0; i < length; i++)
        if (text[i] == u'\n') starts_.push_back(i + 1);
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

void LineIndex::edit(size_t pos, size_t oldLen, const char16_t* inserted, size_t newLen) {
    if (pos > length_) pos = length_;
    if (pos + oldLen > length_) oldLen = length_ - pos;
    // Line starts in (pos, pos + oldLen] followed a '\n' that was replaced.
    const size_t first = firstAfter(pos);
    const size_t last = firstAfter(pos + oldLen);
    // Everything from `last` on is counted from the end, which the edit
    // does not move.
    moveGap(last);
    size_t added = 0;
    for (size_t i = 0; i < newLen; i++)
        if (inserted[i] == u'\n') added++;
    const size_t removed = last - first;
    if (added > removed)
        starts_.insert(starts_.begin() + (ptrdiff_t)last, added - removed, 0);
    else if (added < removed)
        starts_.erase(starts_.begin() + (ptrdiff_t)(first + added),
                      starts_.begin() + (ptrdiff_t)last);
    size_t at = first;
    for (size_t i = 0; i < newLen; i++)
        if (inserted[i] == u'\n') starts_[at++] = pos + i + 1;
    gap_ = at;
    length_ = length_ + newLen - oldLen;
}

size_t LineIndex::lineOf(size_t pos) const {
    return firstAfter(pos) - 1;
}

bool LineIndex::isLineStart(size_t pos) const {
    const size_t after = firstAfter(pos);
    return after > 0 && lineStart(after - 1) == pos;
}
