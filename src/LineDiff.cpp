// LineDiff.cpp — see LineDiff.h.
#include "LineDiff.h"
#include <algorithm>
#include <unordered_map>

namespace LineDiff {

namespace {

struct Lines {
    std::vector<size_t> start;   // offset of each line; one extra entry: the end
    std::vector<int> id;         // equal lines share an id
};

Lines split(const std::u16string& s, std::unordered_map<std::u16string, int>& ids) {
    Lines l;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find(u'\n', pos);
        size_t end = nl == std::u16string::npos ? s.size() : nl + 1;
        l.start.push_back(pos);
        auto it = ids.emplace(s.substr(pos, end - pos), (int)ids.size()).first;
        l.id.push_back(it->second);
        pos = end;
    }
    l.start.push_back(s.size());
    return l;
}

bool isSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' ||
           c == u'\v';
}

// Myers' greedy diff over a[a0, a1) and b[b0, b1). Appends the matched line
// pairs, in order. False when more than maxEdits edits would be needed.
bool myers(const std::vector<int>& a, size_t a0, size_t a1,
           const std::vector<int>& b, size_t b0, size_t b1, size_t maxEdits,
           std::vector<std::pair<size_t, size_t>>& matches) {
    const long n = (long)(a1 - a0), m = (long)(b1 - b0);
    const long max = std::min<long>(n + m, (long)maxEdits);
    std::vector<long> v(2 * max + 3, 0);
    const long off = max + 1;
    std::vector<std::vector<long>> trace;   // trace[d][k + d] = v[k] after step d
    long found = -1;
    for (long d = 0; d <= max && found < 0; d++) {
        for (long k = -d; k <= d; k += 2) {
            long x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1]))
                         ? v[off + k + 1]
                         : v[off + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && a[a0 + x] == b[b0 + y]) { x++; y++; }
            v[off + k] = x;
            if (x >= n && y >= m) { found = d; }
        }
        trace.emplace_back(v.begin() + (off - d), v.begin() + (off + d + 1));
    }
    if (found < 0) return false;

    std::vector<std::pair<size_t, size_t>> rev;
    long x = n, y = m;
    for (long d = found; d > 0; d--) {
        const std::vector<long>& prev = trace[d - 1];   // index k + (d - 1)
        long k = x - y;
        auto at = [&](long kk) { return prev[kk + d - 1]; };
        long prevK = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        long prevX = at(prevK), prevY = prevX - prevK;
        // The snake after the edit: equal lines.
        long startX = prevK == k + 1 ? prevX : prevX + 1;
        while (x > startX) { x--; y--; rev.push_back({(size_t)x, (size_t)y}); }
        x = prevX;
        y = prevY;
    }
    while (x > 0 && y > 0) { x--; y--; rev.push_back({(size_t)x, (size_t)y}); }
    for (auto it = rev.rbegin(); it != rev.rend(); ++it)
        matches.push_back({a0 + it->first, b0 + it->second});
    return true;
}

}  // namespace

