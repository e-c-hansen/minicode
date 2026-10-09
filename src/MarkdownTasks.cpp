// MarkdownTasks.cpp — see MarkdownTasks.h.
#include "MarkdownTasks.h"
#include "MarkdownParser.h"

#include <algorithm>
#include <cstdint>

namespace MarkdownTasks {
namespace {

// How a line begins, read the way the parser reads a list item: indentation
// and block quote markers, then a list marker and its box. Everything here is
// ASCII, so one template serves UTF-8 and UTF-16 lines alike.
struct Prefix {
    bool list = false;
    bool ordered = false;
    long number = 0;
    size_t contentStart = 0;   // after indentation and quote markers
    size_t markerStart = 0, markerEnd = 0;
    size_t textStart = 0;      // after the marker's spaces, and the box
    bool task = false, checked = false;
    size_t mark = 0;           // the character between the box's brackets
    bool empty = false;        // nothing but spaces after the marker and box
};

template <class S>
bool isSpace(const S& s, size_t i) { return i < s.size() && (s[i] == ' ' || s[i] == '\t'); }

template <class S>
Prefix readPrefix(const S& s) {
    Prefix p;
    size_t i = 0;
    while (isSpace(s, i)) i++;
    while (i < s.size() && s[i] == '>') {
        i++;
        while (isSpace(s, i)) i++;
    }
    p.contentStart = i;
    p.markerStart = i;
    size_t k = i;
    if (k < s.size() && (s[k] == '-' || s[k] == '*' || s[k] == '+')) {
        k++;
    } else {
        long n = 0;
        size_t digits = 0;
        while (k < s.size() && digits < 9 && s[k] >= '0' && s[k] <= '9') {
            n = n * 10 + (long)(s[k] - '0');
            k++;
            digits++;
        }
        if (digits == 0 || k >= s.size() || (s[k] != '.' && s[k] != ')')) return p;
        p.ordered = true;
        p.number = n;
        k++;
    }
    if (k < s.size() && !isSpace(s, k)) return p;
    p.list = true;
    p.markerEnd = k;
    size_t spaces = 0;
    while (isSpace(s, k + spaces)) spaces++;
    // Five or more spaces start indented code inside the item; its text
    // starts one space after the marker.
    p.textStart = k + (spaces == 0 ? 0 : spaces >= 5 ? 1 : spaces);
    if (k + spaces >= s.size()) p.textStart = s.size();
    size_t t = p.textStart;
    if (t + 2 < s.size() && s[t] == '[' && s[t + 2] == ']' &&
        (s[t + 1] == ' ' || s[t + 1] == 'x' || s[t + 1] == 'X') &&
        (t + 3 == s.size() || isSpace(s, t + 3))) {
        p.task = true;
        p.checked = s[t + 1] != ' ';
        p.mark = t + 1;
        p.textStart = t + 3 < s.size() ? t + 4 : t + 3;
    }
    size_t rest = p.textStart;
    while (isSpace(s, rest)) rest++;
    p.empty = rest >= s.size();
    return p;
}

// A line the parser might read as a list item: its own marker line, or a
// line of a block quote (which the parser does not split into items).
bool itemLine(MdLine::Kind k) { return k == MdLine::ListItem || k == MdLine::Quote; }

std::string toUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// [start, end) of every line of a UTF-16 text, split at '\n' as the parser
// splits, with a '\r' before it left out.
struct Span { size_t start, end; };
std::vector<Span> splitLines(const std::u16string& t) {
    std::vector<Span> out;
    size_t a = 0;
    for (size_t i = 0; i <= t.size(); i++) {
        if (i == t.size() || t[i] == u'\n') {
            size_t e = i;
            if (e > a && t[e - 1] == u'\r') e--;
            out.push_back({a, e});
            a = i + 1;
        }
    }
    return out;
}

size_t lineOf(const std::vector<Span>& lines, size_t pos) {
    size_t lo = 0, hi = lines.size();
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (lines[mid].start <= pos) lo = mid; else hi = mid;
    }
    return lo;
}

// After `pos`, how far a position moves once `len` units at `at` are
// replaced by `add` units.
size_t shift(size_t pos, size_t at, size_t len, size_t add) {
    if (pos <= at) return pos;
    if (pos >= at + len) return pos - len + add;
    return at + add;
}

}  // namespace

