// TerminalScreen.cpp — see TerminalScreen.h. Behavior follows xterm where the
// details matter to real programs (pending wrap, erase with the current
// background, cursor movement clamped by the scroll region).
#include "TerminalScreen.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

std::string utf8Encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// DEC Special Graphics ("ESC ( 0"), the line-drawing set curses falls back
// on: 0x5F-0x7E map to these code points.
const uint32_t kDecGraphics[32] = {
    0x0020, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

// A packed scrollback cell (HistLine::cells): its offset into the line's
// text above two bits of width. Offsets reach 1 GiB, and nothing is packed
// past kMaxLineBytes (pack and appendCells stop short of it), so a line
// joined from many rows in a reflow can never wrap the offset around. With
// eight bits of width, 16 MiB of combining marks on one line used to wrap
// it, and find read far outside the text.
const size_t kMaxLineBytes = (size_t(1) << 30) - 1;
inline uint32_t packCell(size_t offset, int width) { return (uint32_t)offset << 2 | (uint32_t)width; }
inline size_t cellOffset(uint32_t c) { return c >> 2; }
inline int cellWidth(uint32_t c) { return (int)(c & 3); }

// The code points of a cell's text (or a query), appended to `out`. Bytes
// that are not UTF-8 come out as U+FFFD, one each.
void utf8Decode(const char* p, size_t n, std::vector<uint32_t>& out) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        int need = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2
                 : (c >> 3) == 30 ? 3 : -1;
        uint32_t cp = need == 0 ? c : need == 1 ? (c & 0x1F)
                    : need == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = need >= 0 && i + need < n;
        for (int k = 1; ok && k <= need; k++) {
            unsigned char d = (unsigned char)p[i + k];
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = cp << 6 | (d & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); i++; continue; }
        out.push_back(cp);
        i += need + 1;
    }
}

// Case folding for find: the letters of ASCII, Latin-1, Greek and Cyrillic.
uint32_t foldCase(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp < 0xC0) return cp;
    if (cp <= 0xDE && cp != 0xD7) return cp + 32;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
    return cp;
}

} // namespace

struct TerminalScreen::Sink : TermParser::Handler {
    TerminalScreen& s;
    explicit Sink(TerminalScreen& s) : s(s) {}
    void print(uint32_t cp, const std::string& u) override { s.print(cp, u); }
    void control(unsigned char c) override { s.control(c); }
    void csi(const std::string& p, unsigned char f) override { s.csi(p, f); }
    void esc(const std::string& i, unsigned char f) override { s.esc(i, f); }
    void osc(const std::string& b) override { s.osc(b); }
};

TerminalScreen::TerminalScreen(int cols, int rows)
    : cols_(std::max(cols, 2)), rows_(std::max(rows, 1)) {
    fullReset();
}

void TerminalScreen::fullReset() {
    if (syncOutput_) syncEnds_++;
    below_.clear();
    main_.assign(rows_, Row(cols_, TermCell()));
    alt_.assign(rows_, Row(cols_, TermCell()));
    onAlt_ = false;
    x_ = y_ = 0;
    wrapPending_ = false;
    style_ = TermStyle();
    link_ = 0;          // the table stays: the scrollback's cells use it
    top_ = 0;
    bottom_ = rows_ - 1;
    savedMain_ = savedAlt_ = Saved();
    autowrap_ = true;
    originMode_ = insertMode_ = false;
    cursorVisible_ = true;
    appCursor_ = appKeypad_ = bracketedPaste_ = syncOutput_ = false;
    resetMouseModes();
    promptSp_ = false;
    g0_ = g1_ = gl_ = 0;
    lastChar_.clear();
    resetTabs();
}

void TerminalScreen::resetTabs() {
    tabs_.assign(cols_, false);
    for (int i = 8; i < cols_; i += 8) tabs_[i] = true;
}

TermCell TerminalScreen::blank() const {
    TermCell c;
    c.style.bg = style_.bg;           // erase uses the current background (BCE)
    return c;
}

void TerminalScreen::feed(const char* data, size_t length) {
    Sink sink(*this);
    parser_.feed(data, length, sink);
}

std::string TerminalScreen::takeReplies() {
    std::string r;
    r.swap(replies_);
    return r;
}

// ------------------------------------------------------------------ text
std::string TerminalScreen::rowText(int row) const {
    std::string s;
    size_t keep = 0;
    for (const TermCell& c : grid()[row]) {
        s += c.ch;
        if (c.ch != " ") keep = s.size();
    }
    s.resize(keep);
    return s;
}

std::string TerminalScreen::text() const {
    std::vector<std::string> lines;
    for (int r = 0; r < rows_; r++) lines.push_back(rowText(r));
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) s += (i ? "\n" : "") + lines[i];
    return s;
}

std::string TerminalScreen::textBetween(int r0, int c0, int r1, int c1) const {
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    int firstLine = -historySize();
    r0 = clampInt(r0, firstLine, rows_ - 1);
    r1 = clampInt(r1, firstLine, rows_ - 1);
    std::string out;
    for (int r = r0; r <= r1; r++) {
        // A row that wrapped onto the next is joined to it, its blanks kept
        // (they are the text's), less a wide character's leftover blank.
        bool soft = r < r1 && lineWraps(r);
        int width = soft ? lineTextWidth(r) : lineWidth(r);
        int from = r == r0 ? clampInt(c0, 0, cols_ - 1) : 0;
        int to = r == r1 && c1 < cols_ - 1 ? clampInt(c1, 0, cols_ - 1) : width - 1;
        std::string line;
        size_t keep = 0;
        for (int c = from; c <= to; c++) {
            const TermCell& cell = lineCell(r, c);
            line += cell.ch;
            if (soft || cell.ch != " ") keep = line.size();
        }
        line.resize(keep);
        out += line;
        if (r < r1 && !soft) out += "\n";
    }
    return out;
}

