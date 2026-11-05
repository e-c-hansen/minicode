// BracketMatch.cpp — see BracketMatch.h.
#include "BracketMatch.h"
#include <algorithm>

namespace Brackets {

namespace {

// 0 for ( ), 1 for [ ], 2 for { }, -1 otherwise.
int kindOf(char16_t c) {
    switch (c) {
    case u'(': case u')': return 0;
    case u'[': case u']': return 1;
    case u'{': case u'}': return 2;
    default: return -1;
    }
}

bool inCode(const char16_t* text, size_t at, const std::vector<Span>& skip) {
    return kindOf(text[at]) >= 0 && !skipped(skip, at);
}

}  // namespace

bool isBracket(char16_t c) { return kindOf(c) >= 0; }
bool isOpener(char16_t c) { return c == u'(' || c == u'[' || c == u'{'; }

bool skipped(const std::vector<Span>& skip, size_t at) {
    // The last span starting at or before `at`.
    auto it = std::upper_bound(skip.begin(), skip.end(), at,
                               [](size_t pos, const Span& s) { return pos < s.start; });
    if (it == skip.begin()) return false;
    --it;
    return at < it->end;
}

size_t touching(const char16_t* text, size_t length, size_t caret,
                const std::vector<Span>& skip) {
    if (caret > length) return npos;
    if (caret > 0 && inCode(text, caret - 1, skip)) return caret - 1;
    if (caret < length && inCode(text, caret, skip)) return caret;
    return npos;
}

size_t partner(const char16_t* text, size_t length, size_t at,
               const std::vector<Span>& skip, size_t maxScan) {
    if (at >= length) return npos;
    const char16_t c = text[at];
    const int kind = kindOf(c);
    if (kind < 0) return npos;
    int depth = 0;
    if (isOpener(c)) {
        const size_t stop = std::min(length, at + 1 + maxScan);
        for (size_t i = at + 1; i < stop; i++) {
            if (kindOf(text[i]) != kind || skipped(skip, i)) continue;
            if (isOpener(text[i])) depth++;
            else if (depth-- == 0) return i;
        }
    } else {
        const size_t stop = at > maxScan ? at - maxScan : 0;
        for (size_t i = at; i-- > stop;) {
            if (kindOf(text[i]) != kind || skipped(skip, i)) continue;
            if (!isOpener(text[i])) depth++;
            else if (depth-- == 0) return i;
        }
    }
    return npos;
}

Pair match(const char16_t* text, size_t length, size_t caret,
           const std::vector<Span>& skip, size_t maxScan) {
    Pair p;
    const size_t at = touching(text, length, caret, skip);
    if (at == npos) return p;
    const size_t other = partner(text, length, at, skip, maxScan);
    if (other == npos) return p;
    p.open = std::min(at, other);
    p.close = std::max(at, other);
    return p;
}

Pair enclosing(const char16_t* text, size_t length, size_t caret,
               const std::vector<Span>& skip, size_t maxScan) {
    Pair p;
    if (caret > length) return p;
    int depth[3] = {0, 0, 0};
    const size_t stop = caret > maxScan ? caret - maxScan : 0;
    for (size_t i = caret; i-- > stop;) {
        const int kind = kindOf(text[i]);
        if (kind < 0 || skipped(skip, i)) continue;
        if (!isOpener(text[i])) { depth[kind]++; continue; }
        if (depth[kind] > 0) { depth[kind]--; continue; }
        const size_t close = partner(text, length, i, skip, maxScan);
        if (close == npos) return p;
        p.open = i;
        p.close = close;
        return p;
    }
    return p;
}

size_t jumpTarget(const char16_t* text, size_t length, size_t caret,
                  const std::vector<Span>& skip, size_t maxScan) {
    Pair p = match(text, length, caret, skip, maxScan);
    if (p.found()) {
        // Touching the closer (just before or just after it): to the opener.
        // Otherwise the caret touches the opener: to the closer.
        return caret == p.close || caret == p.close + 1 ? p.open : p.close;
    }
    p = enclosing(text, length, caret, skip, maxScan);
    if (p.found()) return p.close;
    const size_t stop = std::min(length, caret + maxScan);
    for (size_t i = caret; i < stop; i++)
        if (inCode(text, i, skip)) return i;
    return npos;
}

}  // namespace Brackets
