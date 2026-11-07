// SideBySide.cpp — see SideBySide.h.
#include "SideBySide.h"
#include "LineDiff.h"
#include <algorithm>
#include <cstdint>

namespace SideBySide {

size_t lineCount(const std::u16string& text) {
    if (text.empty()) return 0;
    size_t n = 0;
    for (char16_t c : text)
        if (c == u'\n') n++;
    if (text.back() != u'\n') n++;
    return n;
}

std::vector<Row> align(const std::u16string& a, const std::u16string& b, size_t maxEdits) {
    std::vector<Row> rows;
    const size_t na = lineCount(a), nb = lineCount(b);
    // LineDiff counts a line with its "\n"; a last line without one is still
    // a line, and one side ending in "\n" where the other does not would make
    // the last lines differ, so both are compared as if they ended in "\n".
    std::u16string aa = a, bb = b;
    if (!aa.empty() && aa.back() != u'\n') aa += u'\n';
    if (!bb.empty() && bb.back() != u'\n') bb += u'\n';
    std::vector<LineDiff::Hunk> hunks = LineDiff::diff(aa, bb, maxEdits);
    size_t i = 0, j = 0;
    for (const LineDiff::Hunk& h : hunks) {
        while (i < h.oldLine && j < h.newLine) rows.push_back({Kind::Same, (int)i++, (int)j++});
        const size_t pairs = h.oldLines < h.newLines ? h.oldLines : h.newLines;
        for (size_t k = 0; k < pairs; k++) rows.push_back({Kind::Changed, (int)i++, (int)j++});
        for (size_t k = pairs; k < h.oldLines; k++) rows.push_back({Kind::Removed, (int)i++, -1});
        for (size_t k = pairs; k < h.newLines; k++) rows.push_back({Kind::Added, -1, (int)j++});
    }
    while (i < na && j < nb) rows.push_back({Kind::Same, (int)i++, (int)j++});
    // Only when the hunks left lines over (they never should).
    while (i < na) rows.push_back({Kind::Removed, (int)i++, -1});
    while (j < nb) rows.push_back({Kind::Added, -1, (int)j++});
    return rows;
}

std::vector<size_t> changeStarts(const std::vector<Row>& rows) {
    std::vector<size_t> out;
    for (const Hunk& h : hunks(rows)) out.push_back(h.first);
    return out;
}

std::vector<Hunk> hunks(const std::vector<Row>& rows) {
    std::vector<Hunk> out;
    for (size_t r = 0; r < rows.size(); r++) {
        if (rows[r].kind == Kind::Same) continue;
        if (!out.empty() && out.back().end == r) out.back().end = r + 1;
        else out.push_back({r, r + 1});
    }
    return out;
}

int stepHunk(const std::vector<Hunk>& hunks, long from, bool forward) {
    if (hunks.empty()) return -1;
    const int n = (int)hunks.size();
    if (forward) {
        for (int i = 0; i < n; i++)
            if ((long)hunks[(size_t)i].first > from) return i;
        return 0;
    }
    for (int i = n - 1; i >= 0; i--)
        if ((long)hunks[(size_t)i].first < from) return i;
    return n - 1;
}

namespace {
// Where each line of `text` starts, then one past the end: line k is
// [starts[k], starts[k + 1]), its "\n" included.
std::vector<size_t> lineStarts(const std::u16string& text) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == u'\n') starts.push_back(i + 1);
    if (starts.back() != text.size()) starts.push_back(text.size());
    return starts;
}

// The end of a line's text, before its "\n" or "\r\n".
size_t contentEnd(const std::u16string& text, size_t from, size_t to) {
    if (to > from && text[to - 1] == u'\n') to--;
    if (to > from && text[to - 1] == u'\r') to--;
    return to;
}
}  // namespace

