// MarkdownEdit.cpp — see MarkdownEdit.h. Which lines make up a block comes
// from MarkdownParser::lines, the parser's own reading of the source, so what
// the preview shows and what gets edited agree.
#include "MarkdownEdit.h"
#include "MarkdownParser.h"

#include <cctype>
#include <vector>

namespace MarkdownEdit {
namespace {

// Digits then '.' or ')': the marker's length, or 0.
size_t numberedMarker(const std::string& t) {
    size_t p = 0;
    while (p < t.size() && std::isdigit((unsigned char)t[p])) p++;
    if (p == 0 || p >= t.size() || (t[p] != '.' && t[p] != ')')) return 0;
    return p + 1;
}

}  // namespace

Block blockAt(const std::string& source, int line, int column) {
    Block out;
    const std::vector<MdLine> lines = MarkdownParser::lines(source);
    if (line < 0 || (size_t)line >= lines.size()) return out;
    const size_t at = (size_t)line;
    const MdLine& l = lines[at];
    size_t a = at, b = at;

    switch (l.kind) {
    case MdLine::Blank: case MdLine::Fence: case MdLine::Rule: case MdLine::TableSep:
    case MdLine::SetextUnderline: case MdLine::Hidden:
        return out;
    case MdLine::TableHead:
    case MdLine::TableRow: {
        out.kind = Block::TableRow;
        out.start = l.start;
        out.end = l.lineEnd;
        while (out.end > out.start && std::isspace((unsigned char)source[out.end - 1])) out.end--;
        if (column < 0) break;
        // Walk the row's pipes to the column's cell, as the parser splits it.
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
    default: {
        if (l.block < 0) return out;
        while (a > 0 && lines[a - 1].block == l.block) a--;
        while (b + 1 < lines.size() && lines[b + 1].block == l.block) b++;
        const MdLine& first = lines[a];
        switch (first.kind) {
        case MdLine::Heading:
            out.kind = Block::Heading;
            out.start = first.start;
            out.end = lines[b].end;
            break;
        case MdLine::ListItem: {
            out.kind = Block::ListItem;
            out.start = first.start;
            out.end = lines[b].lineEnd;
            // The next item's marker: this one's indentation and marker, a
            // number one higher, and an empty box after a task.
            const std::string lead =
                source.substr(first.lineStart, first.start - first.lineStart);
            size_t body = 0;
            while (body < lead.size() && (lead[body] == ' ' || lead[body] == '\t')) body++;
            const std::string t = lead.substr(body);
            const size_t marker = numberedMarker(t);
            if (marker) {
                const long n = std::stol(t.substr(0, marker - 1));
                out.nextItemPrefix = lead.substr(0, body) + std::to_string(n + 1) +
                                     t[marker - 1] + " ";
            } else {
                out.nextItemPrefix = lead.substr(0, body) + t.substr(0, 1) + " ";
            }
            const std::string text = source.substr(out.start, 4);
            if (text == "[ ] " || text == "[x] " || text == "[X] ") out.nextItemPrefix += "[ ] ";
            break;
        }
        case MdLine::Quote:
            out.kind = Block::Quote;
            out.start = first.start;
            out.end = lines[b].lineEnd;
            break;
        case MdLine::Code:
            out.kind = Block::Code;
            out.start = first.lineStart;
            out.end = lines[b].lineEnd;
            break;
        case MdLine::Math:
            out.kind = Block::Math;
            out.start = first.start;
            out.end = lines[b].lineEnd;
            break;
        case MdLine::Text:
            out.kind = Block::Paragraph;
            out.start = first.start;
            out.end = lines[b].lineEnd;
            break;
        default:
            return out;
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
