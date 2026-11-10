// BracketPairs.cpp — see BracketPairs.h. Pure C++ standard library only.
#include "BracketPairs.h"
#include <algorithm>

namespace BracketPairs {

namespace {

// Lines per block: a depth query walks the blocks before its line and the
// lines before it in its block, so about the square root of the lines a
// large file has (300,000 lines, the large-file limit, is 600 blocks). A
// block splits in two past twice this.
const size_t kBlock = 512;

// Ch is char16_t (UTF-16) or char (UTF-8, where a bracket is one byte and
// no byte of another character is one).
template <class Ch>
inline bool isOpen(Ch c) { return c == '(' || c == '[' || c == '{'; }
template <class Ch>
inline bool isClose(Ch c) { return c == ')' || c == ']' || c == '}'; }

// The stretches of the line [0, n) that are not code, sorted.
struct Skips {
    std::vector<std::pair<size_t, size_t>> spans;
    Skips(size_t n, size_t base, const Token* t, size_t count) {
        for (size_t k = 0; k < count; ++k) {
            if (!isStringStyle(t[k].style) && t[k].style != TokenStyle::Comment) continue;
            if (t[k].start + t[k].length <= base || t[k].start >= base + n) continue;
            const size_t a = t[k].start > base ? t[k].start - base : 0;
            const size_t b = std::min(n, t[k].start + t[k].length - base);
            if (b > a) spans.push_back({a, b});
        }
        std::sort(spans.begin(), spans.end());
    }
};

// Calls f(offset, isOpener) for each bracket in code on the line.
template <class Ch, class F>
void eachBracket(const Ch* s, size_t n, size_t base, const Token* t, size_t count, F&& f) {
    // Most lines have no strings or comments: no skips to build.
    bool any = false;
    for (size_t i = 0; i < n && !any; ++i) any = isOpen(s[i]) || isClose(s[i]);
    if (!any) return;
    Skips skips(n, base, t, count);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        const Ch c = s[i];
        if (!isOpen(c) && !isClose(c)) continue;
        while (k < skips.spans.size() && skips.spans[k].second <= i) ++k;
        if (k < skips.spans.size() && skips.spans[k].first <= i) continue;
        f(i, isOpen(c));
    }
}

template <class Ch>
size_t lineEnd(const IncrementalHighlighter<Ch>& hl, size_t line) {
    return line + 1 < hl.lineCount() ? hl.lineStart(line + 1) : hl.length();
}

// Calls f(line, text, length, start, tokens, count) for each lexed line of
// [first, end), with its tokens; tokens come line by line.
template <class Ch, class F>
void eachLine(const TextSource<Ch>& src, const IncrementalHighlighter<Ch>& hl, size_t first,
              size_t end, const std::vector<Token>& tokens, F&& f) {
    std::vector<Ch> buf;
    size_t k = 0;
    end = std::min(end, hl.lineCount());
    for (size_t line = first; line < end; ++line) {
        const size_t a = hl.lineStart(line), b = lineEnd(hl, line);
        while (k < tokens.size() && tokens[k].start < a) ++k;
        size_t e = k;
        while (e < tokens.size() && tokens[e].start < b) ++e;
        if (lineIsLexed(src, a, b)) {
            buf.resize(b - a);
            if (b > a) src.read(a, b - a, buf.data());
            f(line, buf.data(), b - a, a, tokens.data() + k, e - k);
        } else {
            f(line, static_cast<const Ch*>(nullptr), 0, a, static_cast<const Token*>(nullptr),
              0);
        }
        k = e;
    }
}

template <class Ch>
LineSum summarizeT(const Ch* s, size_t n, size_t base, const Token* tokens, size_t count) {
    int32_t d = 0, low = 0;
    eachBracket(s, n, base, tokens, count, [&](size_t, bool open) {
        if (open) {
            ++d;
        } else {
            --d;
            low = std::min(low, d);
        }
    });
    LineSum r;
    r.net = d;
    r.floor = d - low;
    return r;
}

template <class Ch>
int32_t colorT(const Ch* s, size_t n, size_t base, const Token* tokens, size_t count,
               int32_t depth, std::vector<Bracket>* out) {
    eachBracket(s, n, base, tokens, count, [&](size_t i, bool open) {
        if (open) {
            if (out) out->push_back({base + i, depth});
            ++depth;
        } else if (depth == 0) {
            if (out) out->push_back({base + i, -1});
        } else {
            --depth;
            if (out) out->push_back({base + i, depth});
        }
    });
    return depth;
}

template <class Ch>
std::vector<LineSum> summarizeLinesT(const TextSource<Ch>& src,
                                     const IncrementalHighlighter<Ch>& hl, size_t first,
                                     size_t end, const std::vector<Token>& tokens) {
    std::vector<LineSum> sums;
    sums.reserve(end > first ? end - first : 0);
    eachLine(src, hl, first, end, tokens,
             [&](size_t, const Ch* s, size_t n, size_t a, const Token* t, size_t c) {
                 sums.push_back(s ? summarizeT(s, n, a, t, c) : LineSum{});
             });
    return sums;
}

template <class Ch>
int32_t colorLinesT(const TextSource<Ch>& src, const IncrementalHighlighter<Ch>& hl,
                    size_t first, size_t end, const std::vector<Token>& tokens, int32_t depth,
                    std::vector<Bracket>* out) {
    eachLine(src, hl, first, end, tokens,
             [&](size_t, const Ch* s, size_t n, size_t a, const Token* t, size_t c) {
                 if (s) depth = colorT(s, n, a, t, c, depth, out);
             });
    return depth;
}

}  // namespace

