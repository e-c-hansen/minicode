// MultiCaret.cpp — see MultiCaret.h.
#include "MultiCaret.h"

#include <algorithm>

namespace MultiCaret {

std::u16string Text::substr(size_t pos, size_t n) const {
    if (pos > size_) pos = size_;
    n = std::min(n, size_ - pos);
    if (data_) return std::u16string(data_ + pos, n);
    std::u16string out(n, u'\0');
    if (n >= kBlock) {   // long pieces straight from the reader
        read_(pos, n, &out[0]);
        read_count_ += n;
    } else {
        for (size_t i = 0; i < n; i++) out[i] = (*this)[pos + i];
    }
    return out;
}

void Text::fetch(size_t block) const {
    auto it = blocks_.find(block);
    if (it == blocks_.end()) {
        const size_t start = block << kBlockBits;
        const size_t n = start < size_ ? std::min(kBlock, size_ - start) : 0;
        // Full size whatever the text's end, so reading past it (which no
        // caller does) stays inside the block.
        std::u16string b(kBlock, u'\0');
        if (n) read_(start, n, &b[0]);
        read_count_ += n;
        it = blocks_.emplace(block, std::move(b)).first;
    }
    lastBlock_ = block;
    last_ = it->second.data();
}

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

size_t lineStart(const Text& t, size_t p) {
    while (p > 0 && !isBreak(t[p - 1])) p--;
    return p;
}

size_t lineEnd(const Text& t, size_t p) {
    while (p < t.size() && !isBreak(t[p])) p++;
    return p;
}

size_t stepLeft(const Text& t, size_t p) {
    if (p == 0) return 0;
    if (p >= 2 && t[p - 2] == u'\r' && t[p - 1] == u'\n') return p - 2;
    if (p >= 2 && isLow(t[p - 1]) && isHigh(t[p - 2])) return p - 2;
    return p - 1;
}

size_t stepRight(const Text& t, size_t p) {
    if (p >= t.size()) return t.size();
    if (p + 1 < t.size() && t[p] == u'\r' && t[p + 1] == u'\n') return p + 2;
    if (p + 1 < t.size() && isHigh(t[p]) && isLow(t[p + 1])) return p + 2;
    return p + 1;
}

size_t wordLeft(const Text& t, size_t p) {
    if (p == 0) return 0;
    if (isBreak(t[p - 1])) return stepLeft(t, p);
    while (p > 0 && isBlank(t[p - 1])) p--;
    if (p == 0 || isBreak(t[p - 1])) return p;
    const int cls = charClass(t[p - 1]);
    while (p > 0 && charClass(t[p - 1]) == cls) p--;
    return p;
}

size_t wordRight(const Text& t, size_t p) {
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

Edit backspace(const Text& text, const std::vector<Caret>& carets) {
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

Edit deleteForward(const Text& text, const std::vector<Caret>& carets) {
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

std::vector<Caret> move(const Text& text, const std::vector<Caret>& carets, Move m,
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

std::u16string copy(const Text& text, const std::vector<Caret>& carets) {
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
    const char16_t closer = closerFor(opener);
    if (!closer) return Edit();
    return surround(carets, opener, closer);
}

Edit surround(const std::vector<Caret>& carets, char16_t opener, char16_t closer) {
    Edit e;
    const size_t added = closer ? 2 : 1;
    long long shift = 0;
    for (const Caret& c : carets) {
        if (c.empty()) {
            const size_t p = (size_t)((long long)c.head + shift);
            e.carets.push_back({p, p});
            continue;
        }
        e.changes.push_back({c.lo(), 0, std::u16string(1, opener)});
        if (closer) e.changes.push_back({c.hi(), 0, std::u16string(1, closer)});
        const size_t lo = (size_t)((long long)c.lo() + shift) + 1;
        const size_t hi = (size_t)((long long)c.hi() + shift) + 1;
        e.carets.push_back(c.head < c.anchor ? Caret{hi, lo} : Caret{lo, hi});
        shift += (long long)added;
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

std::vector<Block> lineBlocks(const Text& text, const std::vector<Caret>& carets,
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

Edit duplicateLines(const Text& text, const std::vector<Caret>& carets, bool down) {
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

Edit moveLines(const Text& text, const std::vector<Caret>& carets, bool down) {
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

std::vector<Caret> expandLineSelection(const Text& text,
                                       const std::vector<Caret>& carets) {
    std::vector<Caret> out;
    for (const Caret& c : normalize(carets, text.size())) {
        const size_t s = lineStart(text, c.lo());
        // A selection ending at a line's start already holds the line above
        // it whole: the next line is added. Otherwise the caret's own line.
        const size_t from = !c.empty() && c.hi() == lineStart(text, c.hi()) ? c.hi()
                                                                            : lineEnd(text, c.hi());
        const Caret line{s, stepRight(text, lineEnd(text, from))};
        // Whole lines that meet make one selection, as one block of lines.
        if (!out.empty() && line.lo() <= out.back().hi())
            out.back().head = std::max(out.back().head, line.head);
        else
            out.push_back(line);
    }
    return normalize(out, text.size());
}

Edit deleteLines(const Text& text, const std::vector<Caret>& carets) {
    Edit out;
    const size_t n = text.size();
    long long shift = 0;
    for (const Block& b : lineBlocks(text, carets, true)) {
        const Caret& first = b.carets.front();
        const size_t col = first.head - lineStart(text, first.head);
        size_t from = b.start, to = b.end, land, landLength;
        if (b.end < n) {
            to = stepRight(text, b.end);   // the line below moves up into place
            land = b.start;
            landLength = lineEnd(text, to) - to;
        } else if (b.start > 0) {
            from = stepLeft(text, b.start);   // the last lines: the break above goes
            land = lineStart(text, from);
            landLength = from - land;
        } else {
            land = 0;
            landLength = 0;
        }
        out.changes.push_back({from, to - from, u""});
        const size_t at = (size_t)((long long)land + shift) + std::min(col, landLength);
        out.carets.push_back({at, at});
        shift -= (long long)(to - from);
    }
    out.carets = normalize(out.carets, (size_t)((long long)n + shift));
    return out;
}

std::u16string lineCopy(const Text& text, const std::vector<Caret>& carets) {
    std::u16string out;
    for (const Block& b : lineBlocks(text, carets, false)) {
        out += text.substr(b.start, b.end - b.start);
        const size_t next = stepRight(text, b.end);
        out += next > b.end ? text.substr(b.end, next - b.end) : u"\n";
    }
    return out;
}

Edit pasteLinesAbove(const Text& text, const std::vector<Caret>& carets,
                     const std::u16string& clip) {
    Edit out;
    const std::vector<Caret> sorted = normalize(carets, text.size());
    std::vector<size_t> starts;   // the lines holding a caret, by their start
    for (const Caret& c : sorted) {
        const size_t s = lineStart(text, c.lo());
        if (starts.empty() || starts.back() != s) starts.push_back(s);
    }
    std::u16string whole = clip;
    if (whole.empty() || !isBreak(whole.back())) whole += u'\n';
    // The copy's lines, each with its break.
    std::vector<std::u16string> lines;
    for (size_t p = 0; p < whole.size();) {
        size_t e = lineEnd(whole, p);
        const size_t next = stepRight(whole, e);
        lines.push_back(whole.substr(p, next - p));
        p = next > e ? next : e + 1;
    }
    const bool oneEach = starts.size() > 1 && lines.size() == starts.size();
    std::vector<long long> before(starts.size());   // inserted at or before each line
    long long shift = 0;
    for (size_t i = 0; i < starts.size(); i++) {
        const std::u16string& piece = oneEach ? lines[i] : whole;
        out.changes.push_back({starts[i], 0, piece});
        shift += (long long)piece.size();
        before[i] = shift;
    }
    for (const Caret& c : sorted) {
        const size_t s = lineStart(text, c.lo());
        const size_t i = (size_t)(std::lower_bound(starts.begin(), starts.end(), s) - starts.begin());
        out.carets.push_back({(size_t)((long long)c.anchor + before[i]),
                              (size_t)((long long)c.head + before[i])});
    }
    out.carets = normalize(out.carets, (size_t)((long long)text.size() + shift));
    return out;
}

namespace {

bool isWordChar(char16_t c) { return charClass(c) == 1; }

// VS Code's whole-word test: a match may not run on into a word on either
// side, though a match that starts (or ends) with punctuation may sit
// right against one.
bool wholeWordAt(const std::u16string& t, size_t s, size_t len) {
    if (len == 0) return false;
    const size_t e = s + len;
    if (s > 0 && isWordChar(t[s - 1]) && isWordChar(t[s])) return false;
    if (e < t.size() && isWordChar(t[e]) && isWordChar(t[e - 1])) return false;
    return true;
}

}  // namespace

Caret wordAt(const std::u16string& text, size_t p) {
    const size_t n = text.size();
    if (p > n) p = n;
    if (!((p < n && isWordChar(text[p])) || (p > 0 && isWordChar(text[p - 1])))) return {p, p};
    size_t a = p, b = p;
    while (a > 0 && isWordChar(text[a - 1])) a--;
    while (b < n && isWordChar(text[b])) b++;
    return {a, b};
}

std::vector<Caret> occurrences(const std::u16string& text, const std::u16string& needle,
                               bool wholeWord) {
    std::vector<Caret> out;
    if (needle.empty()) return out;
    size_t pos = 0;
    for (;;) {
        const size_t s = text.find(needle, pos);
        if (s == std::u16string::npos) break;
        if (wholeWord && !wholeWordAt(text, s, needle.size())) {
            pos = s + 1;
            continue;
        }
        out.push_back({s, s + needle.size()});
        pos = s + needle.size();
    }
    return out;
}

Found addNextOccurrence(const std::u16string& text, const std::vector<Caret>& carets,
                        Caret last, bool wholeWord) {
    const size_t n = text.size();
    Found f;
    f.carets = normalize(carets, n);
    f.wholeWord = wholeWord;
    if (last.hi() > n) last = f.carets.back();
    f.added = last;

    // An empty caret selects its word, and the search turns whole-word.
    bool anyEmpty = false;
    for (const Caret& c : f.carets)
        if (c.empty()) anyEmpty = true;
    if (anyEmpty) {
        std::vector<Caret> out;
        for (const Caret& c : f.carets) {
            const Caret w = c.empty() ? wordAt(text, c.head) : c;
            if (!(w == c)) f.changed = true;
            out.push_back(w);
        }
        f.carets = normalize(out, n);
        f.wholeWord = true;
        if (last.empty()) {
            const Caret w = wordAt(text, last.head);
            if (!w.empty()) f.added = w;
        }
        return f;
    }

    const std::u16string needle = text.substr(last.lo(), last.hi() - last.lo());
    const size_t len = needle.size();
    // The selections are sorted and never overlap, so their ends are sorted
    // too: the first one ending after s is the only one that can overlap
    // [s, s + len).
    auto taken = [&](size_t s) {
        size_t lo = 0, hi = f.carets.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (f.carets[mid].hi() <= s) lo = mid + 1;
            else hi = mid;
        }
        return lo < f.carets.size() && f.carets[lo].lo() < s + len;
    };
    const size_t from = last.hi();
    for (int pass = 0; pass < 2; pass++) {
        size_t pos = pass == 0 ? from : 0;
        for (;;) {
            const size_t s = text.find(needle, pos);
            if (s == std::u16string::npos || (pass == 1 && s >= from)) break;
            if ((!wholeWord || wholeWordAt(text, s, len)) && !taken(s)) {
                f.added = {s, s + len};
                std::vector<Caret> out = f.carets;
                out.push_back(f.added);
                f.carets = normalize(out, n);
                f.changed = true;
                return f;
            }
            pos = s + 1;
        }
    }
    return f;
}

Found selectAllOccurrences(const std::u16string& text, Caret last, bool wholeWord) {
    Found f;
    const size_t n = text.size();
    if (last.hi() > n) last = {n, n};
    if (last.empty()) {
        last = wordAt(text, last.head);
        wholeWord = true;
    }
    f.wholeWord = wholeWord;
    f.added = last;
    if (last.empty()) {
        f.carets = {last};
        return f;
    }
    f.carets = occurrences(text, text.substr(last.lo(), last.hi() - last.lo()), wholeWord);
    if (f.carets.empty()) f.carets = {last};   // a selection that fails the whole-word test
    f.changed = true;
    return f;
}

std::u16string applyEdit(const std::u16string& text, const Edit& e) {
    std::u16string out = text;
    for (auto it = e.changes.rbegin(); it != e.changes.rend(); ++it)
        out.replace(it->start, it->length, it->text);
    return out;
}

}  // namespace MultiCaret
