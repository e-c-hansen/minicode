// MarkdownParser.cpp — a compact CommonMark-subset parser. Pure C++.
// Supports: ATX headings, fenced code blocks, blockquotes, unordered/ordered
// lists, horizontal rules, and inline **bold**, *italic*, `code`, [text](url).
#include "MarkdownParser.h"
#include <algorithm>
#include <cctype>
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

// A GitHub table separator row: pipes, dashes, colons and spaces, with at
// least one dash. e.g. "| --- | :--: |".
bool isTableSeparator(const std::string& s) {
    bool dash = false, pipe = false;
    for (char c : s) {
        if (c == '-') dash = true;
        else if (c == '|') pipe = true;
        else if (c != ':' && c != ' ' && c != '\t') return false;
    }
    return dash && pipe;
}

// Split "| a | b |" into {"a","b"}, trimming cells and outer pipes.
std::vector<std::string> splitTableRow(const std::string& line) {
    std::string s = trim(line);
    if (!s.empty() && s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|') s.pop_back();
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

} // namespace

std::vector<MdRun> MarkdownParser::parse(const std::string& markdown) {
    std::vector<MdRun> out;
    auto lines = splitLines(markdown);
    bool inFence = false;

    for (size_t idx = 0; idx < lines.size(); ++idx) {
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

        // GitHub table: a "| ... |" header line followed by a separator row.
        if (trimmed.find('|') != std::string::npos && idx + 1 < lines.size()) {
            int ind2; std::string nextLine = ltrim(lines[idx + 1], ind2);
            if (isTableSeparator(nextLine)) {
                ensureLineStart(out);
                std::vector<std::vector<std::string>> rows;
                rows.push_back(splitTableRow(trimmed));
                size_t j = idx + 2;
                while (j < lines.size()) {
                    int ind3; std::string row = ltrim(lines[j], ind3);
                    if (row.empty() || row.find('|') == std::string::npos) break;
                    rows.push_back(splitTableRow(row));
                    j++;
                }
                idx = j - 1;   // outer loop will ++

                size_t cols = 0;
                for (auto& r : rows) cols = std::max(cols, r.size());
                std::vector<size_t> width(cols, 0);
                for (auto& r : rows)
                    for (size_t c = 0; c < r.size(); c++)
                        width[c] = std::max(width[c], r[c].size());

                for (size_t ri = 0; ri < rows.size(); ri++) {
                    std::string line;
                    for (size_t c = 0; c < cols; c++) {
                        std::string cell = c < rows[ri].size() ? rows[ri][c] : "";
                        if (cell.size() < width[c])
                            cell.append(width[c] - cell.size(), ' ');
                        line += cell;
                        if (c + 1 < cols) line += "  ";
                    }
                    MdRun r; r.table = true; r.text = line + "\n";
                    if (ri == 0) r.bold = true;   // header
                    out.push_back(r);
                    if (ri == 0) {                // underline under the header
                        std::string sep;
                        for (size_t c = 0; c < cols; c++) {
                            sep.append(width[c], '-');
                            if (c + 1 < cols) sep += "  ";
                        }
                        MdRun s; s.table = true; s.text = sep + "\n";
                        out.push_back(s);
                    }
                }
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
    return out;
}