LineSum LineSum::then(const LineSum& next) const {
    LineSum r;
    r.net = net + next.net;
    r.floor = std::max(floor + next.net, next.floor);
    return r;
}

LineSum summarize(const char16_t* s, size_t n, size_t base, const Token* tokens,
                  size_t count) {
    return summarizeT(s, n, base, tokens, count);
}

int32_t color(const char16_t* s, size_t n, size_t base, const Token* tokens, size_t count,
              int32_t depth, std::vector<Bracket>* out) {
    return colorT(s, n, base, tokens, count, depth, out);
}

std::vector<LineSum> summarizeLines(const TextSource<char16_t>& src,
                                    const IncrementalHighlighter<char16_t>& hl, size_t first,
                                    size_t end, const std::vector<Token>& tokens) {
    return summarizeLinesT(src, hl, first, end, tokens);
}

int32_t colorLines(const TextSource<char16_t>& src, const IncrementalHighlighter<char16_t>& hl,
                   size_t first, size_t end, const std::vector<Token>& tokens, int32_t depth,
                   std::vector<Bracket>* out) {
    return colorLinesT(src, hl, first, end, tokens, depth, out);
}

LineSum summarize(const char* s, size_t n, size_t base, const Token* tokens, size_t count) {
    return summarizeT(s, n, base, tokens, count);
}

int32_t color(const char* s, size_t n, size_t base, const Token* tokens, size_t count,
              int32_t depth, std::vector<Bracket>* out) {
    return colorT(s, n, base, tokens, count, depth, out);
}

std::vector<LineSum> summarizeLines(const TextSource<char>& src,
                                    const IncrementalHighlighter<char>& hl, size_t first,
                                    size_t end, const std::vector<Token>& tokens) {
    return summarizeLinesT(src, hl, first, end, tokens);
}

int32_t colorLines(const TextSource<char>& src, const IncrementalHighlighter<char>& hl,
                   size_t first, size_t end, const std::vector<Token>& tokens, int32_t depth,
                   std::vector<Bracket>* out) {
    return colorLinesT(src, hl, first, end, tokens, depth, out);
}

// ---------------------------------------------------------------- Depths

Depths::Depths() { blocks_.resize(1); }

void Depths::resum(Block& b) {
    LineSum s;
    for (const LineSum& l : b.lines) s = s.then(l);
    b.sum = s;
}

void Depths::assign(const std::vector<LineSum>& lines) {
    blocks_.clear();
    for (size_t i = 0; i < lines.size(); i += kBlock) {
        Block b;
        b.lines.assign(lines.begin() + i, lines.begin() + std::min(lines.size(), i + kBlock));
        resum(b);
        blocks_.push_back(std::move(b));
    }
    if (blocks_.empty()) blocks_.resize(1);
    count_ = lines.size();
}

size_t Depths::blockOf(size_t i, size_t* firstLine) const {
    size_t at = 0;
    for (size_t b = 0; b < blocks_.size(); ++b) {
        const size_t n = blocks_[b].lines.size();
        if (i < at + n || b + 1 == blocks_.size()) {
            *firstLine = at;
            return b;
        }
        at += n;
    }
    *firstLine = 0;
    return 0;
}

LineSum Depths::line(size_t i) const {
    size_t f;
    const size_t b = blockOf(i, &f);
    return i - f < blocks_[b].lines.size() ? blocks_[b].lines[i - f] : LineSum{};
}

int32_t Depths::depthAt(size_t line) const {
    int32_t d = 0;
    size_t at = 0;
    for (const Block& b : blocks_) {
        const size_t n = b.lines.size();
        if (line >= at + n) {
            d = b.sum.after(d);
            at += n;
            continue;
        }
        for (size_t k = 0; k < line - at; ++k) d = b.lines[k].after(d);
        return d;
    }
    return d;
}

