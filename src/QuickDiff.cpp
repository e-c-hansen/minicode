// QuickDiff.cpp — see QuickDiff.h.
#include "QuickDiff.h"
#include "LineDiff.h"

#include <algorithm>

namespace QuickDiff {

namespace {

// A line ends after "\n", U+2029, or a "\r" not before "\n", as the
// editor's LineIndex counts them (an old Mac file's lone "\r" too).
bool breakAt(const std::u16string& s, size_t i) {
    const char16_t c = s[i];
    return c == u'\n' || c == 0x2029 || (c == u'\r' && (i + 1 >= s.size() || s[i + 1] != u'\n'));
}

// Where each line starts, the editor's way: one more entry than there are
// line breaks, then the text's end.
std::vector<size_t> lineStarts(const std::u16string& s) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < s.size(); i++)
        if (breakAt(s, i)) starts.push_back(i + 1);
    starts.push_back(s.size());
    return starts;
}

size_t lineCount(const std::vector<size_t>& starts) { return starts.size() - 1; }

// The text LineDiff compares: "\r\n" as "\n", and a "\n" after the last
// line, so the text after the last break (empty after a final newline) is
// a line of its own, as the editor shows it.
std::u16string comparable(const std::u16string& s) {
    std::u16string out;
    out.reserve(s.size() + 1);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n') continue;
        out += breakAt(s, i) ? u'\n' : s[i];
    }
    out += u'\n';
    return out;
}

// The length of the break ending line `i` (0 for the last line).
size_t breakLength(const std::u16string& s, const std::vector<size_t>& starts, size_t i) {
    if (i + 1 >= lineCount(starts)) return 0;
    const size_t end = starts[i + 1];
    return end >= 2 && s[end - 1] == u'\n' && s[end - 2] == u'\r' ? 2 : 1;
}

// The UTF-16 range of lines [line, line + n) of a text, with the breaks
// after them; when they reach the end of the text (and are not the first
// lines) the break before them instead, since the last line has none and
// the run being replaced on the other side may.
struct Range { size_t start, end; };
Range linesRange(const std::u16string& s, const std::vector<size_t>& starts, size_t line,
                 size_t n, bool atEnd) {
    const size_t count = lineCount(starts);
    line = std::min(line, count);
    const size_t last = std::min(line + n, count);
    Range r{starts[line], last >= count ? s.size() : starts[last]};
    if (atEnd && line > 0) r.start = starts[line] - breakLength(s, starts, line - 1);
    return r;
}

// Whether the bar runs to the end of both texts.
bool reachesEnd(const Bar& b, const std::vector<size_t>& oldStarts,
                const std::vector<size_t>& newStarts) {
    return b.line + b.lines >= lineCount(newStarts) && b.oldLine + b.oldLines >= lineCount(oldStarts);
}

}  // namespace

std::vector<Bar> bars(const std::u16string& original, const std::u16string& buffer) {
    std::vector<Bar> out;
    const std::u16string a = comparable(original), b = comparable(buffer);
    if (a == b) return out;
    for (const LineDiff::Hunk& h : LineDiff::diff(a, b, 40000, 0.25, false)) {
        Bar bar;
        bar.line = h.newLine;
        bar.lines = h.newLines;
        bar.oldLine = h.oldLine;
        bar.oldLines = h.oldLines;
        bar.kind = h.oldLines == 0 ? Kind::Added : h.newLines == 0 ? Kind::Deleted : Kind::Modified;
        out.push_back(bar);
    }
    return out;
}

size_t anchorLine(const Bar& b) {
    if (b.kind == Kind::Deleted) return b.line ? b.line - 1 : 0;
    return b.line;
}

int barAt(const std::vector<Bar>& bars, size_t line) {
    for (size_t i = 0; i < bars.size(); i++) {
        const Bar& b = bars[i];
        if (b.kind != Kind::Deleted && line >= b.line && line < b.line + b.lines) return (int)i;
    }
    for (size_t i = 0; i < bars.size(); i++)
        if (bars[i].kind == Kind::Deleted && anchorLine(bars[i]) == line) return (int)i;
    return -1;
}

int nextBar(const std::vector<Bar>& bars, size_t caretLine) {
    if (bars.empty()) return -1;
    for (size_t i = 0; i < bars.size(); i++)
        if (anchorLine(bars[i]) > caretLine) return (int)i;
    return 0;
}