std::u16string paneText(const std::u16string& text, const std::vector<Row>& rows, bool left,
                        std::vector<size_t>* starts) {
    const std::vector<size_t> ls = lineStarts(text);
    const size_t lines = ls.size() - 1;
    std::u16string out;
    out.reserve(text.size() + rows.size());
    if (starts) {
        starts->clear();
        starts->reserve(rows.size());
    }
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0) out += u'\n';
        if (starts) starts->push_back(out.size());
        const int line = left ? rows[r].left : rows[r].right;
        if (line < 0 || (size_t)line >= lines) continue;
        const size_t from = ls[(size_t)line];
        out.append(text, from, contentEnd(text, from, ls[(size_t)line + 1]) - from);
    }
    return out;
}

std::vector<Span> spansOnPane(const std::u16string& text, const std::vector<Row>& rows, bool left,
                              const std::vector<size_t>& starts, const std::vector<Span>& spans) {
    std::vector<Span> out;
    if (rows.size() != starts.size()) return out;
    const std::vector<size_t> ls = lineStarts(text);
    const size_t lines = ls.size() - 1;
    // The row showing each line of this side.
    std::vector<size_t> rowOf(lines, SIZE_MAX);
    for (size_t r = 0; r < rows.size(); r++) {
        const int line = left ? rows[r].left : rows[r].right;
        if (line >= 0 && (size_t)line < lines) rowOf[(size_t)line] = r;
    }
    size_t line = 0;
    for (const Span& s : spans) {
        size_t pos = s.start;
        const size_t end = std::min(s.start + s.length, text.size());
        // Spans come in order; one that went back starts the walk again.
        if (line < lines && pos < ls[line]) line = 0;
        while (pos < end) {
            while (line < lines && ls[line + 1] <= pos) line++;
            if (line >= lines) break;
            const size_t lineStart = ls[line];
            const size_t cut = std::min(end, contentEnd(text, lineStart, ls[line + 1]));
            if (cut > pos && rowOf[line] != SIZE_MAX)
                out.push_back({starts[rowOf[line]] + (pos - lineStart), cut - pos, s.style});
            pos = ls[line + 1];
        }
    }
    return out;
}

