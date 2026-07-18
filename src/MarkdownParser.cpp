// MarkdownParser.cpp — a compact CommonMark-subset parser. Pure C++.
// Supports: ATX headings, fenced code blocks, blockquotes, unordered/ordered
// lists, horizontal rules, and inline **bold**, *italic*, `code`, [text](url).
#include "MarkdownParser.h"
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
            inFence = !inFence;
            if (!inFence) pushBreak(out);
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
            MdRun r; r.rule = true; r.text = "\n"; out.push_back(r);
            pushBreak(out);
            continue;
        }

        // Headings.
        if (trimmed[0] == '#') {
            int level = 0;
            while (level < (int)trimmed.size() && trimmed[level] == '#') level++;
            if (level <= 6 && level < (int)trimmed.size() &&
                trimmed[level] == ' ') {
                MdRun base; base.heading = level;
                parseInline(trimmed.substr(level + 1), base, out);
                pushBreak(out);
                continue;
            }
        }

        // Blockquote.
        if (trimmed[0] == '>') {
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