// ------------------------------------------------------------------- find
std::vector<TermMatch> TerminalScreen::find(const std::string& query, bool matchCase,
                                            size_t limit) const {
    std::vector<TermMatch> found;
    std::vector<uint32_t> q;
    utf8Decode(query.data(), query.size(), q);
    if (q.empty() || limit == 0) return found;
    if (!matchCase)
        for (uint32_t& cp : q) cp = foldCase(cp);
    // One line at a time (rows joined where they wrapped): its code points,
    // and for each the line and cell it came from (a cell's combining marks
    // share it), the cell's width, and its place in the whole line.
    std::vector<uint32_t> cps;
    std::vector<int> cellOf, lineOf, posOf;
    std::vector<uint8_t> widthOf;
    int pos = 0;                      // cells before this row in the line
    auto addCell = [&](const char* p, size_t n, int line, int col, int width) {
        utf8Decode(p, n, cps);
        size_t now = cps.size();
        cellOf.resize(now, col);
        lineOf.resize(now, line);
        posOf.resize(now, pos + col);
        widthOf.resize(now, (uint8_t)(width == 2 ? 2 : 1));
    };
    auto search = [&]() {
        if (cps.size() >= q.size()) {
            if (!matchCase)
                for (uint32_t& cp : cps) cp = foldCase(cp);
            auto sameCell = [&](size_t a, size_t b) {
                return cellOf[a] == cellOf[b] && lineOf[a] == lineOf[b];
            };
            for (size_t i = 0; i + q.size() <= cps.size() && found.size() < limit; ) {
                size_t k = 0;
                while (k < q.size() && cps[i + k] == q[k]) k++;
                if (k < q.size()) { i++; continue; }
                // A match starts at its first cell and ends after its last
                // one; one that starts on a combining mark is no match.
                if (i > 0 && sameCell(i - 1, i)) { i++; continue; }
                size_t e = i + q.size() - 1;
                if (e + 1 < cps.size() && sameCell(e + 1, e)) { i++; continue; }
                TermMatch m;
                m.line = lineOf[i];
                m.col = cellOf[i];
                m.endLine = lineOf[e];
                m.endCol = cellOf[e] + widthOf[e];
                m.width = posOf[e] + widthOf[e] - posOf[i];
                found.push_back(m);
                i += q.size();
            }
        }
        cps.clear(); cellOf.clear(); lineOf.clear(); posOf.clear(); widthOf.clear();
        pos = 0;
    };
    int first = onAlt_ ? 0 : -historySize();
    const Grid& g = grid();
    for (int line = first; line < rows_ && found.size() < limit; line++) {
        bool wraps = line < rows_ - 1 && lineWraps(line);
        int textWidth = wraps ? lineTextWidth(line) : lineWidth(line);
        if (line < 0) {
            const HistLine& hl = history_[historySize() + line];
            int n = std::min(textWidth, hl.width);
            if (hl.cells.empty()) {   // a byte a cell, so plain ASCII
                size_t at = cps.size();
                cps.insert(cps.end(), hl.text.begin(), hl.text.begin() + n);
                cellOf.resize(at + n);
                posOf.resize(at + n);
                for (int c = 0; c < n; c++) {
                    cellOf[at + c] = c;
                    posOf[at + c] = pos + c;
                }
                lineOf.resize(at + n, line);
                widthOf.resize(at + n, 1);
            } else {
                for (int c = 0; c < n; c++) {
                    size_t from = cellOffset(hl.cells[c]);
                    size_t to = c + 1 < hl.width ? cellOffset(hl.cells[c + 1]) : hl.text.size();
                    int w = cellWidth(hl.cells[c]);
                    if (w == 0 || to == from) continue;   // the right half of a wide one
                    addCell(hl.text.data() + from, to - from, line, c, w);
                }
            }
        } else {
            for (int c = 0; c < textWidth; c++) {
                const TermCell& cell = g[line][c];
                if (cell.width == 0 || cell.ch.empty()) continue;
                addCell(cell.ch.data(), cell.ch.size(), line, c, cell.width);
            }
        }
        if (wraps) pos += textWidth;
        else search();
    }
    if (!cps.empty()) search();
    return found;
}

// ------------------------------------------------------------------ clear
void TerminalScreen::clearAll() {
    history_.clear();
    below_.clear();
    if (onAlt_) return;
    for (Row& row : main_) { row.assign(cols_, TermCell()); row.wrap = 0; }
    x_ = y_ = 0;
    wrapPending_ = false;
}

void TerminalScreen::clearToCursorLine() {
    history_.clear();
    below_.clear();
    if (onAlt_ || y_ == 0) return;
    int up = y_;
    main_.erase(main_.begin(), main_.begin() + up);
    for (int i = 0; i < up; i++) main_.push_back(Row(cols_, TermCell()));
    y_ = 0;
    savedMain_.y = std::max(0, savedMain_.y - up);
}

// ------------------------------------------------------------- scrollback
TerminalScreen::HistLine TerminalScreen::pack(const Row& row, size_t width) {
    HistLine h;
    bool simple = true;
    size_t bytes = 0;
    for (size_t c = 0; c < width; c++) {
        const TermCell& cell = row[c];
        if (bytes + cell.ch.size() > kMaxLineBytes) { width = c; break; }   // never, with
        bytes += cell.ch.size();                       // kTermMaxCellBytes a cell
        if (cell.ch.size() != 1 || cell.width != 1) simple = false;
    }
    h.width = (int)width;
    h.text.reserve(bytes);
    if (!simple) h.cells.reserve(width);
    for (size_t c = 0; c < width; c++) {
        const TermCell& cell = row[c];
        if (!simple) h.cells.push_back(packCell(h.text.size(), cell.width));
        h.text += cell.ch;
        if (h.runs.empty() || h.runs.back().style != cell.style)
            h.runs.push_back({(uint32_t)c, cell.style});
    }
    h.runs.shrink_to_fit();
    return h;
}

TerminalScreen::HistLine TerminalScreen::packRow(const Row& row) {
    // Trailing blanks in the default colors are dropped (lineCell gives a
    // blank past a line's end anyway), since most lines are short; a row
    // that wrapped keeps them, since they are part of the line's text.
    size_t keep = row.size();
    while (keep > 0 && !row.wrap) {
        const TermCell& c = row[keep - 1];
        if (c.ch != " " || c.width != 1 || c.style != TermStyle()) break;
        keep--;
    }
    HistLine h = pack(row, keep);
    h.wrap = row.wrap;
    return h;
}

TermCell TerminalScreen::histCell(const HistLine& h, int col) {
    TermCell cell;
    if (col < 0 || col >= h.width) return cell;
    if (h.cells.empty()) {
        cell.ch.assign(1, h.text[col]);
    } else {
        size_t from = cellOffset(h.cells[col]);
        size_t to = col + 1 < h.width ? cellOffset(h.cells[col + 1]) : h.text.size();
        cell.ch.assign(h.text, from, to - from);
        cell.width = (uint8_t)cellWidth(h.cells[col]);
    }
    // The last run starting at or before col.
    auto it = std::upper_bound(h.runs.begin(), h.runs.end(), (uint32_t)col,
                               [](uint32_t c, const StyleRun& r) { return c < r.col; });
    if (it != h.runs.begin()) cell.style = (it - 1)->style;
    return cell;
}

TerminalScreen::Row TerminalScreen::unpack(const HistLine& h) {
    Row row;
    row.reserve(h.width);
    for (int c = 0; c < h.width; c++) row.push_back(histCell(h, c));
    row.wrap = h.wrap;
    return row;
}

TermCell TerminalScreen::lineCell(int line, int col) const {
    if (col < 0) return TermCell();
    if (line >= 0) {
        if (line >= rows_ || col >= cols_) return TermCell();
        return grid()[line][col];
    }
    int i = historySize() + line;
    if (i < 0) return TermCell();
    return histCell(history_[i], col);
}

int TerminalScreen::lineWidth(int line) const {
    if (line >= 0) return cols_;
    int i = historySize() + line;
    return i < 0 ? 0 : history_[i].width;
}

std::string TerminalScreen::lineText(int line) const {
    if (line >= 0) return line < rows_ ? rowText(line) : std::string();
    int i = historySize() + line;
    if (i < 0) return std::string();
    // The cells' text is h.text in order; trailing blanks dropped.
    const HistLine& h = history_[i];
    size_t keep = h.text.size();
    while (keep > 0 && h.text[keep - 1] == ' ') keep--;
    return h.text.substr(0, keep);
}

bool TerminalScreen::lineWraps(int line) const {
    if (line >= 0) return line < rows_ && grid()[line].wrap != 0;
    int i = historySize() + line;
    return i >= 0 && history_[i].wrap != 0;
}

int TerminalScreen::lineTextWidth(int line) const {
    int width = lineWidth(line);
    bool padded = line >= 0 ? (line < rows_ && grid()[line].wrap == 2)
                            : (historySize() + line >= 0 &&
                               history_[historySize() + line].wrap == 2);
    if (padded && width > 0) {
        TermCell last = lineCell(line, width - 1);
        if (last.ch == " " && last.width == 1) width--;
    }
    return width;
}

size_t TerminalScreen::historyBytes() const {
    size_t n = 0;
    for (const HistLine& h : history_) {
        n += sizeof(HistLine);
        // A short string lives inside the std::string itself.
        if (h.text.capacity() >= sizeof(std::string) - 1) n += h.text.capacity() + 1;
        n += h.cells.capacity() * sizeof(uint32_t);
        n += h.runs.capacity() * sizeof(StyleRun);
    }
    return n;
}

size_t TerminalScreen::historyCellBytes() const {
    size_t n = 0;
    for (const HistLine& h : history_)
        n += sizeof(Row) + h.width * sizeof(TermCell);
    return n;
}

void TerminalScreen::setScrollbackLimit(int lines) {
    historyLimit_ = std::max(0, lines);
    while ((int)history_.size() > historyLimit_) history_.pop_front();
}