int previousBar(const std::vector<Bar>& bars, size_t caretLine) {
    if (bars.empty()) return -1;
    for (size_t i = bars.size(); i-- > 0;)
        if (anchorLine(bars[i]) < caretLine) return (int)i;
    return (int)bars.size() - 1;
}

void shift(std::vector<Bar>& bars, size_t line, size_t removed, size_t added) {
    if (removed == added) return;
    const long delta = (long)added - (long)removed;
    const size_t editEnd = line + removed;   // the last line the edit touched, before it
    for (Bar& b : bars) {
        const size_t end = b.lines ? b.line + b.lines - 1 : b.line;
        if (b.line > editEnd) {
            b.line = (size_t)std::max(0L, (long)b.line + delta);
        } else if (b.kind != Kind::Deleted && end >= line) {
            // The edit is in this bar: it keeps its start and its size changes.
            b.lines = (size_t)std::max(1L, (long)b.lines + delta);
        }
    }
}

Edit revertEdit(const Bar& b, const std::u16string& original, const std::u16string& buffer) {
    const std::vector<size_t> os = lineStarts(original), ns = lineStarts(buffer);
    const bool atEnd = reachesEnd(b, os, ns);
    const Range from = linesRange(buffer, ns, b.line, b.lines, atEnd);
    const Range to = linesRange(original, os, b.oldLine, b.oldLines, atEnd);
    Edit e;
    e.start = from.start;
    e.length = from.end - from.start;
    e.text = original.substr(to.start, to.end - to.start);
    return e;
}

std::u16string stagedText(const Bar& b, const std::u16string& original,
                          const std::u16string& buffer) {
    const std::vector<size_t> os = lineStarts(original), ns = lineStarts(buffer);
    const bool atEnd = reachesEnd(b, os, ns);
    const Range keep = linesRange(original, os, b.oldLine, b.oldLines, atEnd);
    const Range take = linesRange(buffer, ns, b.line, b.lines, atEnd);
    std::u16string piece = buffer.substr(take.start, take.end - take.start);
    // The original's line endings, when it has one kind.
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < original.size(); i++)
        if (original[i] == u'\n') (i && original[i - 1] == u'\r' ? crlf : lf)++;
    if (crlf + lf) {
        std::u16string fixed;
        for (size_t i = 0; i < piece.size(); i++) {
            const char16_t c = piece[i];
            if (c == u'\r' && i + 1 < piece.size() && piece[i + 1] == u'\n') continue;
            if (c == u'\n' && crlf && !lf) fixed += u'\r';
            fixed += c;
        }
        if (!(crlf && lf)) piece = fixed;
    }
    return original.substr(0, keep.start) + piece + original.substr(keep.end);
}

std::string describe(const Bar& b) {
    auto lines = [](size_t n) { return std::to_string(n) + (n == 1 ? " line" : " lines"); };
    switch (b.kind) {
    case Kind::Added: return lines(b.lines) + " added";
    case Kind::Deleted: return lines(b.oldLines) + " deleted";
    case Kind::Modified:
        if (b.lines == b.oldLines) return lines(b.lines) + " changed";
        return lines(b.oldLines) + " changed to " + std::to_string(b.lines);
    }
    return "";
}

std::vector<std::u16string> oldLines(const Bar& b, const std::u16string& original) {
    std::vector<std::u16string> out;
    const std::vector<size_t> os = lineStarts(original);
    for (size_t i = b.oldLine; i < b.oldLine + b.oldLines && i < lineCount(os); i++) {
        size_t end = os[i + 1];
        if (end > os[i] && breakAt(original, end - 1)) {
            const bool crlf = original[end - 1] == u'\n' && end - 1 > os[i] &&
                              original[end - 2] == u'\r';
            end -= crlf ? 2 : 1;
        }
        out.push_back(original.substr(os[i], end - os[i]));
    }
    return out;
}

bool stripBom(std::u16string& original, bool bufferBom) {
    if (!bufferBom || original.empty() || original[0] != 0xFEFF) return false;
    original.erase(0, 1);
    return true;
}

void restoreBom(std::u16string& staged, bool stripped) {
    if (stripped) staged.insert(staged.begin(), (char16_t)0xFEFF);
}

std::u16string fromUtf8(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (ok && ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                   (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000)))
            ok = false;
        if (!ok) { out += u'�'; i++; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
        i += len;
    }
    return out;
}

std::string toUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace QuickDiff