namespace {
enum class TokenKind { Word, Blank, Other };

bool isWordUnit(char16_t c) {
    return c == u'_' || (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') ||
           (c >= u'A' && c <= u'Z') || c >= 0x80;
}

struct WordToken {
    size_t start, length;
    TokenKind kind;
};

std::vector<WordToken> tokenize(const std::u16string& s) {
    std::vector<WordToken> out;
    size_t i = 0;
    while (i < s.size()) {
        const char16_t c = s[i];
        size_t j = i + 1;
        TokenKind kind = TokenKind::Other;
        if (c == u' ' || c == u'\t') {
            kind = TokenKind::Blank;
            while (j < s.size() && (s[j] == u' ' || s[j] == u'\t')) j++;
        } else if (isWordUnit(c)) {
            kind = TokenKind::Word;
            while (j < s.size() && isWordUnit(s[j])) j++;
        }
        out.push_back({i, j - i, kind});
        i = j;
    }
    return out;
}

bool sameToken(const std::u16string& a, const WordToken& x, const std::u16string& b,
               const WordToken& y) {
    return x.length == y.length && a.compare(x.start, x.length, b, y.start, y.length) == 0;
}

// The changed tokens as spans, a kept run of blanks between two changes
// joining them.
std::vector<Span> changedSpans(const std::vector<WordToken>& t, std::vector<char> keep) {
    for (size_t k = 1; k < t.size(); k++) {
        if (!keep[k] || t[k].kind != TokenKind::Blank || keep[k - 1]) continue;
        size_t e = k;
        while (e < t.size() && keep[e] && t[e].kind == TokenKind::Blank) e++;
        if (e < t.size() && !keep[e])
            for (size_t q = k; q < e; q++) keep[q] = 0;
    }
    std::vector<Span> out;
    for (size_t k = 0; k < t.size(); k++) {
        if (keep[k]) continue;
        if (!out.empty() && out.back().start + out.back().length == t[k].start)
            out.back().length += t[k].length;
        else
            out.push_back({t[k].start, t[k].length, 0});
    }
    return out;
}
}  // namespace

WordChanges wordChanges(const std::u16string& oldLine, const std::u16string& newLine) {
    WordChanges out;
    if (oldLine == newLine) return out;
    const std::vector<WordToken> ta = tokenize(oldLine), tb = tokenize(newLine);
    const size_t na = ta.size(), nb = tb.size();
    // The common head and tail first; the middle by a longest common
    // subsequence of tokens, or as one change when it is too long to compare
    // (500 tokens a side is longer than any line a person reads).
    size_t head = 0;
    while (head < na && head < nb && sameToken(oldLine, ta[head], newLine, tb[head])) head++;
    size_t tail = 0;
    while (tail < na - head && tail < nb - head &&
           sameToken(oldLine, ta[na - 1 - tail], newLine, tb[nb - 1 - tail]))
        tail++;
    const size_t ma = na - head - tail, mb = nb - head - tail;
    std::vector<char> keepA(na, 0), keepB(nb, 0);
    for (size_t k = 0; k < head; k++) keepA[k] = keepB[k] = 1;
    for (size_t k = 0; k < tail; k++) keepA[na - 1 - k] = keepB[nb - 1 - k] = 1;
    if (ma && mb && ma * mb <= 250000) {
        // at(i, j): the longest common subsequence of the middles from i and j.
        std::vector<uint32_t> lcs((ma + 1) * (mb + 1), 0);
        auto at = [&](size_t i, size_t j) -> uint32_t& { return lcs[i * (mb + 1) + j]; };
        for (size_t i = ma; i-- > 0;)
            for (size_t j = mb; j-- > 0;)
                at(i, j) = sameToken(oldLine, ta[head + i], newLine, tb[head + j])
                               ? at(i + 1, j + 1) + 1
                               : std::max(at(i + 1, j), at(i, j + 1));
        size_t i = 0, j = 0;
        while (i < ma && j < mb) {
            if (sameToken(oldLine, ta[head + i], newLine, tb[head + j])) {
                keepA[head + i] = keepB[head + j] = 1;
                i++;
                j++;
            } else if (at(i + 1, j) >= at(i, j + 1)) {
                i++;
            } else {
                j++;
            }
        }
    }
    // Nothing shared but blanks: the whole line changed, which its tint says.
    bool shared = false;
    for (size_t k = 0; k < na && !shared; k++) shared = keepA[k] && ta[k].kind != TokenKind::Blank;
    if (!shared) return out;
    out.left = changedSpans(ta, keepA);
    out.right = changedSpans(tb, keepB);
    return out;
}

std::u16string assemble(const std::u16string& a, const std::u16string& b,
                        const std::vector<Row>& rows, const std::vector<bool>& accepted) {
    const std::vector<size_t> la = lineStarts(a), lb = lineStarts(b);
    auto lineOf = [](const std::u16string& t, const std::vector<size_t>& ls, int k) {
        if (k < 0 || (size_t)k + 1 >= ls.size()) return std::u16string();
        return t.substr(ls[(size_t)k], ls[(size_t)k + 1] - ls[(size_t)k]);
    };
    std::u16string out;
    out.reserve(std::max(a.size(), b.size()));
    long hunk = -1;
    for (size_t r = 0; r < rows.size(); r++) {
        const Row& row = rows[r];
        if (row.kind == Kind::Same) {
            out += lineOf(b, lb, row.right);
            continue;
        }
        if (r == 0 || rows[r - 1].kind == Kind::Same) hunk++;
        const bool take = (size_t)hunk < accepted.size() && accepted[(size_t)hunk];
        out += take ? lineOf(b, lb, row.right) : lineOf(a, la, row.left);
    }
    return out;
}

}  // namespace SideBySide