void TerminalScreen::pushHistory(const Row& row) {
    historyBase_++;
    if (historyLimit_ <= 0) return;
    history_.push_back(packRow(row));
    while ((int)history_.size() > historyLimit_) history_.pop_front();
}

// ----------------------------------------------------------------- resize
void TerminalScreen::resize(int cols, int rows, std::vector<TermPos>* keep) {
    // Two columns at least: a wide character needs two cells, and at one
    // column print wrote past the row.
    cols = std::max(cols, 2);
    rows = std::max(rows, 1);
    if (cols == cols_ && rows == rows_) return;

    // The main screen and the scrollback: reflowed. The places to carry
    // along: the main screen's saved cursor, its live cursor when it is on
    // show, the host's places, and the top of the screen.
    std::vector<Point> points;
    int hist = historySize();
    points.push_back({hist + savedMain_.y, savedMain_.x, savedMain_.wrapPending, true, false});
    if (!onAlt_) points.push_back({hist + y_, x_, wrapPending_, true, false});
    size_t firstKept = points.size();
    if (keep)
        for (const TermPos& p : *keep)
            points.push_back({hist + p.line, p.col, false, false, hist + p.line < 0});
    reflowMain(cols, rows, points);
    // reflowMain left each point's abs as a line number on the new screen.
    savedMain_.y = points[0].abs;
    savedMain_.x = points[0].col;
    savedMain_.wrapPending = points[0].pending;
    if (!onAlt_) {
        y_ = points[1].abs;
        x_ = points[1].col;
    }
    if (keep)
        for (size_t i = 0; i < keep->size(); i++)
            (*keep)[i] = {points[firstKept + i].abs, points[firstKept + i].col};

    // The alternate screen: columns cut or padded, and lines dropped off
    // the top to keep the cursor's line on screen when it is on show.
    for (Row& row : alt_) {
        row.resize(cols, TermCell());
        row.wrap = 0;
        // A wide character cut in half at the new edge becomes a blank.
        if (row[cols - 1].width == 2) row[cols - 1] = TermCell();
    }
    if (onAlt_) {
        int drop = std::max(0, y_ + 1 - rows);
        if (drop) {
            alt_.erase(alt_.begin(), alt_.begin() + drop);
            y_ -= drop;
        }
    }
    alt_.resize(rows, Row(cols, TermCell()));

    bool pending = !onAlt_ && points[1].pending;
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bottom_ = rows_ - 1;
    resetTabs();
    x_ = clampInt(x_, 0, cols_ - 1);
    y_ = clampInt(y_, 0, rows_ - 1);
    wrapPending_ = pending && autowrap_;
    for (Saved* s : {&savedMain_, &savedAlt_}) {
        s->x = clampInt(s->x, 0, cols_ - 1);
        s->y = clampInt(s->y, 0, rows_ - 1);
    }
}

// --------------------------------------------------------------- reflow
size_t TerminalScreen::cellStart(const HistLine& h, int col) {
    if (col >= h.width) return h.text.size();
    return h.cells.empty() ? (size_t)col : cellOffset(h.cells[col]);
}

// Cells [from, to) of `s` added to the end of `d`, packed the same way.
void TerminalScreen::appendCells(HistLine& d, const HistLine& s, int from, int to) {
    from = std::max(from, 0);
    to = std::min(to, s.width);
    if (from >= to) return;
    size_t b0 = cellStart(s, from), b1 = cellStart(s, to);
    // Never past kMaxLineBytes: the cells that would go beyond are left
    // off (a line of a gigabyte, joined in a reflow).
    while (to > from && d.text.size() + (b1 - b0) > kMaxLineBytes) b1 = cellStart(s, --to);
    if (from >= to) return;
    bool simple = s.cells.empty();
    if (!simple || !d.cells.empty()) {
        if (d.cells.empty())          // d was a byte a cell; say so cell by cell
            for (int c = 0; c < d.width; c++) d.cells.push_back(packCell((size_t)c, 1));
        size_t base = d.text.size();
        for (int c = from; c < to; c++) {
            size_t at = base + cellStart(s, c) - b0;
            d.cells.push_back(packCell(at, simple ? 1 : cellWidth(s.cells[c])));
        }
    }
    d.text.append(s.text, b0, b1 - b0);
    auto it = std::upper_bound(s.runs.begin(), s.runs.end(), (uint32_t)from,
                               [](uint32_t c, const StyleRun& r) { return c < r.col; });
    TermStyle st = it != s.runs.begin() ? (it - 1)->style : TermStyle();
    auto add = [&](uint32_t col, const TermStyle& style) {
        if (d.runs.empty() || d.runs.back().style != style) d.runs.push_back({col, style});
    };
    add((uint32_t)d.width, st);
    for (; it != s.runs.end() && (int)it->col < to; ++it)
        add((uint32_t)(d.width + it->col - from), it->style);
    d.width += to - from;
}

// A byte a cell again where that is what the line is, and no spare room.
void TerminalScreen::simplify(HistLine& h) {
    if (!h.cells.empty() && h.text.size() == (size_t)h.width) {
        bool ones = true;
        for (uint32_t c : h.cells)
            if (cellWidth(c) != 1) { ones = false; break; }
        if (ones) h.cells.clear();
    }
    h.text.shrink_to_fit();
    h.cells.shrink_to_fit();
    h.runs.shrink_to_fit();
}

