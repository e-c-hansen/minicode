// MarkdownParser.cpp — a compact CommonMark-subset parser. Pure C++.
// Supports: ATX headings, fenced code blocks, blockquotes, unordered/ordered
// lists, horizontal rules, GitHub tables, and inline **bold**, *italic*, `code`,
// [text](url), ![alt](src) and a linked image [![alt](src)](url).
#include "MarkdownParser.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

namespace {

// Split into physical lines (keeping empties, dropping trailing \r).
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    lines.push_back(cur);
    return lines;
}

std::string ltrim(const std::string& s, int& removed) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    removed = (int)i;
    return s.substr(i);
}

bool isRule(const std::string& s) {
    int count = 0; char first = 0;
    for (char c : s) {
        if (c == ' ') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (!first) first = c;
        if (c != first) return false;
        count++;
    }
    return count >= 3;
}

// Reads "![alt](src)" at `i`: fills `r` and returns the index past it, or 0.
// A title after the source ("src \"title\"") and angle brackets are dropped.
size_t parseImage(const std::string& text, size_t i, MdRun& r) {
    if (text.compare(i, 2, "![") != 0) return 0;
    const size_t close = text.find(']', i + 2);
    if (close == std::string::npos || close + 1 >= text.size() ||
        text[close + 1] != '(')
        return 0;
    const size_t end = text.find(')', close + 2);
    if (end == std::string::npos) return 0;
    std::string src = text.substr(close + 2, end - close - 2);
    const size_t a = src.find_first_not_of(' ');
    src = a == std::string::npos ? "" : src.substr(a);
    if (!src.empty() && src[0] == '<') {
        const size_t gt = src.find('>');
        src = src.substr(1, gt == std::string::npos ? std::string::npos : gt - 1);
    } else {
        src = src.substr(0, src.find(' '));
    }
    if (src.empty()) return 0;
    r.image = true;
    r.text = text.substr(i + 2, close - i - 2);
    r.src = src;
    return end + 1;
}

// Parse inline spans of one text line into runs, inheriting a template run.
void parseInline(const std::string& text, MdRun base, std::vector<MdRun>& out) {
    std::string buf;
    auto flush = [&](MdRun r) {
        if (!buf.empty()) { r.text = buf; out.push_back(r); buf.clear(); }
    };
    size_t i = 0, n = text.size();
    while (i < n) {
        char c = text[i];
        // Inline code `...`
        if (c == '`') {
            flush(base);
            size_t j = i + 1;
            while (j < n && text[j] != '`') j++;
            MdRun r = base; r.code = true;
            r.text = text.substr(i + 1, j - i - 1);
            out.push_back(r);
            i = (j < n) ? j + 1 : n;
            continue;
        }
        // Bold **...** or __...__
        if ((c == '*' || c == '_') && i + 1 < n && text[i+1] == c) {
            std::string delim(2, c);
            size_t j = text.find(delim, i + 2);
            if (j != std::string::npos) {
                flush(base);
                MdRun r = base; r.bold = true;
                r.text = text.substr(i + 2, j - i - 2);
                out.push_back(r);
                i = j + 2;
                continue;
            }
        }
        // Italic *...* or _..._
        if (c == '*' || c == '_') {
            size_t j = text.find(c, i + 1);
            if (j != std::string::npos && j > i + 1) {
                flush(base);
                MdRun r = base; r.italic = true;
                r.text = text.substr(i + 1, j - i - 1);
                out.push_back(r);
                i = j + 1;
                continue;
            }
        }
        // Image ![alt](src)
        if (c == '!') {
            MdRun r = base;
            if (size_t next = parseImage(text, i, r)) {
                flush(base);
                out.push_back(r);
                i = next;
                continue;
            }
        }
        // A linked image [![alt](src)](url), as badges are written
        if (c == '[' && i + 1 < n && text[i + 1] == '!') {
            MdRun r = base;
            size_t next = parseImage(text, i + 1, r);
            if (next && next + 1 < n && text[next] == ']' && text[next + 1] == '(') {
                const size_t urlEnd = text.find(')', next + 2);
                if (urlEnd != std::string::npos) {
                    flush(base);
                    r.link = true;
                    r.url = text.substr(next + 2, urlEnd - next - 2);
                    out.push_back(r);
                    i = urlEnd + 1;
                    continue;
                }
            }
        }
        // Link [text](url)
        if (c == '[') {
            size_t close = text.find(']', i);
            if (close != std::string::npos && close + 1 < n &&
                text[close+1] == '(') {
                size_t urlEnd = text.find(')', close + 2);
                if (urlEnd != std::string::npos) {
                    flush(base);
                    MdRun r = base; r.link = true;
                    r.text = text.substr(i + 1, close - i - 1);
                    r.url  = text.substr(close + 2, urlEnd - close - 2);
                    out.push_back(r);
                    i = urlEnd + 1;
                    continue;
                }
            }
        }
        buf.push_back(c);
        i++;
    }
    flush(base);
}

