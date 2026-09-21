// TerminalScreen.cpp — see TerminalScreen.h. Behavior follows xterm where the
// details matter to real programs (pending wrap, erase with the current
// background, cursor movement clamped by the scroll region).
#include "TerminalScreen.h"
#include <algorithm>

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
    : cols_(std::max(cols, 1)), rows_(std::max(rows, 1)) {
    fullReset();
}

void TerminalScreen::fullReset() {
    main_.assign(rows_, Row(cols_, TermCell()));
    alt_.assign(rows_, Row(cols_, TermCell()));
    onAlt_ = false;
    x_ = y_ = 0;
    wrapPending_ = false;
    style_ = TermStyle();
    top_ = 0;
    bottom_ = rows_ - 1;
    savedMain_ = savedAlt_ = Saved();
    autowrap_ = true;
    originMode_ = insertMode_ = false;
    cursorVisible_ = true;
    appCursor_ = appKeypad_ = bracketedPaste_ = false;
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
    r0 = clampInt(r0, 0, rows_ - 1);
    r1 = clampInt(r1, 0, rows_ - 1);
    std::string out;
    for (int r = r0; r <= r1; r++) {
        int from = r == r0 ? clampInt(c0, 0, cols_ - 1) : 0;
        int to = r == r1 ? clampInt(c1, 0, cols_ - 1) : cols_ - 1;
        std::string line;
        size_t keep = 0;
        for (int c = from; c <= to; c++) {
            line += grid()[r][c].ch;
            if (grid()[r][c].ch != " ") keep = line.size();
        }
        line.resize(keep);
        out += line;
        if (r < r1) out += "\n";
    }
    return out;
}

// ----------------------------------------------------------------- resize
void TerminalScreen::resize(int cols, int rows) {
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    if (cols == cols_ && rows == rows_) return;
    for (Grid* g : {&main_, &alt_}) {
        for (Row& row : *g) {
            row.resize(cols, TermCell());
            // A wide character cut in half at the new edge becomes a blank.
            if (cols > 0 && row[cols - 1].width == 2) row[cols - 1] = TermCell();
        }
        // Drop lines off the top to keep the cursor's line on screen.
        int drop = g == &grid() ? std::max(0, y_ + 1 - rows) : 0;
        if (drop) g->erase(g->begin(), g->begin() + drop);
        g->resize(rows, Row(cols, TermCell()));
        if (g == &grid()) y_ -= drop;
    }
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bottom_ = rows_ - 1;
    resetTabs();
    x_ = clampInt(x_, 0, cols_ - 1);
    y_ = clampInt(y_, 0, rows_ - 1);
    wrapPending_ = false;
    for (Saved* s : {&savedMain_, &savedAlt_}) {
        s->x = clampInt(s->x, 0, cols_ - 1);
        s->y = clampInt(s->y, 0, rows_ - 1);
    }
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

void TerminalScreen::moveTo(int x, int y) {
    x_ = clampInt(x, 0, cols_ - 1);
    y_ = clampInt(y, 0, rows_ - 1);
    wrapPending_ = false;
}

void TerminalScreen::scrollUp(int n) {
    n = std::min(n, bottom_ - top_ + 1);
    if (n <= 0) return;
    Grid& g = grid();
    std::rotate(g.begin() + top_, g.begin() + top_ + n, g.begin() + bottom_ + 1);
    for (int r = bottom_ - n + 1; r <= bottom_; r++) g[r].assign(cols_, blank());
}

void TerminalScreen::scrollDown(int n) {
    n = std::min(n, bottom_ - top_ + 1);
    if (n <= 0) return;
    Grid& g = grid();
    std::rotate(g.begin() + top_, g.begin() + bottom_ + 1 - n, g.begin() + bottom_ + 1);
    for (int r = top_; r < top_ + n; r++) g[r].assign(cols_, blank());
}

void TerminalScreen::lineFeed() {
    if (y_ == bottom_) scrollUp(1);
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
        if (x >= 0) g[y_][x].ch += utf8;
        return;
    }
    if (wrapPending_ && autowrap_) {
        x_ = 0;
        lineFeed();
    }
    wrapPending_ = false;
    if (w == 2 && x_ == cols_ - 1) {
        if (autowrap_) {                     // doesn't fit: wrap it whole
            eraseCells(y_, x_, x_ + 1);
            x_ = 0;
            lineFeed();
        } else {
            w = 1;                           // no room and no wrap: truncate
            utf8 = " ";
        }
    }
    Row& row = g[y_];
    if (insertMode_) {
        clearWide(y_, x_);
        row.insert(row.begin() + x_, w, blank());
        row.resize(cols_);
        if (row[cols_ - 1].width == 2) row[cols_ - 1] = blank();
    }
    clearWide(y_, x_);
    if (w == 2) clearWide(y_, x_ + 1);
    TermCell c;
    c.ch = utf8;
    c.style = style_;
    c.width = (uint8_t)w;
    row[x_] = c;
    if (w == 2) {
        TermCell right;
        right.ch = "";
        right.style = style_;
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
            for (Row& r : alt_) r.assign(cols_, b);
        }
    } else {
        if (clearAlt) {
            for (Row& r : alt_) r.assign(cols_, TermCell());
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
        for (Row& r : grid())
            for (TermCell& c : r) { c = TermCell(); c.ch = "E"; }
        moveTo(0, 0);
    }
}

void TerminalScreen::osc(const std::string& body) {
    if (body.rfind("0;", 0) == 0 || body.rfind("2;", 0) == 0)
        title_ = body.substr(2);
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
        default: break;                          // mouse modes and the rest
        }
    }
}

void TerminalScreen::csi(const std::string& csi, unsigned char f) {
    char lead = csi.empty() ? 0 : csi[0];
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
        clearWide(y_, x_);
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
        if (raw == 0) {
            eraseCells(y_, x_, cols_);
            for (int r = y_ + 1; r < rows_; r++) eraseCells(r, 0, cols_);
        } else if (raw == 1) {
            for (int r = 0; r < y_; r++) eraseCells(r, 0, cols_);
            eraseCells(y_, 0, x_ + 1);
        } else if (raw == 2 || raw == 3) {
            for (int r = 0; r < rows_; r++) eraseCells(r, 0, cols_);
        }
        break;
    case 'K':                                    // EL
        wrapPending_ = false;
        if (raw == 0) eraseCells(y_, x_, cols_);
        else if (raw == 1) eraseCells(y_, 0, x_ + 1);
        else if (raw == 2) eraseCells(y_, 0, cols_);
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
    case TermKey::Enter: return meta + "\r";
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
    // A paste must not be able to end the bracket early.
    std::string safe;
    const std::string endMark = "\x1b[201~";
    for (size_t i = 0; i < body.size(); i++) {
        if (body.compare(i, endMark.size(), endMark) == 0) { i += endMark.size() - 1; continue; }
        safe += body[i];
    }
    return "\x1b[200~" + safe + endMark;
}