// The scrollback and the main screen as one list of lines, rows that wrapped
// joined into the lines they are part of and wrapped again at `cols`, then
// split into scrollback and a screen of `rows`. Each point (abs: an index
// into scrollback + screen) is carried to its character; on return abs is
// its line as lineCell numbers them. Points marked `cursor` keep their place
// past the end of the text (the line grows blanks to reach it), and one
// whose `pending` is set stands just after its cell, as a cursor waiting to
// wrap does.
void TerminalScreen::reflowMain(int cols, int rows, std::vector<Point>& points) {
    int oldRows = rows_;
    int hist = historySize();
    long long firstId = historyBase_ - hist;
    // The last screen row in use: text, a wrap, or a cursor on it. Rows
    // below it are blank and are not carried.
    int lastUsed = below_.empty() ? 0 : oldRows - 1;
    for (int r = oldRows - 1; r > lastUsed; r--) {
        const Row& row = main_[r];
        bool used = row.wrap != 0;
        for (const TermCell& c : row)
            if (used || c.ch != " " || c.width != 1 || c.style != TermStyle()) { used = true; break; }
        if (used) { lastUsed = r; break; }
    }
    for (const Point& p : points)
        if (p.cursor) lastUsed = std::max(lastUsed, p.abs - hist);
    int oldTrailing = oldRows - 1 - lastUsed;

    std::vector<HistLine> src;
    src.reserve(hist + lastUsed + 1);
    for (HistLine& h : history_) src.push_back(std::move(h));
    history_.clear();
    for (int r = 0; r <= lastUsed; r++) src.push_back(packRow(main_[r]));
    for (HistLine& h : below_) src.push_back(std::move(h));
    below_.clear();
    int srcCount = (int)src.size();
    // The top of the screen, as a point too.
    points.push_back({hist, 0, false, false, false});

    std::vector<char> mapped(points.size(), 0);
    auto onLine = [&](size_t k, int i, int j) {
        return !mapped[k] && !points[k].dropped && points[k].abs >= i && points[k].abs <= j;
    };
    std::vector<HistLine> dst;
    dst.reserve(src.size() + src.size() / 4);
    std::vector<int> segStart;      // a line's rows' offsets, old and new
    std::vector<int> rowStart, rowCells;
    for (int i = 0; i < srcCount; ) {
        int j = i;
        while (j + 1 < srcCount && src[j].wrap) j++;
        // Rows that need nothing: every wrapped row exactly as wide as the
        // new width, the last no wider, and no cursor beyond the edge.
        bool same = src[j].width <= cols;
        for (int k = i; k < j && same; k++)
            if (src[k].width != cols) same = false;
        for (size_t k = 0; k < points.size() && same; k++) {
            if (!points[k].cursor || !onLine(k, i, j)) continue;
            int off = points[k].col + (points[k].pending ? 1 : 0);
            if (off > cols || (off == cols && !points[k].pending)) same = false;
        }
        if (same) {
            int base = (int)dst.size();
            for (size_t k = 0; k < points.size(); k++)
                if (onLine(k, i, j)) {
                    points[k].abs = base + (points[k].abs - i);
                    points[k].col = std::min(points[k].col, cols - 1);
                    // Waiting to wrap only at the last column; on a wider
                    // row the cursor simply stands after its cell.
                    if (points[k].pending && points[k].col < cols - 1) {
                        points[k].col++;
                        points[k].pending = false;
                    }
                    mapped[k] = 1;
                }
            for (int k = i; k <= j; k++) dst.push_back(std::move(src[k]));
            i = j + 1;
            continue;
        }
        // Join the rows, less a wide character's leftover blank at the end
        // of a row that wrapped.
        segStart.clear();
        HistLine line;
        for (int k = i; k <= j; k++) {
            segStart.push_back(line.width);
            int w = src[k].width;
            if (k < j && src[k].wrap == 2 && w > 0) {
                TermCell last = histCell(src[k], w - 1);
                if (last.ch == " " && last.width == 1) w--;
            }
            appendCells(line, src[k], 0, w);
        }
        // Its length: trailing blanks in the default colors dropped, but as
        // far as any cursor on it.
        int n = line.width;
        while (n > 0) {
            TermCell c = histCell(line, n - 1);
            if (c.ch != " " || c.width != 1 || c.style != TermStyle()) break;
            n--;
        }
        auto offsetOf = [&](const Point& p) {
            return segStart[p.abs - i] + p.col + (p.pending ? 1 : 0);
        };
        int needed = n;
        for (size_t k = 0; k < points.size(); k++)
            if (points[k].cursor && onLine(k, i, j))
                needed = std::max(needed, offsetOf(points[k]));
        while (line.width < needed) {           // blanks out to a cursor
            HistLine blankCell;
            blankCell.text = " ";
            blankCell.width = 1;
            blankCell.runs.push_back({0, TermStyle()});
            int before = line.width;
            appendCells(line, blankCell, 0, 1);
            if (line.width == before) break;    // the line is as long as it can be
        }
        n = std::min(needed, line.width);
        // Wrap at the new width: a wide character that would be cut in two
        // goes whole to the next row, leaving a blank.
        rowStart.clear();
        rowCells.clear();
        int base = (int)dst.size();
        int a = 0;
        do {
            HistLine row;
            if (n - a <= cols) {
                appendCells(row, line, a, n);
                rowStart.push_back(a);
                rowCells.push_back(n - a);
                a = n;
            } else {
                int b = a + cols;
                bool wide = !line.cells.empty() && cellWidth(line.cells[b - 1]) == 2;
                if (wide && b - 1 > a) {
                    appendCells(row, line, a, b - 1);
                    HistLine pad;
                    pad.text = " ";
                    pad.width = 1;
                    pad.runs.push_back({0, TermStyle()});
                    appendCells(row, pad, 0, 1);
                    row.wrap = 2;
                    rowStart.push_back(a);
                    rowCells.push_back(b - 1 - a);
                    a = b - 1;
                } else {
                    appendCells(row, line, a, b);
                    row.wrap = 1;
                    rowStart.push_back(a);
                    rowCells.push_back(b - a);
                    a = b;
                }
            }
            simplify(row);
            dst.push_back(std::move(row));
        } while (a < n);
        // Each point to the row holding its offset.
        int last = (int)rowStart.size() - 1;
        for (size_t k = 0; k < points.size(); k++) {
            if (!onLine(k, i, j)) continue;
            Point& p = points[k];
            int off = std::min(offsetOf(p), n);
            int r = last;
            while (r > 0 && rowStart[r] > off) r--;
            int col = off - rowStart[r];
            p.pending = false;
            if (col >= cols) {                 // just past a full last row
                col = cols - 1;
                p.pending = p.cursor;
            } else if (r < last && col >= rowCells[r]) {
                col = rowCells[r] - 1;
            }
            p.abs = base + r;
            p.col = col;
            mapped[k] = 1;
        }
        i = j + 1;
    }
    // Points on blank rows below the text keep their distance from it.
    int newLast = (int)dst.size() - 1;
    for (size_t k = 0; k < points.size(); k++) {
        Point& p = points[k];
        if (mapped[k] || p.dropped) continue;
        p.abs = newLast + (p.abs - (srcCount - 1));
        p.col = std::min(p.col, cols - 1);
    }

    // Where the screen starts: where it started (its first character's new
    // row), moved down if the text and cursor no longer fit below it, or up
    // to bring lines back from the scrollback when there is room (blank
    // rows below the text stay as many as before, while there are lines to
    // bring back).
    Point top = points.back();
    points.pop_back();
    int newTop = top.abs;
    int trailing = std::min(oldTrailing, rows - 1);
    newTop = std::min(newTop, newLast - (rows - 1 - trailing));
    newTop = std::max(newTop, newLast - rows + 1);
    // The cursor stays on screen above all: lines below it that no longer
    // fit (a completion list under the prompt) are set aside in below_, and
    // come back when the screen grows again.
    newTop = std::min(newTop, points[onAlt_ ? 0 : 1].abs);
    newTop = std::max(newTop, 0);
    int keepBelow = newLast;
    while (keepBelow >= newTop + rows) {         // trailing blank lines: not kept
        const HistLine& h = dst[keepBelow];
        bool blankLine = h.wrap == 0;
        for (int c = 0; blankLine && c < h.width; c++) {
            TermCell cell = histCell(h, c);
            if (cell.ch != " " || cell.width != 1 || cell.style != TermStyle()) blankLine = false;
        }
        if (!blankLine) break;
        keepBelow--;
    }
    for (int r = newTop + rows; r <= keepBelow; r++) below_.push_back(std::move(dst[r]));

    // Above it: the scrollback, oldest lines dropped past the limit.
    int from = std::max(0, newTop - std::max(historyLimit_, 0));
    for (int r = from; r < newTop; r++) history_.push_back(std::move(dst[r]));
    historyBase_ = firstId + newTop;
    main_.clear();
    main_.reserve(rows);
    for (int r = newTop; r < newTop + rows; r++) {
        if (r < (int)dst.size()) {
            Row row = unpack(dst[r]);
            row.resize(cols, TermCell());
            if (row[cols - 1].width == 2) row[cols - 1] = TermCell();
            main_.push_back(std::move(row));
        } else {
            main_.push_back(Row(cols, TermCell()));
        }
    }
    // Lines as lineCell numbers them; a point whose line left the
    // scrollback goes above it.
    int histNow = (int)history_.size();
    for (Point& p : points) {
        if (!p.dropped) p.abs -= newTop;
        if (p.dropped || p.abs < -histNow) p.abs = -histNow - 1;
    }
}

// ------------------------------------------------------------------ mouse
void TerminalScreen::resetMouseModes() {
    mouseTracking_ = 0;
    mouseEncoding_ = 0;
    focusEvents_ = false;
}

