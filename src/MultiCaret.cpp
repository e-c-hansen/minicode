// MultiCaret.cpp — see MultiCaret.h.
#include "MultiCaret.h"

#include <algorithm>

namespace MultiCaret {

namespace {

bool isBreak(char16_t c) { return c == u'\n' || c == u'\r'; }
bool isBlank(char16_t c) { return c == u' ' || c == u'\t'; }
bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

int charClass(char16_t c) {
    if (isBlank(c)) return 0;
    if (isBreak(c)) return 3;
    if ((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
        c == u'_' || c >= 0x80)
        return 1;
    return 2;
}

size_t lineStart(const std::u16string& t, size_t p) {
    while (p > 0 && !isBreak(t[p - 1])) p--;
    return p;
}

size_t lineEnd(const std::u16string& t, size_t p) {
    while (p < t.size() && !isBreak(t[p])) p++;
    return p;
}

size_t stepLeft(const std::u16string& t, size_t p) {
    if (p == 0) return 0;
    if (p >= 2 && t[p - 2] == u'\r' && t[p - 1] == u'\n') return p - 2;
    if (p >= 2 && isLow(t[p - 1]) && isHigh(t[p - 2])) return p - 2;
    return p - 1;
}

size_t stepRight(const std::u16string& t, size_t p) {
    if (p >= t.size()) return t.size();
    if (p + 1 < t.size() && t[p] == u'\r' && t[p + 1] == u'\n') return p + 2;
    if (p + 1 < t.size() && isHigh(t[p]) && isLow(t[p + 1])) return p + 2;
    return p + 1;
}

size_t wordLeft(const std::u16string& t, size_t p) {
    if (p == 0) return 0;
    if (isBreak(t[p - 1])) return stepLeft(t, p);
    while (p > 0 && isBlank(t[p - 1])) p--;
    if (p == 0 || isBreak(t[p - 1])) return p;
    const int cls = charClass(t[p - 1]);
    while (p > 0 && charClass(t[p - 1]) == cls) p--;
    return p;
}

size_t wordRight(const std::u16string& t, size_t p) {
    const size_t n = t.size();
    if (p >= n) return n;
    if (isBreak(t[p])) return stepRight(t, p);
    while (p < n && isBlank(t[p])) p++;
    if (p >= n || isBreak(t[p])) return p;
    const int cls = charClass(t[p]);
    while (p < n && charClass(t[p]) == cls) p++;
    return p;
}

// Builds an edit from one change per caret (in order, a zero-length empty
// change for "nothing here") and, per caret, whether it ends after the
// inserted text (always, here: deletions insert nothing).
Edit build(const std::vector<Caret>& carets, std::vector<Change> changes, size_t oldLength) {
    Edit e;
    long long shift = 0;
    std::vector<Caret> after;
    after.reserve(carets.size());
    for (size_t i = 0; i < changes.size(); i++) {
        const Change& c = changes[i];
        const size_t pos = (size_t)((long long)c.start + shift) + c.text.size();
        after.push_back({pos, pos});
        shift += (long long)c.text.size() - (long long)c.length;
    }
    for (Change& c : changes)
        if (c.length || !c.text.empty()) e.changes.push_back(std::move(c));
    e.carets = normalize(after, (size_t)((long long)oldLength + shift));
    return e;
}

}  // namespace

std::vector<Caret> normalize(std::vector<Caret> carets, size_t n) {
    for (Caret& c : carets) {
        c.anchor = std::min(c.anchor, n);
        c.head = std::min(c.head, n);
    }
    std::stable_sort(carets.begin(), carets.end(), [](const Caret& a, const Caret& b) {
        return a.lo() < b.lo() || (a.lo() == b.lo() && a.hi() < b.hi());
    });
    std::vector<Caret> out;
    for (const Caret& c : carets) {
        if (!out.empty()) {
            Caret& last = out.back();
            const bool touch = c.lo() == last.hi() && (c.empty() || last.empty());
            if (c.lo() < last.hi() || touch) {
                // One selection over both, kept in the direction of the
                // later caret (the one most recently extended, as a rule).
                const size_t lo = std::min(last.lo(), c.lo()), hi = std::max(last.hi(), c.hi());
                const bool backwards = c.empty() ? last.head < last.anchor : c.head < c.anchor;
                last = backwards ? Caret{hi, lo} : Caret{lo, hi};
                continue;
            }
        }
        out.push_back(c);
    }
    if (out.empty()) out.push_back({0, 0});
    return out;
}

Edit insert(const std::vector<Caret>& carets, const std::vector<std::u16string>& texts) {
    std::vector<Change> changes;
    size_t length = 0;
    for (const Caret& c : carets) length = std::max(length, c.hi());
    for (size_t i = 0; i < carets.size(); i++) {
        const std::u16string& s = texts.size() == carets.size() ? texts[i]
                                  : texts.empty()               ? std::u16string()
                                                                : texts[0];
        changes.push_back({carets[i].lo(), carets[i].hi() - carets[i].lo(), s});
    }
    // The text's length is not known here, only that it reaches the last
    // caret; that is enough, since no caret ends past what was inserted.
    return build(carets, std::move(changes), length);
}

Edit backspace(const std::u16string& text, const std::vector<Caret>& carets) {
    std::vector<Change> changes;
    for (const Caret& c : carets) {
        if (!c.empty()) {
            changes.push_back({c.lo(), c.hi() - c.lo(), {}});
            continue;
        }
        const size_t p = std::min(c.head, text.size());
        if (p == 0 || isBreak(text[p - 1])) {
            changes.push_back({p, 0, {}});
            continue;
        }
        const size_t q = stepLeft(text, p);
        changes.push_back({q, p - q, {}});
    }
    return build(carets, std::move(changes), text.size());
}

Edit deleteForward(const std::u16string& text, const std::vector<Caret>& carets) {
    std::vector<Change> changes;
    for (const Caret& c : carets) {
        if (!c.empty()) {
            changes.push_back({c.lo(), c.hi() - c.lo(), {}});
            continue;
        }
        const size_t p = std::min(c.head, text.size());
        if (p >= text.size() || isBreak(text[p])) {
            changes.push_back({p, 0, {}});
            continue;
        }
        changes.push_back({p, stepRight(text, p) - p, {}});
    }
    return build(carets, std::move(changes), text.size());
}

std::vector<Caret> move(const std::u16string& text, const std::vector<Caret>& carets, Move m,
                        bool extend) {
    std::vector<Caret> out;
    out.reserve(carets.size());
    for (const Caret& c : carets) {
        size_t p = std::min(c.head, text.size());
        if (!extend && !c.empty() && (m == Move::Left || m == Move::Right)) {
            const size_t edge = m == Move::Left ? c.lo() : c.hi();
            out.push_back({edge, edge});
            continue;
        }
        switch (m) {
        case Move::Left: p = stepLeft(text, p); break;
        case Move::Right: p = stepRight(text, p); break;
        case Move::WordLeft: p = wordLeft(text, p); break;
        case Move::WordRight: p = wordRight(text, p); break;
        case Move::LineStart: {
            const size_t s = lineStart(text, p), e = lineEnd(text, p);
            size_t f = s;
            while (f < e && isBlank(text[f])) f++;
            p = p == f ? s : f;
            break;
        }
        case Move::LineEnd: p = lineEnd(text, p); break;
        }
        out.push_back(extend ? Caret{c.anchor, p} : Caret{p, p});
    }
    return normalize(out, text.size());
}

std::vector<std::u16string> pieces(const std::u16string& clip, size_t carets) {
    if (carets < 2) return {clip};
    std::u16string body = clip;
    if (!body.empty() && body.back() == u'\n') body.pop_back();
    if (!body.empty() && body.back() == u'\r') body.pop_back();
    std::vector<std::u16string> lines;
    size_t start = 0;
    for (;;) {
        const size_t nl = body.find(u'\n', start);
        std::u16string line = body.substr(start, nl == std::u16string::npos ? nl : nl - start);
        if (!line.empty() && line.back() == u'\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::u16string::npos) break;
        start = nl + 1;
    }
    if (lines.size() == carets) return lines;
    return {clip};
}

std::u16string copy(const std::u16string& text, const std::vector<Caret>& carets) {
    std::u16string out;
    bool first = true;
    for (const Caret& c : carets) {
        if (c.empty() || c.lo() >= text.size()) continue;
        if (!first) out += u'\n';
        first = false;
        out += text.substr(c.lo(), std::min(c.hi(), text.size()) - c.lo());
    }
    return out;
}

char16_t closerFor(char16_t c) {
    switch (c) {
    case u'(': return u')';
    case u'[': return u']';
    case u'{': return u'}';
    case u'"': return u'"';
    case u'\'': return u'\'';
    case u'`': return u'`';
    default: return 0;
    }
}

Edit surround(const std::vector<Caret>& carets, char16_t opener) {
    Edit e;
    const char16_t closer = closerFor(opener);
    if (!closer) return e;
    long long shift = 0;
    for (const Caret& c : carets) {
        if (c.empty()) {
            const size_t p = (size_t)((long long)c.head + shift);
            e.carets.push_back({p, p});
            continue;
        }
        e.changes.push_back({c.lo(), 0, std::u16string(1, opener)});
        e.changes.push_back({c.hi(), 0, std::u16string(1, closer)});
        const size_t lo = (size_t)((long long)c.lo() + shift) + 1;
        const size_t hi = (size_t)((long long)c.hi() + shift) + 1;
        e.carets.push_back(c.head < c.anchor ? Caret{hi, lo} : Caret{lo, hi});
        shift += 2;
    }
    return e;
}

namespace {

// The whole lines each caret touches, as [start, end) without the final
// break, in order. A selection ending at the start of a line leaves that
// line out; carets whose lines overlap share a block, and with `adjacent`
// so do carets on neighbouring lines.
struct Block {
    size_t start, end;
    std::vector<Caret> carets;
};

std::vector<Block> lineBlocks(const std::u16string& text, const std::vector<Caret>& carets,
                              bool adjacent) {
    std::vector<Block> blocks;
    for (const Caret& c : normalize(carets, text.size())) {
        const size_t s = lineStart(text, c.lo());
        size_t last = c.hi();
        if (c.hi() > c.lo() && c.hi() == lineStart(text, c.hi()) && c.hi() > s)
            last = stepLeft(text, c.hi());
        const size_t e = lineEnd(text, last);
        const bool joins = !blocks.empty() &&
                           (s <= blocks.back().end ||
                            (adjacent && s == stepRight(text, blocks.back().end)));
        if (joins) {
            blocks.back().end = std::max(blocks.back().end, e);
            blocks.back().carets.push_back(c);
        } else {
            blocks.push_back({s, e, {c}});
        }
    }
    return blocks;
}

}  // namespace

Edit duplicateLines(const std::u16string& text, const std::vector<Caret>& carets, bool down) {
    const std::vector<Block> blocks = lineBlocks(text, carets, false);
    Edit out;
    long long shift = 0;
    for (const Block& b : blocks) {
        const std::u16string body = text.substr(b.start, b.end - b.start);
        const std::u16string br = b.end < text.size() && text[b.end] == u'\r' ? u"\r\n" : u"\n";
        const long long moved = down ? (long long)(body.size() + br.size()) : 0;
        if (down)
            out.changes.push_back({b.end, 0, br + body});
        else
            out.changes.push_back({b.start, 0, body + br});
        for (const Caret& c : b.carets)
            out.carets.push_back({(size_t)((long long)c.anchor + shift + moved),
                                  (size_t)((long long)c.head + shift + moved)});
        shift += (long long)(body.size() + br.size());
    }
    return out;
}

Edit moveLines(const std::u16string& text, const std::vector<Caret>& carets, bool down) {
    const std::vector<Block> blocks = lineBlocks(text, carets, true);
    Edit out;
    // Nothing moves when a block is already at the edge it would move past.
    if (down ? blocks.back().end >= text.size() : blocks.front().start == 0) {
        out.carets = normalize(carets, text.size());
        return out;
    }
    for (const Block& b : blocks) {
        const std::u16string body = text.substr(b.start, b.end - b.start);
        if (down) {
            // Swap with the line below: [start, nextEnd) becomes next + br + body.
            const size_t ns = stepRight(text, b.end), ne = lineEnd(text, ns);
            const std::u16string br = text.substr(b.end, ns - b.end);
            const std::u16string next = text.substr(ns, ne - ns);
            out.changes.push_back({b.start, ne - b.start, next + br + body});
            const size_t by = next.size() + br.size();
            for (const Caret& c : b.carets) out.carets.push_back({c.anchor + by, c.head + by});
        } else {
            // Swap with the line above: [prevStart, end) becomes body + br + prev.
            const size_t pe = stepLeft(text, b.start), ps = lineStart(text, pe);
            const std::u16string br = text.substr(pe, b.start - pe);
            const std::u16string prev = text.substr(ps, pe - ps);
            out.changes.push_back({ps, b.end - ps, body + br + prev});
            const size_t by = prev.size() + br.size();
            for (const Caret& c : b.carets) out.carets.push_back({c.anchor - by, c.head - by});
        }
    }
    return out;
}

std::u16string applyEdit(const std::u16string& text, const Edit& e) {
    std::u16string out = text;
    for (auto it = e.changes.rbegin(); it != e.changes.rend(); ++it)
        out.replace(it->start, it->length, it->text);
    return out;
}

}  // namespace MultiCaret