// Append a newline-only run so blocks visually separate.
void pushBreak(std::vector<MdRun>& out) {
    MdRun r; r.text = "\n"; out.push_back(r);
}

// Ensure the next run begins on a fresh line, so a block element that isn't
// preceded by a blank line does not get glued onto the previous text.
void ensureLineStart(std::vector<MdRun>& out) {
    if (out.empty()) return;
    const std::string& t = out.back().text;
    if (!t.empty() && t.back() == '\n') return;
    pushBreak(out);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Column alignment taken from a table's separator row.
enum class Align { Left, Center, Right };

// Split "| a | b |" into {"a","b"}, trimming cells and outer pipes. An escaped
// pipe (\|) is a literal character inside a cell, not a column boundary.
std::vector<std::string> splitTableRow(const std::string& line) {
    std::string s = trim(line);
    if (!s.empty() && s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|' &&
        !(s.size() >= 2 && s[s.size() - 2] == '\\'))
        s.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') { cur += '|'; i++; }
        else if (s[i] == '|') { cells.push_back(trim(cur)); cur.clear(); }
        else cur += s[i];
    }
    cells.push_back(trim(cur));
    return cells;
}

// A GitHub table separator row, e.g. "| --- | :--: | --: |". It needs at least
// one pipe, and every cell must be dashes with optional colons at either end.
// On success the per-column alignments are written to `aligns`.
bool parseTableSeparator(const std::string& line, std::vector<Align>& aligns) {
    if (line.find('|') == std::string::npos) return false;
    aligns.clear();
    for (const std::string& cell : splitTableRow(line)) {
        if (cell.empty()) return false;
        bool left = cell.front() == ':';
        bool right = cell.size() > 1 && cell.back() == ':';
        size_t a = left ? 1 : 0, b = cell.size() - (right ? 1 : 0);
        if (a >= b) return false;
        for (size_t i = a; i < b; i++)
            if (cell[i] != '-') return false;
        aligns.push_back(left && right ? Align::Center
                         : right       ? Align::Right
                                       : Align::Left);
    }
    return true;
}

// How many monospace columns a UTF-8 string occupies. Counts code points, not
// bytes; combining marks and variation selectors take no space, while East
// Asian wide characters and most emoji take two.
size_t displayWidth(const std::string& s) {
    size_t width = 0, i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80)                { cp = c;        len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                         { cp = c;        len = 1; }  // stray byte
        for (int k = 1; k < len; k++) {
            if (i + k >= n || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
                len = k; break;                 // truncated sequence
            }
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        i += len;

        if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F))
            continue;                           // zero width
        bool wide =
            (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
            (cp >= 0x20000 && cp <= 0x3FFFD);
        width += wide ? 2 : 1;
    }
    return width;
}

// Render a GitHub table as aligned monospace rows. Cells keep their inline
// styling (bold, code, links); padding is computed from the visible text, so
// markup characters that don't display don't throw the columns off.
void emitTable(const std::vector<std::vector<std::string>>& rows,
               const std::vector<Align>& aligns, int id, std::vector<MdRun>& out) {
    size_t cols = aligns.size();

    // Inline-parse every cell once, and measure what will actually show.
    std::vector<std::vector<std::vector<MdRun>>> cells(rows.size());
    std::vector<size_t> width(cols, 1);
    for (size_t ri = 0; ri < rows.size(); ri++) {
        cells[ri].resize(cols);
        for (size_t c = 0; c < cols; c++) {
            MdRun base; base.table = true; base.bold = (ri == 0);
            base.tableId = id;
            base.tableRow = (int)ri;
            base.tableCol = (int)c;
            base.tableCols = (int)cols;
            base.tableAlign = (int)aligns[c];
            if (c < rows[ri].size()) parseInline(rows[ri][c], base, cells[ri][c]);
            size_t w = 0;
            for (const MdRun& r : cells[ri][c]) w += displayWidth(r.text);
            width[c] = std::max(width[c], w);
        }
    }

    auto plain = [&](const std::string& text) {
        MdRun r; r.table = true; r.text = text;
        r.tableId = id; r.tableCols = (int)cols;
        out.push_back(r);
    };

    for (size_t ri = 0; ri < rows.size(); ri++) {
        for (size_t c = 0; c < cols; c++) {
            if (c > 0) plain(" \xE2\x94\x82 ");          // " │ "
            size_t w = 0;
            for (const MdRun& r : cells[ri][c]) w += displayWidth(r.text);
            size_t pad = width[c] - w, before = 0;
            if (aligns[c] == Align::Right) before = pad;
            else if (aligns[c] == Align::Center) before = pad / 2;
            if (before) plain(std::string(before, ' '));
            for (const MdRun& r : cells[ri][c]) out.push_back(r);
            if (pad - before) plain(std::string(pad - before, ' '));
        }
        plain("\n");
        if (ri == 0) {                                  // rule under the header
            std::string sep;
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) sep += "\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80";  // ─┼─
                for (size_t k = 0; k < width[c]; k++) sep += "\xE2\x94\x80";
            }
            plain(sep + "\n");
        }
    }
}

} // namespace