std::string TerminalScreen::encodeMouse(TermMouse kind, int button, int col, int row,
                                        int mods) const {
    int mode = mouseTracking_;
    if (!mode || col < 0 || row < 0) return "";
    bool wheel = kind == TermMouse::WheelUp || kind == TermMouse::WheelDown;
    if (mode == 9 && kind != TermMouse::Press && !wheel) return "";   // X10: presses
    if (kind == TermMouse::Motion && (mode < 1002 || (button < 0 && mode < 1003))) return "";
    if ((kind == TermMouse::Press || kind == TermMouse::Release) && (button < 0 || button > 2))
        return "";
    bool sgr = mouseEncoding_ == 1006;
    int cb = 0;
    switch (kind) {
    case TermMouse::Press: cb = button; break;
    case TermMouse::Release: cb = sgr ? button : 3; break;
    case TermMouse::Motion: cb = (button < 0 ? 3 : button) + 32; break;
    case TermMouse::WheelUp: cb = 64; break;
    case TermMouse::WheelDown: cb = 65; break;
    }
    if (mode != 9) {
        if (mods & TermModShift) cb |= 4;
        if (mods & TermModAlt) cb |= 8;
        if (mods & TermModCtrl) cb |= 16;
    }
    int x = col + 1, y = row + 1;
    if (sgr)
        return "\x1b[<" + std::to_string(cb) + ";" + std::to_string(x) + ";" +
               std::to_string(y) + (kind == TermMouse::Release ? "m" : "M");
    if (mouseEncoding_ == 1015)
        return "\x1b[" + std::to_string(cb + 32) + ";" + std::to_string(x) + ";" +
               std::to_string(y) + "M";
    // xterm's own bytes: each value plus 32 in a byte, so columns and rows
    // past 223 cannot be sent; with 1005 the coordinates are UTF-8, to 2015.
    std::string out = "\x1b[M";
    out += (char)(cb + 32);
    for (int v : {x, y}) {
        v += 32;
        if (mouseEncoding_ == 1005) {
            if (v > 2047) return "";
            out += utf8Encode((uint32_t)v);
        } else {
            if (v > 255) return "";
            out += (char)v;
        }
    }
    return out;
}

// ---------------------------------------------------------------- editing
void TerminalScreen::clearWide(int y, int x) {
    Row& row = grid()[y];
    if (x < 0 || x >= cols_) return;
    if (row[x].width == 0 && x > 0) row[x - 1] = blank();
    if (row[x].width == 2 && x + 1 < cols_) row[x + 1] = blank();
}

void TerminalScreen::eraseCells(int y, int from, int to) {
    from = clampInt(from, 0, cols_);
    to = clampInt(to, 0, cols_);
    if (from >= to) return;
    clearWide(y, from);
    clearWide(y, to - 1);
    Row& row = grid()[y];
    for (int x = from; x < to; x++) row[x] = blank();
}

// Row y no longer continues a row above it (erased from its first column):
// the row above, or the newest scrollback line above row 0, stops wrapping.
void TerminalScreen::unwrapAbove(int y) {
    if (y > 0) grid()[y - 1].wrap = 0;
    else if (y == 0 && !onAlt_ && !history_.empty()) history_.back().wrap = 0;
}

void TerminalScreen::moveTo(int x, int y) {
    x_ = clampInt(x, 0, cols_ - 1);
    y_ = clampInt(y, 0, rows_ - 1);
    wrapPending_ = false;
}

void TerminalScreen::scrollUp(int n, bool save) {
    n = std::min(n, bottom_ - top_ + 1);
    if (n <= 0) return;
    if (!onAlt_) below_.clear();      // what was set aside no longer lines up
    Grid& g = grid();
    if (save && !onAlt_ && top_ == 0)
        for (int r = 0; r < n; r++) pushHistory(g[r]);
    std::rotate(g.begin() + top_, g.begin() + top_ + n, g.begin() + bottom_ + 1);
    for (int r = bottom_ - n + 1; r <= bottom_; r++) { g[r].assign(cols_, blank()); g[r].wrap = 0; }
}

void TerminalScreen::scrollDown(int n) {
    n = std::min(n, bottom_ - top_ + 1);
    if (n <= 0) return;
    if (!onAlt_) below_.clear();      // what was set aside no longer lines up
    Grid& g = grid();
    std::rotate(g.begin() + top_, g.begin() + bottom_ + 1 - n, g.begin() + bottom_ + 1);
    for (int r = top_; r < top_ + n; r++) { g[r].assign(cols_, blank()); g[r].wrap = 0; }
}

void TerminalScreen::lineFeed() {
    if (y_ == bottom_) scrollUp(1, true);
    else if (y_ < rows_ - 1) y_++;
}

void TerminalScreen::reverseIndex() {
    if (y_ == top_) scrollDown(1);
    else if (y_ > 0) y_--;
}

void TerminalScreen::print(uint32_t cp, const std::string& utf8In) {
    std::string utf8 = utf8In;
    int charset = gl_ == 0 ? g0_ : g1_;
    if (charset == 1 && cp >= 0x5F && cp <= 0x7E) {
        cp = kDecGraphics[cp - 0x5F];
        utf8 = utf8Encode(cp);
    }
    int w = termCharWidth(cp);
    Grid& g = grid();
    if (w == 0) {
        // Combining mark: joins the character before the cursor.
        int x = wrapPending_ ? x_ : x_ - 1;
        if (x >= 0 && g[y_][x].width == 0 && x > 0) x--;
        if (x >= 0 && g[y_][x].ch.size() + utf8.size() <= kTermMaxCellBytes)
            g[y_][x].ch += utf8;
        return;
    }
    if (wrapPending_ && autowrap_) {
        g[y_].wrap = promptSp_ ? 0 : 1;      // the row continues on the next
        promptSp_ = false;
        x_ = 0;
        lineFeed();
    }
    wrapPending_ = false;
    if (w == 2 && x_ == cols_ - 1) {
        if (autowrap_) {                     // doesn't fit: wrap it whole
            eraseCells(y_, x_, x_ + 1);
            g[y_].wrap = promptSp_ ? 0 : 2;
            promptSp_ = false;
            x_ = 0;
            lineFeed();
        } else {
            w = 1;                           // no room and no wrap: truncate
            utf8 = " ";
        }
    }
    Row& row = g[y_];
    if (insertMode_) {
        // The shift moves a wide character whole when the cursor is on its
        // left half. On its right half the character is split, so both
        // halves go: clearWide alone blanked only the left one, and the
        // right half moved on as an orphan (and with the cursor on a left
        // half it blanked the right one, orphaning the left).
        if (row[x_].width == 0) eraseCells(y_, x_, x_ + 1);
        row.insert(row.begin() + x_, w, blank());
        row.resize(cols_);
        if (row[cols_ - 1].width == 2) row[cols_ - 1] = blank();
    }
    clearWide(y_, x_);
    if (w == 2) clearWide(y_, x_ + 1);
    TermCell c;
    c.ch = utf8;
    c.style = style_;
    c.style.link = link_;
    c.width = (uint8_t)w;
    row[x_] = c;
    if (w == 2) {
        TermCell right;
        right.ch = "";
        right.style = c.style;
        right.width = 0;
        row[x_ + 1] = right;
    }
    lastChar_ = utf8;
    lastCp_ = cp;
    x_ += w;
    if (x_ >= cols_) {
        x_ = cols_ - 1;
        wrapPending_ = autowrap_;
    }
}

void TerminalScreen::control(unsigned char c) {
    switch (c) {
    case '\n': case '\v': case '\f':
        lineFeed();
        wrapPending_ = false;
        break;
    case '\r':
        x_ = 0;
        wrapPending_ = false;
        promptSp_ = false;
        break;
    case '\b':
        if (x_ > 0) x_--;
        wrapPending_ = false;
        break;
    case '\t': {
        int x = x_ + 1;
        while (x < cols_ && !tabs_[x]) x++;
        x_ = std::min(x, cols_ - 1);
        break;
    }
    case 0x0E: gl_ = 1; break;               // SO: G1 into GL
    case 0x0F: gl_ = 0; break;               // SI: G0 into GL
    default: break;                          // BEL and the rest
    }
}

// --------------------------------------------------------------- escapes
void TerminalScreen::saveCursor() {
    Saved& s = altScreen() ? savedAlt_ : savedMain_;
    s.x = x_; s.y = y_; s.style = style_;
    s.originMode = originMode_; s.autowrap = autowrap_;
    s.wrapPending = wrapPending_;
    s.g0 = g0_; s.g1 = g1_; s.gl = gl_;
}