std::vector<Hunk> diff(const std::u16string& a, const std::u16string& b,
                       size_t maxEdits) {
    std::vector<Hunk> hunks;
    if (a == b) return hunks;
    std::unordered_map<std::u16string, int> ids;
    Lines la = split(a, ids), lb = split(b, ids);
    const size_t na = la.id.size(), nb = lb.id.size();

    size_t head = 0;
    while (head < na && head < nb && la.id[head] == lb.id[head]) head++;
    size_t tail = 0;
    while (tail < na - head && tail < nb - head &&
           la.id[na - 1 - tail] == lb.id[nb - 1 - tail])
        tail++;

    std::vector<std::pair<size_t, size_t>> matches;
    if (!myers(la.id, head, na - tail, lb.id, head, nb - tail, maxEdits, matches))
        matches.clear();   // too different: the whole middle is one hunk
    matches.push_back({na - tail, nb - tail});   // sentinel: the common tail

    size_t ai = head, bi = head;
    for (const auto& mt : matches) {
        if (mt.first > ai || mt.second > bi) {
            Hunk h;
            h.oldLine = ai;
            h.oldLines = mt.first - ai;
            h.newLine = bi;
            h.newLines = mt.second - bi;
            h.oldStart = la.start[ai];
            h.oldLength = la.start[mt.first] - h.oldStart;
            h.newStart = lb.start[bi];
            h.newLength = lb.start[mt.second] - h.newStart;
            hunks.push_back(h);
        }
        ai = mt.first + 1;
        bi = mt.second + 1;
    }

    // Join hunks that only blank lines keep apart. A blank line matches any
    // other, so the diff may pair one the formatter kept with one it added
    // elsewhere, leaving "def h(x,y):" in one hunk and "def h(x, y):" in the
    // next; joined, both sides of a hunk hold the same code, which is what
    // mapOffset needs.
    std::vector<Hunk> joined;
    for (const Hunk& h : hunks) {
        if (!joined.empty()) {
            Hunk& last = joined.back();
            size_t gapStart = last.oldStart + last.oldLength;
            bool blank = true;
            for (size_t i = gapStart; i < h.oldStart && blank; i++) blank = isSpace(a[i]);
            if (blank) {
                last.oldLines = h.oldLine + h.oldLines - last.oldLine;
                last.newLines = h.newLine + h.newLines - last.newLine;
                last.oldLength = h.oldStart + h.oldLength - last.oldStart;
                last.newLength = h.newStart + h.newLength - last.newStart;
                continue;
            }
        }
        joined.push_back(h);
    }
    return joined;
}

std::u16string apply(const std::u16string& a, const std::u16string& b,
                     const std::vector<Hunk>& hunks) {
    std::u16string out = a;
    for (auto it = hunks.rbegin(); it != hunks.rend(); ++it)
        out.replace(it->oldStart, it->oldLength, b, it->newStart, it->newLength);
    return out;
}

size_t mapOffset(const std::u16string& a, const std::u16string& b,
                 const std::vector<Hunk>& hunks, size_t pos) {
    pos = std::min(pos, a.size());
    long delta = 0;
    for (const Hunk& h : hunks) {
        const size_t oldEnd = h.oldStart + h.oldLength;
        if (pos < h.oldStart) break;
        if (pos >= oldEnd) {
            delta += (long)h.newLength - (long)h.oldLength;
            continue;
        }
        // Inside the hunk: count what is not whitespace up to pos, and note
        // the line breaks and the column between the last of it and pos.
        size_t count = 0, lastEnd = h.oldStart;
        for (size_t i = h.oldStart; i < pos; i++)
            if (!isSpace(a[i])) { count++; lastEnd = i + 1; }
        size_t breaks = 0, column = 0;
        for (size_t i = lastEnd; i < pos; i++) {
            if (a[i] == u'\n') { breaks++; column = 0; }
            else column++;
        }
        const size_t end = h.newStart + h.newLength;
        size_t p = h.newStart, seen = 0;
        while (p < end && seen < count) {
            if (!isSpace(b[p])) seen++;
            p++;
        }
        if (!isSpace(a[pos])) {   // just before a character: stay before it
            while (p < end && isSpace(b[p])) p++;
            return p;
        }
        for (size_t k = 0; k < breaks; k++) {
            size_t q = p;
            while (q < end && isSpace(b[q]) && b[q] != u'\n') q++;
            if (q < end && b[q] == u'\n') p = q + 1;
            else break;
        }
        if (breaks > 0)
            for (size_t c = 0; c < column && p < end && (b[p] == u' ' || b[p] == u'\t'); c++)
                p++;
        return p;
    }
    long mapped = (long)pos + delta;
    if (mapped < 0) mapped = 0;
    return std::min((size_t)mapped, b.size());
}

}  // namespace LineDiff
