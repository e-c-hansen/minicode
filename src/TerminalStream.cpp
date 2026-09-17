// TerminalStream.cpp — see TerminalStream.h. The escape-sequence states follow
// the usual DEC/ANSI parser shape (Paul Williams, vt100.net), trimmed to what
// a log view needs: recognize every sequence so none leaks into the text, act
// on SGR and in-line cursor movement, and read OSC 133 (prompt/command marks)
// and OSC 7 (working directory).
#include "TerminalStream.h"
#include <algorithm>
#include <cstdlib>

namespace {

const size_t kMaxOsc = 8192;   // a runaway OSC is abandoned past this size
const size_t kMaxCsi = 64;     // longer CSI parameter lists are ignored
const char kReplacement[] = "\xEF\xBF\xBD";   // U+FFFD

// Characters that take no column of their own: combining marks, joiners,
// variation selectors, emoji skin-tone modifiers and tag characters.
bool isZeroWidth(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0020 && cp <= 0xE007F);
}

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

// Split CSI parameters on ';'. Each parameter keeps its ':' sub-parameters.
// Empty values are -1 (meaning "default").
std::vector<std::vector<int>> parseParams(const std::string& s) {
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

} // namespace

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

// ----------------------------------------------------------- line model
void TerminalStream::breakLine() {
    line_.clear();
    col_ = 0;
    dirty_ = false;
}

void TerminalStream::moveTo(size_t col) {
    col_ = std::min(col, kMaxLineCells - 1);
}