void TerminalScreen::restoreCursor() {
    const Saved& s = altScreen() ? savedAlt_ : savedMain_;
    moveTo(s.x, s.y);
    style_ = s.style;
    originMode_ = s.originMode;
    autowrap_ = s.autowrap;
    wrapPending_ = s.wrapPending;
    g0_ = s.g0; g1_ = s.g1; gl_ = s.gl;
}

void TerminalScreen::switchScreen(bool alt, bool save, bool clearAlt) {
    if (alt == altScreen()) return;
    if (alt) {
        if (save) saveCursor();              // saved into the main slot
        onAlt_ = true;
        if (clearAlt) {
            TermCell b = blank();
            for (Row& r : alt_) { r.assign(cols_, b); r.wrap = 0; }
        }
    } else {
        if (clearAlt) {
            for (Row& r : alt_) { r.assign(cols_, TermCell()); r.wrap = 0; }
        }
        onAlt_ = false;
        if (save) restoreCursor();
    }
    wrapPending_ = false;
}

void TerminalScreen::esc(const std::string& inter, unsigned char f) {
    if (inter.empty()) {
        switch (f) {
        case '7': saveCursor(); break;           // DECSC
        case '8': restoreCursor(); break;        // DECRC
        case 'D': lineFeed(); wrapPending_ = false; break;   // IND
        case 'E': x_ = 0; lineFeed(); wrapPending_ = false; break;   // NEL
        case 'M': reverseIndex(); wrapPending_ = false; break;   // RI
        case 'H': if (x_ < cols_) tabs_[x_] = true; break;   // HTS
        case 'c': fullReset(); break;            // RIS
        case '=': appKeypad_ = true; break;      // DECKPAM
        case '>': appKeypad_ = false; break;     // DECKPNM
        default: break;
        }
        return;
    }
    if (inter == "(" || inter == ")") {          // designate G0 / G1
        int set = f == '0' ? 1 : 0;
        (inter == "(" ? g0_ : g1_) = set;
        return;
    }
    if (inter == "#" && f == '8') {              // DECALN: fill with E
        for (Row& r : grid()) {
            for (TermCell& c : r) { c = TermCell(); c.ch = "E"; }
            r.wrap = 0;
        }
        moveTo(0, 0);
    }
}

void TerminalScreen::osc(const std::string& body) {
    if (body.rfind("8;", 0) == 0) {
        hyperlink(body);
    } else if (body.rfind("0;", 0) == 0 || body.rfind("2;", 0) == 0) {
        title_ = termCleanTitle(body.substr(2), 1024);
    } else if (body.rfind("133;D", 0) == 0) {
        commandsEnded_++;
    } else if (body.rfind("133;A", 0) == 0) {
        promptSp_ = true;
    } else {
        std::string path = termOsc7Path(body);
        if (!path.empty()) directory_ = path;
    }
}

// ---------------------------------------------------------- hyperlinks
namespace {

bool startsWithNoCase(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[i]) != prefix[i]) return false;
    return true;
}

// An address a link may have: http, https or file, nothing after the
// scheme's slashes missing, no spaces or control characters.
bool linkableURI(const std::string& uri) {
    if (uri.size() > TerminalScreen::kMaxLinkBytes) return false;
    size_t rest = startsWithNoCase(uri, "https://") ? 8
                : startsWithNoCase(uri, "http://") || startsWithNoCase(uri, "file://") ? 7 : 0;
    if (rest == 0 || rest >= uri.size()) return false;
    for (unsigned char ch : uri)
        if (ch <= 0x20 || ch == 0x7F) return false;
    return true;
}

}  // namespace

const std::string& TerminalScreen::linkURI(uint32_t id) const {
    static const std::string none;
    return id >= 1 && id <= links_.size() ? links_[id - 1] : none;
}

void TerminalScreen::hyperlink(const std::string& body) {
    // "8;params;URI": params are key=value pairs split by ':', of which
    // only id= means anything (text with the same id and address is one
    // link, even when printed in pieces).
    size_t semi = body.find(';', 2);
    if (semi == std::string::npos) { link_ = 0; return; }
    std::string uri = body.substr(semi + 1);
    if (uri.empty() || !linkableURI(uri)) { link_ = 0; return; }
    std::string id;
    const std::string params = body.substr(2, semi - 2);
    for (size_t i = 0; i <= params.size();) {
        size_t j = params.find(':', i);
        if (j == std::string::npos) j = params.size();
        if (params.compare(i, 3, "id=") == 0) id = params.substr(i + 3, std::min<size_t>(j - i - 3, 256));
        i = j + 1;
    }
    link_ = internLink(id + '\x1f' + uri, uri);
}

uint32_t TerminalScreen::internLink(const std::string& key, const std::string& uri) {
    auto it = linkIds_.find(key);
    if (it != linkIds_.end()) return it->second;
    if (freeLinks_.empty() && links_.size() >= kMaxLinks) collectLinks();
    uint32_t id;
    if (!freeLinks_.empty()) {
        id = freeLinks_.back();
        freeLinks_.pop_back();
        links_[id - 1] = uri;
        linkKeys_[id - 1] = key;
    } else if (links_.size() < kMaxLinks) {
        links_.push_back(uri);
        linkKeys_.push_back(key);
        id = (uint32_t)links_.size();
    } else {
        return 0;   // every id is still on screen or in the scrollback
    }
    linkIds_[key] = id;
    return id;
}

void TerminalScreen::collectLinks() {
    std::vector<bool> used(links_.size() + 1, false);
    auto mark = [&](uint32_t id) { if (id && id < used.size()) used[id] = true; };
    for (const Grid* g : {&main_, &alt_})
        for (const Row& row : *g)
            for (const TermCell& c : row) mark(c.style.link);
    for (const HistLine& h : history_)
        for (const StyleRun& r : h.runs) mark(r.style.link);
    for (const HistLine& h : below_)
        for (const StyleRun& r : h.runs) mark(r.style.link);
    mark(link_);
    for (uint32_t id = 1; id <= links_.size(); id++) {
        if (used[id] || links_[id - 1].empty()) continue;
        linkIds_.erase(linkKeys_[id - 1]);
        links_[id - 1].clear();
        linkKeys_[id - 1].clear();
        freeLinks_.push_back(id);
    }
}

void TerminalScreen::setMode(const std::string& csi, bool on) {
    bool priv = !csi.empty() && csi[0] == '?';
    for (const auto& p : TermParser::params(priv ? csi.substr(1) : csi)) {
        int m = p[0];
        if (!priv) {
            if (m == 4) insertMode_ = on;        // IRM
            continue;
        }
        switch (m) {
        case 1: appCursor_ = on; break;          // DECCKM
        case 6:                                  // DECOM
            originMode_ = on;
            moveTo(0, on ? top_ : 0);
            break;
        case 7: autowrap_ = on; if (!on) wrapPending_ = false; break;
        case 25: cursorVisible_ = on; break;     // DECTCEM
        case 47: switchScreen(on, false, false); break;
        case 1047: switchScreen(on, false, !on); break;
        case 1048: if (on) saveCursor(); else restoreCursor(); break;
        case 1049: switchScreen(on, true, true); break;
        case 2004: bracketedPaste_ = on; break;
        case 9: case 1000: case 1002: case 1003:   // mouse tracking
            mouseTracking_ = on ? m : 0;         // any reset turns it off (xterm)
            break;
        case 1005: case 1006: case 1015:         // mouse encodings
            if (on) mouseEncoding_ = m;
            else if (mouseEncoding_ == m) mouseEncoding_ = 0;
            break;
        case 1004: focusEvents_ = on; break;
        case 2026:                               // synchronized output
            if (syncOutput_ && !on) syncEnds_++;
            syncOutput_ = on;
            break;
        default: break;
        }
    }
}

