// MarkdownEdit.cpp — see MarkdownEdit.h. The line classification mirrors
// MarkdownParser::parse (fences, blank lines, rules, tables, headings,
// quotes, lists, paragraphs, in that order), so what the preview shows and
// what gets edited agree.
#include "MarkdownEdit.h"

#include <cctype>
#include <vector>

namespace MarkdownEdit {
namespace {

enum class LineKind { Blank, Fence, Code, Rule, TableHead, TableSep, TableRow,
                      Heading, Quote, Bullet, Numbered, Text };

struct Line {
    size_t start, end;   // bytes, without the newline (or a \r before it)
    LineKind kind = LineKind::Text;
};

std::vector<Line> splitLines(const std::string& s) {
    std::vector<Line> lines;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\n') {
            size_t e = i;
            if (e > a && s[e - 1] == '\r') e--;
            lines.push_back({a, e});
            a = i + 1;
        }
    }
    return lines;
}

size_t indentOf(const std::string& s, const Line& l) {
    size_t i = l.start;
    while (i < l.end && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

bool isRule(const std::string& t) {
    int count = 0; char first = 0;
    for (char c : t) {
        if (c == ' ') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (!first) first = c;
        if (c != first) return false;
        count++;
    }
    return count >= 3;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Cell count of a table row, as MarkdownParser splits it.
size_t cellCount(const std::string& row) {
    std::string s = trim(row);
    if (!s.empty() && s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|' && !(s.size() >= 2 && s[s.size() - 2] == '\\'))
        s.pop_back();
    size_t n = 1;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') i++;
        else if (s[i] == '|') n++;
    }
    return n;
}

bool isSeparator(const std::string& row, size_t& columns) {
    std::string s = trim(row);
    if (s.find('|') == std::string::npos) return false;
    if (s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|') s.pop_back();
    columns = 0;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find('|', a);
        if (b == std::string::npos) b = s.size();
        std::string cell = trim(s.substr(a, b - a));
        if (cell.empty()) return false;
        size_t i = 0, j = cell.size();
        if (cell[i] == ':') i++;
        if (j > i && cell[j - 1] == ':') j--;
        if (i >= j) return false;
        for (size_t k = i; k < j; k++) if (cell[k] != '-') return false;
        columns++;
        a = b + 1;
    }
    return true;
}

// Digits then '.' or ')' then a space: the length of the marker, or 0.
size_t numberedMarker(const std::string& t) {
    size_t p = 0;
    while (p < t.size() && std::isdigit((unsigned char)t[p])) p++;
    if (p == 0 || p + 1 >= t.size() || (t[p] != '.' && t[p] != ')') || t[p + 1] != ' ')
        return 0;
    return p + 2;
}

void classify(const std::string& s, std::vector<Line>& lines) {
    bool inFence = false;
    for (size_t i = 0; i < lines.size(); i++) {
        Line& l = lines[i];
        const size_t body = indentOf(s, l);
        const std::string t = s.substr(body, l.end - body);
        if (t.rfind("```", 0) == 0) { l.kind = LineKind::Fence; inFence = !inFence; continue; }
        if (inFence) { l.kind = LineKind::Code; continue; }
        if (t.empty()) { l.kind = LineKind::Blank; continue; }
        if (isRule(t)) { l.kind = LineKind::Rule; continue; }
        size_t cols = 0;
        if (t.find('|') != std::string::npos && i + 1 < lines.size() &&
            isSeparator(s.substr(lines[i + 1].start, lines[i + 1].end - lines[i + 1].start), cols) &&
            cols == cellCount(t)) {
            l.kind = LineKind::TableHead;
            lines[i + 1].kind = LineKind::TableSep;
            size_t j = i + 2;
            for (; j < lines.size(); j++) {
                const std::string row = trim(s.substr(lines[j].start, lines[j].end - lines[j].start));
                if (row.empty() || row.find('|') == std::string::npos) break;
                lines[j].kind = LineKind::TableRow;
            }
            i = j - 1;
            continue;
        }
        if (t[0] == '#') {
            size_t level = 0;
            while (level < t.size() && t[level] == '#') level++;
            if (level <= 6 && level < t.size() && t[level] == ' ') {
                l.kind = LineKind::Heading;
                continue;
            }
        }
        if (t[0] == '>') { l.kind = LineKind::Quote; continue; }
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            l.kind = LineKind::Bullet;
            continue;
        }
        if (numberedMarker(t)) { l.kind = LineKind::Numbered; continue; }
        l.kind = LineKind::Text;
    }
}

// The run of lines of `kind` around `at`.
void extent(const std::vector<Line>& lines, size_t at, LineKind kind, size_t& a, size_t& b) {
    a = b = at;
    while (a > 0 && lines[a - 1].kind == kind) a--;
    while (b + 1 < lines.size() && lines[b + 1].kind == kind) b++;
}

}  // namespace

Block blockAt(const std::string& source, int line, int column) {
    Block out;
    std::vector<Line> lines = splitLines(source);
    if (line < 0 || (size_t)line >= lines.size()) return out;
    classify(source, lines);
    const size_t at = (size_t)line;
    const Line& l = lines[at];
    const size_t body = indentOf(source, l);
    const std::string t = source.substr(body, l.end - body);
    size_t a = at, b = at;

    switch (l.kind) {
    case LineKind::Blank: case LineKind::Fence: case LineKind::Rule: case LineKind::TableSep:
        return out;
    case LineKind::Heading: {
        size_t level = 0;
        while (level < t.size() && t[level] == '#') level++;
        out.kind = Block::Heading;
        out.start = body + level + 1;
        out.end = l.end;
        break;
    }
    case LineKind::Bullet:
    case LineKind::Numbered: {
        const bool numbered = l.kind == LineKind::Numbered;
        const size_t marker = numbered ? numberedMarker(t) : 2;
        out.kind = Block::ListItem;
        out.start = body + marker;
        out.end = l.end;
        std::string indent = source.substr(l.start, body - l.start);
        if (numbered) {
            const long n = std::stol(t.substr(0, marker - 2));
            out.nextItemPrefix = indent + std::to_string(n + 1) + t[marker - 2] + " ";
        } else {
            out.nextItemPrefix = indent + t.substr(0, 2);
        }
        break;
    }
    case LineKind::Quote:
    case LineKind::Text:
    case LineKind::Code:
        extent(lines, at, l.kind, a, b);
        out.kind = l.kind == LineKind::Quote ? Block::Quote
                 : l.kind == LineKind::Code  ? Block::Code
                                             : Block::Paragraph;
        out.start = l.kind == LineKind::Code ? lines[a].start : indentOf(source, lines[a]);
        out.end = lines[b].end;
        break;
    case LineKind::TableHead:
    case LineKind::TableRow: {
        out.kind = Block::TableRow;
        out.start = body;
        out.end = l.end;
        while (out.end > out.start && std::isspace((unsigned char)source[out.end - 1])) out.end--;
        if (column < 0) break;
        // Walk the row's pipes to the column's cell, as splitTableRow does.
        size_t i = out.start, last = out.end;
        if (i < last && source[i] == '|') i++;
        if (last > i && source[last - 1] == '|' &&
            !(last - i >= 2 && source[last - 2] == '\\'))
            last--;
        int col = 0;
        size_t cellStart = i;
        for (; i <= last; i++) {
            const bool endOfCell = i == last ||
                (source[i] == '|' && !(i > cellStart && source[i - 1] == '\\'));
            if (!endOfCell) continue;
            if (col == column) {
                size_t x = cellStart, y = i;
                while (x < y && std::isspace((unsigned char)source[x])) x++;
                while (y > x && std::isspace((unsigned char)source[y - 1])) y--;
                out.kind = Block::TableCell;
                // An empty cell's text goes where a space would have been.
                if (x == y && x > cellStart) x = y = cellStart + 1;
                out.start = x;
                out.end = y;
                break;
            }
            col++;
            cellStart = i + 1;
        }
        break;
    }
    }
    out.firstLine = (int)a;
    out.lastLine = (int)b;
    return out;
}

std::string replace(const std::string& source, const Block& block, const std::string& text) {
    if (block.kind == Block::None || block.start > block.end || block.end > source.size())
        return source;
    std::string t = text;
    // A table cell holds one line, and a pipe in it would start a new column.
    if (block.kind == Block::TableCell) {
        std::string clean;
        for (char c : t) {
            if (c == '\n' || c == '\r') { clean += ' '; continue; }
            if (c == '|' && (clean.empty() || clean.back() != '\\')) clean += '\\';
            clean += c;
        }
        t = clean;
    }
    return source.substr(0, block.start) + t + source.substr(block.end);
}

std::string addItem(const std::string& source, const Block& block, const std::string& text,
                    size_t* newStart) {
    if (block.kind != Block::ListItem || block.end > source.size()) return source;
    const std::string insert = "\n" + block.nextItemPrefix + text;
    if (newStart) *newStart = block.end + 1 + block.nextItemPrefix.size();
    return source.substr(0, block.end) + insert + source.substr(block.end);
}

}  // namespace MarkdownEdit