Box boxOnLine(const std::string& source, int line) {
    Box b;
    if (line < 0) return b;
    const std::vector<MdLine> lines = MarkdownParser::lines(source);
    if ((size_t)line >= lines.size() || !itemLine(lines[(size_t)line].kind)) return b;
    const MdLine& L = lines[(size_t)line];
    const Prefix p = readPrefix(source.substr(L.lineStart, L.lineEnd - L.lineStart));
    if (!p.list || !p.task) return b;
    b.found = true;
    b.checked = p.checked;
    b.mark = L.lineStart + p.mark;
    return b;
}

std::string toggleBox(const std::string& source, int line, size_t* mark) {
    const Box b = boxOnLine(source, line);
    if (!b.found) return source;
    std::string out = source;
    out[b.mark] = b.checked ? ' ' : 'x';
    if (mark) *mark = b.mark;
    return out;
}

Counts count(const std::string& source) {
    Counts c;
    for (const MdLine& L : MarkdownParser::lines(source)) {
        if (!itemLine(L.kind)) continue;
        const Prefix p = readPrefix(source.substr(L.lineStart, L.lineEnd - L.lineStart));
        if (!p.list || !p.task) continue;
        c.total++;
        if (p.checked) c.done++;
    }
    return c;
}

std::vector<OpenTask> openTasks(const std::string& source) {
    std::vector<OpenTask> out;
    const std::vector<MdLine> lines = MarkdownParser::lines(source);
    for (size_t i = 0; i < lines.size(); i++) {
        const MdLine& L = lines[i];
        if (!itemLine(L.kind)) continue;
        const Prefix p = readPrefix(source.substr(L.lineStart, L.lineEnd - L.lineStart));
        if (p.list && p.task && !p.checked) out.push_back({(int)i, p.mark - 1});
    }
    return out;
}