// DECRQM, "CSI ? Ps $ p" (or without "?" for an ANSI mode): is the mode set?
// The answer is "CSI ? Ps ; Pm $ y", Pm 1 set, 2 reset, 0 not known. A
// program asks for 2026 to learn whether synchronized output is supported.
void TerminalScreen::reportMode(const std::string& params) {
    bool priv = !params.empty() && params[0] == '?';
    auto p = TermParser::params(priv ? params.substr(1) : params);
    int m = p.empty() ? -1 : p[0][0];
    int state = 0;
    auto is = [](bool on) { return on ? 1 : 2; };
    if (priv) {
        switch (m) {
        case 1: state = is(appCursor_); break;
        case 6: state = is(originMode_); break;
        case 7: state = is(autowrap_); break;
        case 25: state = is(cursorVisible_); break;
        case 47: case 1047: case 1049: state = is(onAlt_); break;
        case 2004: state = is(bracketedPaste_); break;
        case 2026: state = is(syncOutput_); break;
        case 9: case 1000: case 1002: case 1003: state = is(mouseTracking_ == m); break;
        case 1005: case 1006: case 1015: state = is(mouseEncoding_ == m); break;
        case 1004: state = is(focusEvents_); break;
        default: break;
        }
    } else if (m == 4) {
        state = is(insertMode_);
    }
    replies_ += std::string("\033[") + (priv ? "?" : "") + std::to_string(m < 0 ? 0 : m) +
                ";" + std::to_string(state) + "$y";
}

void TerminalScreen::csi(const std::string& csi, unsigned char f) {
    char lead = csi.empty() ? 0 : csi[0];
    if (f == 'p' && !csi.empty() && csi.back() == '$') {
        reportMode(csi.substr(0, csi.size() - 1));
        return;
    }
    if (f == 'h' || f == 'l') { setMode(csi, f == 'h'); return; }
    if (lead == '>' || lead == '<' || lead == '=') return;   // xterm extensions
    if (lead == '?') return;                 // DECSED, DECRQM and friends
    // Intermediates (e.g. " q" cursor style, "!p" soft reset).
    for (char c : csi)
        if (c >= 0x20 && c <= 0x2F) {
            if (csi.back() == '!' && f == 'p') {       // DECSTR
                style_ = TermStyle();
                insertMode_ = originMode_ = false;
                autowrap_ = cursorVisible_ = true;
                appCursor_ = appKeypad_ = false;
                top_ = 0; bottom_ = rows_ - 1;
                wrapPending_ = false;
                // As xterm: the character sets back to ASCII and the saved
                // cursor home. And synchronized output ends, since a host
                // sends this after a program died mid-frame (a curses
                // program killed in DEC graphics left q drawn as a line,
                // and the host kept waiting on the frame).
                g0_ = g1_ = gl_ = 0;
                (onAlt_ ? savedAlt_ : savedMain_) = Saved();
                if (syncOutput_) { syncOutput_ = false; syncEnds_++; }
            }
            return;
        }
    if (f == 'm') { applySgr(style_, csi); return; }

    auto params = TermParser::params(csi);
    auto arg = [&](size_t i, int def) {
        int v = i < params.size() ? params[i][0] : -1;
        return v <= 0 ? def : v;
    };
    int n = arg(0, 1);
    int raw = params[0][0] < 0 ? 0 : params[0][0];
    int rowBase = originMode_ ? top_ : 0;

    switch (f) {
    case '@': {                                  // ICH
        if (grid()[y_][x_].width == 0) eraseCells(y_, x_, x_ + 1);   // as IRM above
        Row& row = grid()[y_];
        n = std::min(n, cols_ - x_);
        row.insert(row.begin() + x_, n, blank());
        row.resize(cols_);
        if (row[cols_ - 1].width == 2) row[cols_ - 1] = blank();
        wrapPending_ = false;
        break;
    }
    case 'A':                                    // CUU
        moveTo(x_, std::max(y_ - n, y_ >= top_ ? top_ : 0));
        break;
    case 'B': case 'e':                          // CUD, VPR
        moveTo(x_, std::min(y_ + n, y_ <= bottom_ ? bottom_ : rows_ - 1));
        break;
    case 'C': case 'a':                          // CUF, HPR
        moveTo(x_ + n, y_);
        break;
    case 'D':                                    // CUB
        moveTo(x_ - n, y_);
        break;
    case 'E':                                    // CNL
        moveTo(0, std::min(y_ + n, y_ <= bottom_ ? bottom_ : rows_ - 1));
        break;
    case 'F':                                    // CPL
        moveTo(0, std::max(y_ - n, y_ >= top_ ? top_ : 0));
        break;
    case 'G': case '`':                          // CHA, HPA
        moveTo(n - 1, y_);
        break;
    case 'H': case 'f': {                        // CUP, HVP
        int row = arg(0, 1) - 1 + rowBase;
        if (originMode_) row = std::min(row, bottom_);
        moveTo(arg(1, 1) - 1, row);
        break;
    }
    case 'd': {                                  // VPA
        int row = n - 1 + rowBase;
        if (originMode_) row = std::min(row, bottom_);
        moveTo(x_, row);
        break;
    }
    case 'I':                                    // CHT
        for (int i = 0; i < n; i++) control('\t');
        break;
    case 'Z': {                                  // CBT
        int x = x_;
        for (int i = 0; i < n && x > 0; i++) {
            x--;
            while (x > 0 && !tabs_[x]) x--;
        }
        moveTo(x, y_);
        break;
    }
    case 'J':                                    // ED
        wrapPending_ = false;
        if (!onAlt_) below_.clear();
        // Soft wraps go with what is erased (as in xterm.js): rows erased
        // whole wrap no more, nor does the row above one erased from its
        // first column.
        if (raw == 0) {
            if (x_ == 0) unwrapAbove(y_);
            eraseCells(y_, x_, cols_);
            for (int r = y_ + 1; r < rows_; r++) eraseCells(r, 0, cols_);
            for (int r = y_; r < rows_; r++) grid()[r].wrap = 0;
        } else if (raw == 1) {
            for (int r = 0; r < y_; r++) { eraseCells(r, 0, cols_); grid()[r].wrap = 0; }
            eraseCells(y_, 0, x_ + 1);
            unwrapAbove(y_);
            if (x_ + 1 >= cols_) grid()[y_].wrap = 0;
        } else if (raw == 2) {
            unwrapAbove(0);
            for (int r = 0; r < rows_; r++) { eraseCells(r, 0, cols_); grid()[r].wrap = 0; }
        } else if (raw == 3) {
            history_.clear();                    // xterm: erase saved lines
        }
        break;
    case 'K':                                    // EL
        wrapPending_ = false;
        if (raw == 0) {
            if (x_ == 0) unwrapAbove(y_);
            eraseCells(y_, x_, cols_);
        } else if (raw == 1) {
            eraseCells(y_, 0, x_ + 1);
        } else if (raw == 2) {
            unwrapAbove(y_);
            eraseCells(y_, 0, cols_);
        }
        break;
    case 'X':                                    // ECH
        wrapPending_ = false;
        eraseCells(y_, x_, x_ + n);
        break;
    case 'L': case 'M': {                        // IL, DL
        if (y_ < top_ || y_ > bottom_) break;
        int saveTop = top_;
        top_ = y_;                               // scroll the part below
        if (f == 'L') scrollDown(n); else scrollUp(n);
        top_ = saveTop;
        x_ = 0;
        wrapPending_ = false;
        break;
    }
    case 'P': {                                  // DCH
        clearWide(y_, x_);
        Row& row = grid()[y_];
        n = std::min(n, cols_ - x_);
        row.erase(row.begin() + x_, row.begin() + x_ + n);
        row.resize(cols_, blank());
        if (row[x_].width == 0) row[x_] = blank();
        wrapPending_ = false;
        break;
    }
    case 'S': scrollUp(n); break;                // SU
    case 'T': if (params.size() == 1) scrollDown(n); break;   // SD
    case 'b':                                    // REP
        if (!lastChar_.empty()) {
            std::string ch = lastChar_;
            uint32_t cp = lastCp_;
            int saveCharset = gl_ == 0 ? g0_ : g1_;
            (gl_ == 0 ? g0_ : g1_) = 0;          // already translated
            for (int i = 0; i < std::min(n, cols_ * rows_); i++) print(cp, ch);
            (gl_ == 0 ? g0_ : g1_) = saveCharset;
        }
        break;
    case 'g':                                    // TBC
        if (raw == 0 && x_ < cols_) tabs_[x_] = false;
        else if (raw == 3) tabs_.assign(cols_, false);
        break;
    case 'n':                                    // DSR
        if (raw == 5) replies_ += "\x1b[0n";
        else if (raw == 6)
            replies_ += "\x1b[" + std::to_string(y_ - rowBase + 1) + ";" +
                        std::to_string(x_ + 1) + "R";
        break;
    case 'c':                                    // DA: a VT100 with AVO
        if (raw == 0) replies_ += "\x1b[?1;2c";
        break;
    case 'r': {                                  // DECSTBM
        int t = arg(0, 1) - 1;
        int b = arg(1, rows_) - 1;
        b = std::min(b, rows_ - 1);
        if (t < b) {
            top_ = t;
            bottom_ = b;
            moveTo(0, originMode_ ? top_ : 0);
        }
        break;
    }
    case 's':                                    // SCOSC
        if (csi.empty()) saveCursor();
        break;
    case 'u':                                    // SCORC
        if (csi.empty()) restoreCursor();
        break;
    default:                                     // window ops and the rest
        break;
    }
}

