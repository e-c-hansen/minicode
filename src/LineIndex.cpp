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
    // The starts the edit can change: the one at pos (a '\r' before it ends
    // a line only if what now follows it is not '\n'), and those in
    // (pos, pos + oldLen], which followed a unit that was replaced.
    const size_t first = firstAt(pos);
    const size_t last = firstAfter(pos + oldLen);
    // Everything from `last` on is counted from the end, which the edit
    // does not move.
    moveGap(last);
    std::vector<size_t> fresh;
    if (pos > 0 && breaksAfter(before, newLen ? inserted[0] : after)) fresh.push_back(pos);
    for (size_t i = 0; i < newLen; i++)
        if (breaksAfter(inserted[i], i + 1 < newLen ? inserted[i + 1] : after))
            fresh.push_back(pos + i + 1);
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

size_t LineIndex::lineOf(size_t pos) const {
    return firstAfter(pos) - 1;
}

bool LineIndex::isLineStart(size_t pos) const {
    const size_t after = firstAfter(pos);
    return after > 0 && lineStart(after - 1) == pos;
}