void TerminalStream::printCodepoint(uint32_t cp, const std::string& utf8) {
    if (isZeroWidth(cp) && col_ > 0 && col_ - 1 < line_.size()) {
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

// ------------------------------------------------------------------ UTF-8
// Complete the character in utf8_. `abandon` means it was cut short by a
// byte that can't continue it, so it becomes U+FFFD.
void TerminalStream::finishUtf8(bool abandon) {
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
    if (ok) printCodepoint(cp, utf8_);
    else    printCodepoint(0xFFFD, kReplacement);
    utf8_.clear();
    utf8Need_ = 0;
}

// ------------------------------------------------------------ sequences
void TerminalStream::finishCsi(unsigned char final) {
    if (csiOverflow_) return;
    // Private (e.g. "?25l") or intermediate-byte sequences: nothing we act on.
    for (char c : csi_)
        if (c == '?' || c == '>' || c == '<' || c == '=' || (c >= 0x20 && c <= 0x2F))
            return;
    if (final == 'm') { applySgr(); return; }

    auto params = parseParams(csi_);
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

void TerminalStream::applySgr() {
    auto params = parseParams(csi_);
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
            (code == 38 ? style_.fg : style_.bg) = c;
            continue;
        }

        auto indexed = [](int idx) {
            TermColor c; c.kind = TermColor::Indexed; c.index = (uint8_t)idx;
            return c;
        };
        if (code == 0) style_ = TermStyle();
        else if (code == 1) style_.bold = true;
        else if (code == 2) style_.dim = true;
        else if (code == 3) style_.italic = true;
        else if (code == 4) style_.underline = p.size() < 2 || p[1] != 0;
        else if (code == 7) style_.inverse = true;
        else if (code == 9) style_.strike = true;
        else if (code == 21) style_.underline = true;
        else if (code == 22) style_.bold = style_.dim = false;
        else if (code == 23) style_.italic = false;
        else if (code == 24) style_.underline = false;
        else if (code == 27) style_.inverse = false;
        else if (code == 29) style_.strike = false;
        else if (code >= 30 && code <= 37) style_.fg = indexed(code - 30);
        else if (code == 39) style_.fg = TermColor();
        else if (code >= 40 && code <= 47) style_.bg = indexed(code - 40);
        else if (code == 49) style_.bg = TermColor();
        else if (code >= 90 && code <= 97) style_.fg = indexed(code - 90 + 8);
        else if (code >= 100 && code <= 107) style_.bg = indexed(code - 100 + 8);
        // blink, hidden, fonts and the rest: ignored
    }
}

void TerminalStream::finishOsc(std::vector<TermEvent>& out) {
    const std::string& s = osc_;
    if (s.rfind("133;", 0) == 0 && s.size() >= 5) {
        TermEvent e;
        switch (s[4]) {
            case 'A': e.kind = TermEvent::PromptStart; break;
            case 'C': e.kind = TermEvent::CommandStart; break;
            case 'D':
                e.kind = TermEvent::CommandEnd;
                if (s.size() > 6 && s[5] == ';') e.status = std::atoi(s.c_str() + 6);
                break;
            default: osc_.clear(); return;   // B and others: nothing to do
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
    osc_.clear();
}

// ------------------------------------------------------------------ feed
std::vector<TermEvent> TerminalStream::feed(const char* data, size_t length) {
    std::vector<TermEvent> out;
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)data[i];

        // A character in progress must be continued by a continuation byte.
        if (utf8Need_ && state_ == State::Ground) {
            if ((c & 0xC0) == 0x80) {
                utf8_ += (char)c;
                if ((int)utf8_.size() == utf8Need_) finishUtf8(false);
                i++;
                continue;
            }
            finishUtf8(true);            // cut short; reprocess this byte
        }

        switch (state_) {
        case State::Ground:
            if (c == 0x1B) state_ = State::Escape;
            else if (c == '\n') newline(out);
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
            }
            else if (c < 0x20 || c == 0x7F) { /* other controls: ignore */ }
            else if (c < 0x80) printCodepoint(c, std::string(1, (char)c));
            else {
                int need = (c >= 0xC2 && c <= 0xDF) ? 2
                         : (c >= 0xE0 && c <= 0xEF) ? 3
                         : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
                if (need) { utf8_.assign(1, (char)c); utf8Need_ = need; }
                else printCodepoint(0xFFFD, kReplacement);   // invalid lead
            }
            if (line_.size() >= kMaxLineCells) newline(out);
            break;

        case State::Escape:
            if (c == '[') { state_ = State::Csi; csi_.clear(); csiOverflow_ = false; }
            else if (c == ']') { state_ = State::Osc; osc_.clear(); }
            else if (c == 'P' || c == 'X' || c == '^' || c == '_')
                state_ = State::String;           // DCS, SOS, PM, APC
            else if (c >= 0x20 && c <= 0x2F) state_ = State::EscapeIntermediate;
            else if (c == 0x1B) state_ = State::Escape;
            else state_ = State::Ground;          // two-byte escape, e.g. ESC 7
            break;

        case State::EscapeIntermediate:           // e.g. ESC ( B
            if (c >= 0x30 && c <= 0x7E) state_ = State::Ground;
            else if (c == 0x1B) state_ = State::Escape;
            else if (c < 0x20 || c > 0x2F) state_ = State::Ground;
            break;

        case State::Csi:                          // ESC [ params inter final
            if (c >= 0x40 && c <= 0x7E) { finishCsi(c); state_ = State::Ground; }
            else if (c == 0x1B) state_ = State::Escape;
            else if (c >= 0x20 && c <= 0x3F) {
                if (csi_.size() < kMaxCsi) csi_ += (char)c;
                else csiOverflow_ = true;
            }
            // controls inside CSI are ignored
            break;

        case State::Osc:                          // ESC ] body (BEL | ESC \)
            if (c == 0x07) { finishOsc(out); state_ = State::Ground; }
            else if (c == 0x1B) state_ = State::OscEscape;
            else if (osc_.size() < kMaxOsc) osc_ += (char)c;
            else { osc_.clear(); state_ = State::String; }  // give up on it
            break;

        case State::OscEscape:
            if (c == '\\') { finishOsc(out); state_ = State::Ground; }
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
    flushLine(out);
    return out;
}