// ------------------------------------------------------------------- keys
std::string TerminalScreen::encodeKey(TermKey key, int mods, bool appCursor,
                                      bool appKeypad) {
    std::string m = std::to_string(1 + mods);
    auto cursor = [&](char c) -> std::string {
        if (mods) return "\x1b[1;" + m + c;
        return std::string(appCursor ? "\x1bO" : "\x1b[") + c;
    };
    auto tilde = [&](int code) -> std::string {
        std::string s = "\x1b[" + std::to_string(code);
        if (mods) s += ";" + m;
        return s + "~";
    };
    auto ss3 = [&](char c) -> std::string {
        if (mods) return "\x1b[1;" + m + c;
        return std::string("\x1bO") + c;
    };
    std::string meta = (mods & TermModAlt) ? "\x1b" : "";
    switch (key) {
    case TermKey::Up: return cursor('A');
    case TermKey::Down: return cursor('B');
    case TermKey::Right: return cursor('C');
    case TermKey::Left: return cursor('D');
    case TermKey::Home: return cursor('H');
    case TermKey::End: return cursor('F');
    case TermKey::Insert: return tilde(2);
    case TermKey::Delete: return tilde(3);
    case TermKey::PageUp: return tilde(5);
    case TermKey::PageDown: return tilde(6);
    case TermKey::F1: return ss3('P');
    case TermKey::F2: return ss3('Q');
    case TermKey::F3: return ss3('R');
    case TermKey::F4: return ss3('S');
    case TermKey::F5: return tilde(15);
    case TermKey::F6: return tilde(17);
    case TermKey::F7: return tilde(18);
    case TermKey::F8: return tilde(19);
    case TermKey::F9: return tilde(20);
    case TermKey::F10: return tilde(21);
    case TermKey::F11: return tilde(23);
    case TermKey::F12: return tilde(24);
    // Shift+Return is ESC CR, as VS Code's terminal setup for Claude Code
    // sends it: Claude Code (and other Ink programs) take it as a new line
    // rather than submitting, and zsh inserts a line break with it.
    case TermKey::Enter:
        if ((mods & TermModShift) && !(mods & TermModCtrl)) return "\x1b\r";
        return meta + "\r";
    case TermKey::KeypadEnter: return appKeypad ? "\x1bOM" : meta + "\r";
    case TermKey::Backspace:
        return meta + ((mods & TermModCtrl) ? "\x08" : "\x7f");
    case TermKey::Tab: return (mods & TermModShift) ? "\x1b[Z" : meta + "\t";
    case TermKey::Escape: return meta + "\x1b";
    }
    return "";
}

std::string TerminalScreen::encodeChar(uint32_t cp, int mods) {
    std::string out;
    if (mods & TermModCtrl) {
        uint32_t c = cp;
        if (c >= 'a' && c <= 'z') c -= 0x20;
        if (c >= '@' && c <= '_') out = std::string(1, (char)(c - '@'));
        else if (c == ' ' || c == '2') out = std::string(1, '\0');
        else if (c == '3') out = "\x1b";
        else if (c == '4') out = "\x1c";
        else if (c == '5') out = "\x1d";
        else if (c == '6') out = "\x1e";
        else if (c == '7' || c == '-' || c == '/') out = "\x1f";
        else if (c == '8' || c == '?') out = "\x7f";
    }
    if (out.empty()) out = utf8Encode(cp);
    if (mods & TermModAlt) out = "\x1b" + out;
    return out;
}

std::string TerminalScreen::encodePaste(const std::string& text, bool bracketed) {
    std::string body;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        body += text[i] == '\n' ? '\r' : text[i];
    }
    if (!bracketed) return body;
    // A paste must not be able to end the bracket early. Taking out the end
    // mark once was not enough: "ESC[20" + "ESC[201~" + "1~" closed up into
    // a new one. With no ESC left inside, no mark can form however the text
    // is split, and nothing else in it can be an escape sequence either.
    std::string safe;
    safe.reserve(body.size() + 12);
    safe += "\x1b[200~";
    for (char c : body) {
        if (c == '\x1b') safe += "\xE2\x90\x9B";   // U+241B SYMBOL FOR ESCAPE
        else safe += c;
    }
    safe += "\x1b[201~";
    return safe;
}

namespace {

// The paste's lines: split at "\r\n", "\n" and a lone "\r".
std::vector<std::string> pasteLines(const std::string& text) {
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
            lines.emplace_back();
        } else {
            lines.back() += c;
        }
    }
    return lines;
}

bool blankText(const std::string& s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\f' && c != '\v') return false;
    return true;
}

// The first `n` characters (code points) of UTF-8 text, and whether it was
// longer.
std::string firstChars(const std::string& s, size_t n, bool& cut) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (chars == n) break;
            chars++;
        }
        i++;
    }
    cut = i < s.size();
    return s.substr(0, i);
}

}  // namespace

TerminalScreen::PasteCheck TerminalScreen::checkPaste(const std::string& text, bool bracketed,
                                                      const std::string& mode) {
    PasteCheck out;
    out.text = text;
    const std::vector<std::string> lines = pasteLines(text);
    out.lines = (int)lines.size();
    if (lines.size() == 1 || mode == "never") return out;
    if (mode != "always") {
        if (bracketed) return out;
        // A command copied with its line break: pasted without it, so it
        // waits for Return (VS Code's clipboard-hijacking guard).
        if (lines.size() == 2 && blankText(lines[1])) {
            out.text = lines[0];
            out.lines = 1;
            return out;
        }
    }
    out.ask = true;
    for (size_t i = 0; i < lines.size() && i < 3; i++) {
        bool cut = false;
        // Control characters would draw oddly in the question.
        std::string line;
        for (char c : firstChars(lines[i], 30, cut))
            line += ((unsigned char)c < 0x20 || c == 0x7F) ? ' ' : c;
        if (i) out.preview += '\n';
        out.preview += line;
        if (cut) out.preview += "\xE2\x80\xA6";   // …
    }
    if (lines.size() > 3) out.preview += "\n\xE2\x80\xA6";
    return out;
}

std::string TerminalScreen::pasteAsOneLine(const std::string& text) {
    std::string out;
    for (char c : text)
        if (c != '\r' && c != '\n') out += c;
    return out;
}