Edit toggle(const std::u16string& text, size_t selStart, size_t selEnd) {
    Edit e;
    if (selStart > selEnd) std::swap(selStart, selEnd);
    selStart = std::min(selStart, text.size());
    selEnd = std::min(selEnd, text.size());
    const std::vector<Span> spans = splitLines(text);
    const std::vector<MdLine> kinds = MarkdownParser::lines(toUtf8(text));
    auto kindOf = [&](size_t i) {
        return i < kinds.size() ? kinds[i].kind : MdLine::Blank;
    };

    size_t first = lineOf(spans, selStart), last = lineOf(spans, selEnd);
    if (last > first && spans[last].start == selEnd) last--;

    // The lines to act on: a continuation line stands for its item's line.
    std::vector<size_t> targets;
    bool anyBlank = false;
    for (size_t i = first; i <= last; i++) {
        size_t t = i;
        const MdLine::Kind k = kindOf(i);
        if (k == MdLine::Blank) { anyBlank = true; continue; }
        if (k == MdLine::Text && i < kinds.size() && kinds[i].block >= 0) {
            for (size_t j = i; j-- > 0;) {
                if (kindOf(j) == MdLine::ListItem && kinds[j].block == kinds[i].block) {
                    t = j;
                    break;
                }
                if (kinds[j].block != kinds[i].block) break;
            }
        }
        const MdLine::Kind tk = kindOf(t);
        if (tk != MdLine::ListItem && tk != MdLine::Quote && tk != MdLine::Text) continue;
        if (targets.empty() || targets.back() != t) targets.push_back(t);
    }
    // One blank line on its own becomes an empty task.
    if (targets.empty()) {
        if (!(anyBlank && first == last)) return e;
        const Span s = spans[first];
        const std::u16string add = u"- [ ] ";
        e.changed = true;
        e.replaceStart = s.end;
        e.replaceLength = 0;
        e.replacement = add;
        e.selStart = e.selEnd = s.end + add.size();
        return e;
    }

    struct LineEdit { size_t at, len; std::u16string add; };
    std::vector<LineEdit> edits;
    std::vector<Prefix> prefixes;
    bool allTasks = true, allChecked = true;
    for (size_t t : targets) {
        const Span s = spans[t];
        Prefix p = readPrefix(text.substr(s.start, s.end - s.start));
        prefixes.push_back(p);
        if (!p.list || !p.task) allTasks = false;
        else if (!p.checked) allChecked = false;
    }
    for (size_t n = 0; n < targets.size(); n++) {
        const Span s = spans[targets[n]];
        const Prefix& p = prefixes[n];
        if (allTasks) {
            edits.push_back({s.start + p.mark, 1, allChecked ? u" " : u"x"});
        } else if (!p.list) {
            // "[ ] text" written without a list marker already has its box:
            // it gets the marker only, keeping the box and whether it was
            // ticked, rather than a second box in front of it.
            const std::u16string line = text.substr(s.start, s.end - s.start);
            const size_t c = p.contentStart;
            const bool hasBox = c + 2 < line.size() && line[c] == u'[' && line[c + 2] == u']' &&
                                (line[c + 1] == u' ' || line[c + 1] == u'x' || line[c + 1] == u'X') &&
                                (c + 3 == line.size() || isSpace(line, c + 3));
            edits.push_back({s.start + p.contentStart, 0, hasBox ? u"- " : u"- [ ] "});
        } else if (!p.task) {
            // After the marker and one space; a marker with no text gets one.
            if (p.textStart > p.markerEnd)
                edits.push_back({s.start + p.textStart, 0, u"[ ] "});
            else
                edits.push_back({s.start + p.markerEnd, 0, u" [ ] "});
        }
    }
    if (edits.empty()) return e;

    // One replacement from the first edit to the end of the last.
    const size_t from = edits.front().at;
    const size_t to = edits.back().at + edits.back().len;
    std::u16string rep;
    size_t pos = from;
    for (const LineEdit& le : edits) {
        rep.append(text, pos, le.at - pos);
        rep += le.add;
        pos = le.at + le.len;
    }
    e.changed = true;
    e.replaceStart = from;
    e.replaceLength = to - from;
    e.replacement = rep;
    // The selection follows the text it was on.
    size_t a = selStart, b = selEnd;
    size_t moved = 0;
    for (const LineEdit& le : edits) {
        const size_t at = le.at + moved;
        a = shift(a, at, le.len, le.add.size());
        b = shift(b, at, le.len, le.add.size());
        if (selStart == selEnd && a == at && le.len == 0) a = b = at + le.add.size();
        moved += le.add.size() - le.len;
    }
    e.selStart = a;
    e.selEnd = b;
    return e;
}

Edit newline(const std::u16string& text, size_t selStart, size_t selEnd) {
    Edit e;
    if (selStart > selEnd) std::swap(selStart, selEnd);
    if (selEnd > text.size()) return e;
    const std::vector<Span> spans = splitLines(text);
    const size_t li = lineOf(spans, selStart);
    if (lineOf(spans, selEnd) != li) return e;
    const Span s = spans[li];
    const Prefix p = readPrefix(text.substr(s.start, s.end - s.start));
    if (!p.list || selStart < s.start + p.textStart) return e;
    // Only where the parser reads a list item, never in code.
    const std::vector<MdLine> kinds = MarkdownParser::lines(toUtf8(text));
    if (li >= kinds.size() || !itemLine(kinds[li].kind)) return e;

    if (p.empty) {
        // Return on an empty item ends the list.
        e.changed = true;
        e.replaceStart = s.start;
        e.replaceLength = s.end - s.start;
        e.selStart = e.selEnd = s.start;
        return e;
    }
    std::u16string add = u"\n";
    add.append(text, s.start, p.markerStart);
    if (p.ordered) {
        for (char c : std::to_string(p.number + 1)) add += (char16_t)c;
        add += text[s.start + p.markerEnd - 1];
    } else {
        add += text[s.start + p.markerStart];
    }
    // The spaces after the marker as written, so the text stays aligned.
    const size_t gapEnd = p.task ? p.mark - 1 : p.textStart;
    if (gapEnd > p.markerEnd) add.append(text, s.start + p.markerEnd, gapEnd - p.markerEnd);
    else add += u' ';
    if (p.task) add += u"[ ] ";
    e.changed = true;
    e.replaceStart = selStart;
    e.replaceLength = selEnd - selStart;
    e.replacement = add;
    e.selStart = e.selEnd = selStart + add.size();
    return e;
}

}  // namespace MarkdownTasks