bool Depths::replace(size_t first, size_t oldCount, const std::vector<LineSum>& now) {
    if (first > count_) first = count_;
    if (oldCount > count_ - first) oldCount = count_ - first;
    const int32_t before = depthAt(first);
    size_t f0;
    const size_t b0 = blockOf(first, &f0);
    // The blocks holding the replaced lines, as one run of lines.
    std::vector<LineSum> merged;
    size_t b1 = b0, covered = f0;
    do {
        merged.insert(merged.end(), blocks_[b1].lines.begin(), blocks_[b1].lines.end());
        covered += blocks_[b1].lines.size();
        ++b1;
    } while (covered < first + oldCount && b1 < blocks_.size());
    const size_t off = first - f0;
    LineSum was, is;
    for (size_t k = off; k < off + oldCount; ++k) was = was.then(merged[k]);
    for (const LineSum& l : now) is = is.then(l);
    merged.erase(merged.begin() + off, merged.begin() + off + oldCount);
    merged.insert(merged.begin() + off, now.begin(), now.end());
    // Cut back into blocks: one, unless it grew past two blocks' worth.
    std::vector<Block> pieces;
    const size_t parts = merged.size() > 2 * kBlock ? (merged.size() + kBlock - 1) / kBlock : 1;
    for (size_t p = 0; p < parts; ++p) {
        const size_t a = merged.size() * p / parts, b = merged.size() * (p + 1) / parts;
        Block blk;
        blk.lines.assign(merged.begin() + a, merged.begin() + b);
        resum(blk);
        pieces.push_back(std::move(blk));
    }
    blocks_.erase(blocks_.begin() + b0, blocks_.begin() + b1);
    // An empty block goes, unless it is the only one.
    if (pieces.size() == 1 && pieces[0].lines.empty() && !blocks_.empty()) pieces.clear();
    blocks_.insert(blocks_.begin() + b0, pieces.begin(), pieces.end());
    if (blocks_.empty()) blocks_.resize(1);
    count_ = count_ - oldCount + now.size();
    return was.after(before) != is.after(before);
}

bool PaintedEdits::add(size_t at, size_t oldLen, size_t newLen, bool cut) {
    const long long change = (long long)newLen - (long long)oldLen;
    size_t original;
    if (empty()) {
        original = at;
        back_.push_back({at, oldLen, newLen});
        low_ = at;
        highEnd_ = at + newLen;
    } else if (at + oldLen <= low_) {   // before them all: the old text's place
        original = at;
        front_.push_back({at, oldLen, newLen});
        low_ = at;
        highEnd_ = (size_t)((long long)highEnd_ + change);
    } else if (at >= highEnd_) {        // after them all
        original = (size_t)((long long)at - delta_);
        back_.push_back({original, oldLen, newLen});
        highEnd_ = at + newLen;
    } else {
        return false;
    }
    delta_ += change;
    if (cut) cut_ = std::min(cut_, original);
    return true;
}

void PaintedEdits::clear() {
    front_.clear();
    back_.clear();
    low_ = highEnd_ = 0;
    delta_ = 0;
    cut_ = SIZE_MAX;
}

std::vector<std::pair<size_t, size_t>> PaintedEdits::apply(
    const std::vector<std::pair<size_t, size_t>>& spans) const {
    std::vector<Edit> edits(front_.rbegin(), front_.rend());
    edits.insert(edits.end(), back_.begin(), back_.end());
    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(spans.size() + edits.size());
    auto emit = [&](size_t a, size_t b, long long shift) {
        if (b <= a) return;
        const size_t s = (size_t)((long long)a + shift), e = (size_t)((long long)b + shift);
        if (!out.empty() && out.back().second >= s) out.back().second = std::max(out.back().second, e);
        else out.push_back({s, e});
    };
    const size_t n = edits.size();
    size_t k = 0;          // the first edit not wholly before the span
    long long shift = 0;   // what the edits before k moved the text by
    for (auto span : spans) {
        if (span.first >= cut_) break;
        span.second = std::min(span.second, cut_);
        while (k < n && edits[k].at + edits[k].oldLen <= span.first) {
            shift += (long long)edits[k].newLen - (long long)edits[k].oldLen;
            ++k;
        }
        // Through the edits starting in it (and one it starts inside).
        size_t at = span.first, j = k;
        long long sh = shift;
        while (at < span.second) {
            if (j < n && edits[j].at < span.second) {
                emit(at, std::min(edits[j].at, span.second), sh);
                at = std::max(at, edits[j].at + edits[j].oldLen);
                sh += (long long)edits[j].newLen - (long long)edits[j].oldLen;
                ++j;
            } else {
                emit(at, span.second, sh);
                at = span.second;
            }
        }
    }
    return out;
}

}  // namespace BracketPairs