std::string MarkdownParser::anchor(const std::string& headingText) {
    std::string t = trim(headingText), out;
    for (char c : t) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80) out += c;
        else if (std::isalnum(u)) out += (char)std::tolower(u);
        else if (c == ' ') out += '-';
        else if (c == '-' || c == '_') out += c;
    }
    return out;
}

std::vector<MdRun> MarkdownParser::parse(const std::string& markdown) {
    std::vector<MdRun> out;
    auto lines = splitLines(markdown);
    bool inFence = false;
    int tables = 0;
    // Every run is stamped with the line it came from once that line (or
    // a table's first line) is done, so a GUI can map between the source
    // and the rendered text.
    size_t stamped = 0;
    int current = 0;
    auto stamp = [&] {
        for (; stamped < out.size(); stamped++) out[stamped].line = current;
    };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        stamp();
        current = (int)idx;
        const std::string& raw = lines[idx];

        // Fenced code blocks.
        int indent = 0;
        std::string trimmed = ltrim(raw, indent);
        if (trimmed.rfind("```", 0) == 0) {
            if (!inFence) ensureLineStart(out);   // opening a block
            inFence = !inFence;
            if (!inFence) pushBreak(out);         // closing
            continue;
        }
        if (inFence) {
            MdRun r; r.codeBlock = true; r.code = true;
            r.text = raw + "\n";
            out.push_back(r);
            continue;
        }

        // Blank line -> paragraph break.
        if (trimmed.empty()) { pushBreak(out); continue; }

        // Horizontal rule.
        if (isRule(trimmed)) {
            ensureLineStart(out);
            MdRun r; r.rule = true; r.text = "\n"; out.push_back(r);
            pushBreak(out);
            continue;
        }

        // GitHub table: a header line with pipes, then a separator row with
        // the same number of columns.
        if (trimmed.find('|') != std::string::npos && idx + 1 < lines.size()) {
            std::vector<Align> aligns;
            std::vector<std::string> header = splitTableRow(trimmed);
            if (parseTableSeparator(trim(lines[idx + 1]), aligns) &&
                aligns.size() == header.size()) {
                ensureLineStart(out);
                std::vector<std::vector<std::string>> rows{header};
                size_t j = idx + 2;
                while (j < lines.size()) {
                    std::string row = trim(lines[j]);
                    if (row.empty() || row.find('|') == std::string::npos) break;
                    rows.push_back(splitTableRow(row));
                    j++;
                }
                idx = j - 1;   // outer loop will ++
                emitTable(rows, aligns, ++tables, out);
                pushBreak(out);
                continue;
            }
        }

        // Headings.
        if (trimmed[0] == '#') {
            int level = 0;
            while (level < (int)trimmed.size() && trimmed[level] == '#') level++;
            if (level <= 6 && level < (int)trimmed.size() &&
                trimmed[level] == ' ') {
                ensureLineStart(out);
                MdRun base; base.heading = level;
                parseInline(trimmed.substr(level + 1), base, out);
                pushBreak(out);
                continue;
            }
        }

        // Blockquote.
        if (trimmed[0] == '>') {
            ensureLineStart(out);
            MdRun base; base.quote = true;
            std::string body = trimmed.substr(1);
            int r2; body = ltrim(body, r2);
            parseInline(body, base, out);
            pushBreak(out);
            continue;
        }

        // Unordered list.
        if ((trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
            trimmed.size() > 1 && trimmed[1] == ' ') {
            ensureLineStart(out);
            MdRun base; base.listDepth = 1 + indent / 2;
            MdRun bullet; bullet.listDepth = base.listDepth;
            bullet.text = std::string(base.listDepth * 2, ' ') + "• ";
            out.push_back(bullet);
            parseInline(trimmed.substr(2), base, out);
            pushBreak(out);
            continue;
        }

        // Ordered list.
        if (std::isdigit((unsigned char)trimmed[0])) {
            size_t p = 0;
            while (p < trimmed.size() && std::isdigit((unsigned char)trimmed[p]))
                p++;
            if (p < trimmed.size() && (trimmed[p] == '.' || trimmed[p] == ')') &&
                p + 1 < trimmed.size() && trimmed[p+1] == ' ') {
                ensureLineStart(out);
                MdRun base; base.ordered = true; base.listDepth = 1 + indent / 2;
                MdRun num; num.listDepth = base.listDepth;
                num.text = std::string(base.listDepth * 2, ' ') +
                           trimmed.substr(0, p + 1) + " ";
                out.push_back(num);
                parseInline(trimmed.substr(p + 2), base, out);
                pushBreak(out);
                continue;
            }
        }

        // Plain paragraph line.
        MdRun base;
        parseInline(trimmed, base, out);
        MdRun sp; sp.text = " "; out.push_back(sp); // soft-wrap spacing
    }
    stamp();
    return out;
}
