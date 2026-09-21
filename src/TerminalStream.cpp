// TerminalStream.cpp — see TerminalStream.h. The escape-sequence states follow
// the usual DEC/ANSI parser shape (Paul Williams, vt100.net). TermParser
// recognizes every sequence; TerminalStream, the log model, acts on SGR and
// in-line cursor movement and reads OSC 133 (prompt/command marks) and OSC 7
// (working directory). TerminalScreen reads the same callbacks as a grid.
#include "TerminalStream.h"
#include <algorithm>
#include <cstdlib>

namespace {

const size_t kMaxOsc = 8192;   // a runaway OSC is abandoned past this size
const size_t kMaxCsi = 64;     // longer CSI parameter lists are ignored
const char kReplacement[] = "\xEF\xBF\xBD";   // U+FFFD

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexValue(s[i + 1]), lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

} // namespace

// ------------------------------------------------------------ char width
int termCharWidth(uint32_t cp) {
    // Characters that take no column of their own: combining marks, joiners,
    // variation selectors, emoji skin-tone modifiers and tag characters.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
        (cp >= 0xE0020 && cp <= 0xE007F))
        return 0;
    if (cp < 0x1100) return 1;
    // East Asian Wide and Fullwidth ranges, and the emoji that terminals
    // (and wcwidth) draw two columns wide.
    static const uint32_t wide[][2] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
        {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
        {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
        {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
        {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
        {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
        {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
        {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
        {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
        {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004},
        {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
        {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
        {0x1F7E0, 0x1F7EB}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
        {0x20000, 0x3FFFD},
    };
    for (const auto& r : wide) {
        if (cp < r[0]) break;
        if (cp <= r[1]) return 2;
    }
    return 1;
}

// --------------------------------------------------------------- colors
uint32_t TermColor::rgb() const {
    if (kind == RGB) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (index < 16) {
        // Tuned for the panel's dark background (VS Code's dark terminal).
        static const uint32_t base[16] = {
            0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC,
            0x11A8CD, 0xE5E5E5, 0x666666, 0xF14C4C, 0x23D18B, 0xF5F543,
            0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
        };
        return base[index];
    }
    if (index < 232) {   // 6x6x6 color cube
        static const uint8_t level[6] = {0, 95, 135, 175, 215, 255};
        int i = index - 16;
        return ((uint32_t)level[i / 36] << 16) |
               ((uint32_t)level[(i / 6) % 6] << 8) | level[i % 6];
    }
    uint32_t gray = 8 + 10 * (index - 232);   // 24-step gray ramp
    return (gray << 16) | (gray << 8) | gray;
}

// ------------------------------------------------------------------ SGR
void applySgr(TermStyle& style, const std::string& csi) {
    auto params = TermParser::params(csi);
    for (size_t i = 0; i < params.size(); i++) {
        const std::vector<int>& p = params[i];
        int code = p[0] < 0 ? 0 : p[0];

        if (code == 38 || code == 48) {
            // Extended color, either "38;5;n" / "38;2;r;g;b" or the colon
            // forms "38:5:n" / "38:2:[colorspace]:r:g:b".
            std::vector<int> args;
            if (p.size() > 1) {
                args.assign(p.begin() + 1, p.end());
                if (args.size() == 5 && args[0] == 2) args.erase(args.begin() + 1);
            } else {
                size_t want = 1;
                if (i + 1 < params.size())
                    want = params[i + 1][0] == 5 ? 2 : params[i + 1][0] == 2 ? 4 : 1;
                for (size_t k = 1; k <= want && i + k < params.size(); k++)
                    args.push_back(params[i + k][0]);
                i += args.size();
            }
            TermColor c;
            if (args.size() >= 2 && args[0] == 5 && args[1] >= 0 && args[1] <= 255) {
                c.kind = TermColor::Indexed;
                c.index = (uint8_t)args[1];
            } else if (args.size() >= 4 && args[0] == 2) {
                c.kind = TermColor::RGB;
                c.r = (uint8_t)std::min(std::max(args[1], 0), 255);
                c.g = (uint8_t)std::min(std::max(args[2], 0), 255);
                c.b = (uint8_t)std::min(std::max(args[3], 0), 255);
            } else {
                continue;                        // malformed: ignore
            }
            (code == 38 ? style.fg : style.bg) = c;
            continue;
        }

        auto indexed = [](int idx) {
            TermColor c; c.kind = TermColor::Indexed; c.index = (uint8_t)idx;
            return c;
        };
        if (code == 0) style = TermStyle();
        else if (code == 1) style.bold = true;
        else if (code == 2) style.dim = true;
        else if (code == 3) style.italic = true;
        else if (code == 4) style.underline = p.size() < 2 || p[1] != 0;
        else if (code == 7) style.inverse = true;
        else if (code == 9) style.strike = true;
        else if (code == 21) style.underline = true;
        else if (code == 22) style.bold = style.dim = false;
        else if (code == 23) style.italic = false;
        else if (code == 24) style.underline = false;
        else if (code == 27) style.inverse = false;
        else if (code == 29) style.strike = false;
        else if (code >= 30 && code <= 37) style.fg = indexed(code - 30);
        else if (code == 39) style.fg = TermColor();
        else if (code >= 40 && code <= 47) style.bg = indexed(code - 40);
        else if (code == 49) style.bg = TermColor();
        else if (code >= 90 && code <= 97) style.fg = indexed(code - 90 + 8);
        else if (code >= 100 && code <= 107) style.bg = indexed(code - 100 + 8);
        // blink, hidden, fonts and the rest: ignored
    }
}

// --------------------------------------------------------------- parser
std::vector<std::vector<int>> TermParser::params(const std::string& s) {
    std::vector<std::vector<int>> params(1, std::vector<int>(1, -1));
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            int& v = params.back().back();
            v = (v < 0 ? 0 : v);
            if (v < 100000) v = v * 10 + (c - '0');
        } else if (c == ';') {
            params.push_back(std::vector<int>(1, -1));
        } else if (c == ':') {
            params.back().push_back(-1);
        }
    }
    return params;
}

// Complete the character in utf8_. `abandon` means it was cut short by a
// byte that can't continue it, so it becomes U+FFFD.
void TermParser::finishUtf8(bool abandon, Handler& h) {
    if (utf8_.empty()) return;
    const unsigned char* b = (const unsigned char*)utf8_.data();
    uint32_t cp = 0;
    bool ok = !abandon && (int)utf8_.size() == utf8Need_;
    if (ok) {
        switch (utf8Need_) {
        case 2: cp = ((b[0] & 0x1F) << 6) | (b[1] & 0x3F); break;
        case 3: cp = ((b[0] & 0x0F) << 12) | ((b[1] & 0x3F) << 6) | (b[2] & 0x3F);
                ok = cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF); break;
        case 4: cp = ((b[0] & 0x07) << 18) | ((b[1] & 0x3F) << 12) |
                     ((b[2] & 0x3F) << 6) | (b[3] & 0x3F);
                ok = cp >= 0x10000 && cp <= 0x10FFFF; break;
        }
    }
    std::string done;
    done.swap(utf8_);
    utf8Need_ = 0;
    if (ok) h.print(cp, done);
    else    h.print(0xFFFD, kReplacement);
}

void TermParser::feed(const char* data, size_t length, Handler& h) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)data[i];

        // A character in progress must be continued by a continuation byte.
        if (utf8Need_ && state_ == State::Ground) {
            if ((c & 0xC0) == 0x80) {
                utf8_ += (char)c;
                if ((int)utf8_.size() == utf8Need_) finishUtf8(false, h);
                i++;
                continue;
            }
            finishUtf8(true, h);         // cut short; reprocess this byte
        }

        switch (state_) {
        case State::Ground:
            if (c == 0x1B) state_ = State::Escape;
            else if (c < 0x20 || c == 0x7F) h.control(c);
            else if (c < 0x80) h.print(c, std::string(1, (char)c));
            else {
                int need = (c >= 0xC2 && c <= 0xDF) ? 2
                         : (c >= 0xE0 && c <= 0xEF) ? 3
                         : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
                if (need) { utf8_.assign(1, (char)c); utf8Need_ = need; }
                else h.print(0xFFFD, kReplacement);   // invalid lead
            }
            break;

        case State::Escape:
            if (c == '[') { state_ = State::Csi; seq_.clear(); csiOverflow_ = false; }
            else if (c == ']') { state_ = State::Osc; osc_.clear(); }
            else if (c == 'P' || c == 'X' || c == '^' || c == '_')
                state_ = State::String;           // DCS, SOS, PM, APC
            else if (c >= 0x20 && c <= 0x2F) {
                seq_.assign(1, (char)c);
                state_ = State::EscapeIntermediate;
            }
            else if (c == 0x1B) state_ = State::Escape;
            else if (c < 0x20) h.control(c);      // executed, still in ESC
            else { state_ = State::Ground; h.esc("", c); }   // e.g. ESC 7
            break;

        case State::EscapeIntermediate:           // e.g. ESC ( B
            if (c >= 0x30 && c <= 0x7E) { state_ = State::Ground; h.esc(seq_, c); }
            else if (c >= 0x20 && c <= 0x2F) { if (seq_.size() < 4) seq_ += (char)c; }
            else if (c == 0x1B) state_ = State::Escape;
            else state_ = State::Ground;
            break;

        case State::Csi:                          // ESC [ params inter final
            if (c >= 0x40 && c <= 0x7E) {
                state_ = State::Ground;
                if (!csiOverflow_) h.csi(seq_, c);
            }
            else if (c == 0x1B) state_ = State::Escape;
            else if (c >= 0x20 && c <= 0x3F) {
                if (seq_.size() < kMaxCsi) seq_ += (char)c;
                else csiOverflow_ = true;
            }
            else if (c == 0x18 || c == 0x1A) state_ = State::Ground;  // CAN, SUB
            // other controls inside CSI are ignored
            break;

        case State::Osc:                          // ESC ] body (BEL | ESC \)
            if (c == 0x07) { state_ = State::Ground; h.osc(osc_); osc_.clear(); }
            else if (c == 0x1B) state_ = State::OscEscape;
            else if (osc_.size() < kMaxOsc) osc_ += (char)c;
            else { osc_.clear(); state_ = State::String; }  // give up on it
            break;

        case State::OscEscape:
            if (c == '\\') { state_ = State::Ground; h.osc(osc_); osc_.clear(); }
            else {                                // ESC started something new
                osc_.clear();
                state_ = State::Escape;
                continue;                         // reprocess this byte
            }
            break;

        case State::String:                       // skip until BEL or ESC \ .
            if (c == 0x07) state_ = State::Ground;
            else if (c == 0x1B) state_ = State::StringEscape;
            break;

        case State::StringEscape:
            state_ = (c == '\\') ? State::Ground : State::String;
            break;
        }
        i++;
    }
}

// ----------------------------------------------------------- line model
struct TerminalStream::Sink : TermParser::Handler {
    TerminalStream& s;
    std::vector<TermEvent>& out;
    Sink(TerminalStream& s, std::vector<TermEvent>& out) : s(s), out(out) {}
    void print(uint32_t cp, const std::string& utf8) override {
        if (s.alt_) return;
        s.printCodepoint(cp, utf8);
        if (s.line_.size() >= kMaxLineCells) s.newline(out);
    }
    void control(unsigned char c) override { s.control(c, out); }
    void csi(const std::string& p, unsigned char f) override { s.finishCsi(p, f); }
    void esc(const std::string&, unsigned char) override {}
    void osc(const std::string& body) override { s.finishOsc(body, out); }
};

void TerminalStream::breakLine() {
    line_.clear();
    col_ = 0;
    dirty_ = false;
}

void TerminalStream::moveTo(size_t col) {
    col_ = std::min(col, kMaxLineCells - 1);
}

void TerminalStream::printCodepoint(uint32_t cp, const std::string& utf8) {
    if (termCharWidth(cp) == 0 && col_ > 0 && col_ - 1 < line_.size()) {
        line_[col_ - 1].ch += utf8;   // attach to the character before
        dirty_ = true;
        return;
    }
    Cell cell{utf8, style_};
    if (col_ < line_.size()) {
        line_[col_] = cell;           // overwrite, like a terminal does
    } else {
        Cell blank{" ", TermStyle()};
        while (line_.size() < col_) line_.push_back(blank);
        line_.push_back(cell);
    }
    col_++;
    dirty_ = true;
}

void TerminalStream::flushLine(std::vector<TermEvent>& out) {
    if (!dirty_) return;
    TermEvent e;
    e.kind = TermEvent::Line;
    for (const Cell& c : line_) {
        if (e.runs.empty() || e.runs.back().style != c.style)
            e.runs.push_back(TermRun{"", c.style});
        e.runs.back().text += c.ch;
    }
    out.push_back(e);
    dirty_ = false;
}

void TerminalStream::newline(std::vector<TermEvent>& out) {
    dirty_ = true;   // an empty line still ends
    flushLine(out);
    out.back().ended = true;
    breakLine();
}

void TerminalStream::control(unsigned char c, std::vector<TermEvent>& out) {
    if (alt_) return;                 // the grid shows the alternate screen
    if (c == '\n') newline(out);
    else if (c == '\r') col_ = 0;
    else if (c == '\b') { if (col_ > 0) col_--; }
    else if (c == '\t') {
        size_t stop = (col_ / 8 + 1) * 8;
        if (stop > line_.size()) {    // tabs past the end fill blanks
            Cell blank{" ", TermStyle()};
            while (line_.size() < stop) line_.push_back(blank);
            dirty_ = true;
        }
        moveTo(stop);
        if (line_.size() >= kMaxLineCells) newline(out);
    }
    // other controls: ignored
}

// ------------------------------------------------------------ sequences
void TerminalStream::finishCsi(const std::string& csi, unsigned char final) {
    // The alternate screen switches are the one private mode that matters
    // here: what is drawn there does not belong in the log.
    if (csi.size() > 1 && csi[0] == '?' && (final == 'h' || final == 'l')) {
        for (const auto& p : TermParser::params(csi.substr(1))) {
            if (p[0] == 1049 || p[0] == 1047 || p[0] == 47) {
                alt_ = final == 'h';
                if (!alt_) style_ = TermStyle();   // leave its colors behind
            }
        }
        return;
    }
    // Other private (e.g. "?25l") or intermediate-byte sequences: ignored.
    for (char c : csi)
        if (c == '?' || c == '>' || c == '<' || c == '=' || (c >= 0x20 && c <= 0x2F))
            return;
    if (final == 'm') { applySgr(style_, csi); return; }
    if (alt_) return;

    auto params = TermParser::params(csi);
    int n = params[0][0];
    size_t count = n <= 0 ? 1 : (size_t)n;   // movement defaults to 1
    switch (final) {
    case 'C':                                // cursor forward
        moveTo(col_ + count);
        break;
    case 'D':                                // cursor back
        col_ = count > col_ ? 0 : col_ - count;
        break;
    case 'G': case '`':                      // cursor to column (1-based)
        moveTo(count - 1);
        break;
    case 'K': {                              // erase in line
        int mode = n < 0 ? 0 : n;
        if (mode == 0) {
            if (col_ < line_.size()) { line_.resize(col_); dirty_ = true; }
        } else if (mode == 1) {
            for (size_t i = 0; i <= col_ && i < line_.size(); i++)
                line_[i] = Cell{" ", TermStyle()};
            dirty_ = true;
        } else if (mode == 2) {
            line_.clear();
            dirty_ = true;
        }
        break;
    }
    case 'X':                                // erase characters
        for (size_t i = col_; i < col_ + count && i < line_.size(); i++) {
            line_[i] = Cell{" ", TermStyle()};
            dirty_ = true;
        }
        break;
    default:                                 // other lines / screen: ignored
        break;
    }
}

void TerminalStream::finishOsc(const std::string& s, std::vector<TermEvent>& out) {
    if (s.rfind("133;", 0) == 0 && s.size() >= 5) {
        TermEvent e;
        switch (s[4]) {
            case 'A': e.kind = TermEvent::PromptStart; break;
            case 'C': e.kind = TermEvent::CommandStart; break;
            case 'D':
                e.kind = TermEvent::CommandEnd;
                if (s.size() > 6 && s[5] == ';') e.status = std::atoi(s.c_str() + 6);
                break;
            default: return;                 // B and others: nothing to do
        }
        flushLine(out);                      // keep events in order
        out.push_back(e);
    } else if (s.rfind("7;", 0) == 0) {
        // "7;file://host/path" — the path starts at the first '/' after the
        // host. A bare "7;/path" is accepted too.
        std::string url = s.substr(2);
        size_t pathStart = 0;
        if (url.rfind("file://", 0) == 0) pathStart = url.find('/', 7);
        if (pathStart != std::string::npos && pathStart < url.size() &&
            url[pathStart] == '/') {
            flushLine(out);
            TermEvent e; e.kind = TermEvent::Directory;
            e.text = percentDecode(url.substr(pathStart));
            out.push_back(e);
        }
    }
}

// ------------------------------------------------------------------ feed
std::vector<TermEvent> TerminalStream::feed(const char* data, size_t length) {
    std::vector<TermEvent> out;
    Sink sink(*this, out);
    parser_.feed(data, length, sink);
    flushLine(out);
    return out;
}
