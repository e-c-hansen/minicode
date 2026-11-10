// EditSpans.cpp — see EditSpans.h.
#include "EditSpans.h"

#include <algorithm>

namespace EditSpans {

bool valid(const std::vector<Span>& spans, std::size_t size) {
    std::size_t lastEnd = 0;
    for (const Span& s : spans) {
        if (s.start < lastEnd) return false;
        if (s.start > size || s.length > size - s.start) return false;
        lastEnd = s.start + s.length;
    }
    return true;
}

std::size_t mapThrough(std::size_t at, const std::vector<Span>& spans) {
    // What the spans wholly before `at` added (or took away), so far.
    std::size_t added = 0, removed = 0;
    for (const Span& s : spans) {
        const std::size_t end = s.start + s.length;
        if (at >= end) {
            added += s.newLength;
            removed += s.length;
            continue;
        }
        if (at > s.start)
            return s.start + added - removed + std::min(at - s.start, s.newLength);
        break;   // the rest lie after it
    }
    return at + added - removed;
}

namespace {
bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
}  // namespace

std::vector<std::size_t> codePoints(const std::u16string& text,
                                    const std::vector<std::size_t>& sortedUnits) {
    std::vector<std::size_t> out;
    out.reserve(sortedUnits.size());
    const std::size_t n = text.size();
    std::size_t u = 0, cp = 0;
    for (const std::size_t t : sortedUnits) {
        while (u < t && u < n) {
            const std::size_t step = (isHigh(text[u]) && u + 1 < n && isLow(text[u + 1])) ? 2 : 1;
            if (u + step > t) break;   // t is inside the pair: its start
            u += step;
            cp++;
        }
        out.push_back(cp);
    }
    return out;
}

std::size_t unitsAt(const std::u16string& text, std::size_t point) {
    const std::size_t n = text.size();
    std::size_t u = 0;
    for (std::size_t cp = 0; cp < point && u < n; cp++)
        u += (isHigh(text[u]) && u + 1 < n && isLow(text[u + 1])) ? 2 : 1;
    return u;
}

}  // namespace EditSpans
