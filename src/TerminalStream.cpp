// TerminalStream.cpp — see TerminalStream.h. The escape-sequence states follow
// the usual DEC/ANSI parser shape (Paul Williams, vt100.net), trimmed to what
// a log view needs: recognize every sequence so none leaks into the text, and
// act only on OSC 133 (prompt/command marks) and OSC 7 (working directory).
#include "TerminalStream.h"
#include <cstdlib>

namespace {

const size_t kMaxOsc = 8192;   // a runaway OSC is abandoned past this size

// Length of the UTF-8 sequence introduced by lead byte `c`, or 0 if `c` can't
// start one.
int utf8Length(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 0;
}

// Replace malformed UTF-8 with U+FFFD so the GUI never gets a string it can't
// decode (which would drop the whole chunk).
std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = utf8Length(c);
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; k++)
            ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok && len == 3) {   // no overlongs or UTF-16 surrogates
            unsigned char d = (unsigned char)s[i + 1];
            if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d > 0x9F)) ok = false;
        }
        if (ok && len == 4) {   // no overlongs or code points past U+10FFFF
            unsigned char d = (unsigned char)s[i + 1];
            if ((c == 0xF0 && d < 0x90) || (c == 0xF4 && d > 0x8F)) ok = false;
        }
        if (ok) { out.append(s, i, len); i += len; }
        else    { out += "\xEF\xBF\xBD"; i++; }
    }
    return out;
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

} // namespace

void TerminalStream::flushText(std::vector<TermEvent>& out, bool final) {
    if (text_.empty()) return;
    // At the end of a chunk, keep back a UTF-8 character that isn't complete
    // yet; its remaining bytes are in the next read.
    size_t keep = 0;
    if (!final) {
        size_t n = text_.size();
        for (size_t back = 1; back <= 3 && back <= n; back++) {
            unsigned char c = (unsigned char)text_[n - back];
            if ((c & 0xC0) == 0x80) continue;          // continuation byte
            int len = utf8Length(c);
            if (len > (int)back) keep = back;          // lead byte, incomplete
            break;
        }
    }
    std::string ready = text_.substr(0, text_.size() - keep);
    text_.erase(0, text_.size() - keep);
    if (ready.empty()) return;
    TermEvent e; e.kind = TermEvent::Text; e.text = sanitizeUtf8(ready);
    out.push_back(e);
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
        flushText(out, true);
        out.push_back(e);
    } else if (s.rfind("7;", 0) == 0) {
        // "7;file://host/path" — the path starts at the first '/' after the
        // host. A bare "7;/path" is accepted too.
        std::string url = s.substr(2);
        size_t pathStart = 0;
        if (url.rfind("file://", 0) == 0) pathStart = url.find('/', 7);
        if (pathStart != std::string::npos && pathStart < url.size() &&
            url[pathStart] == '/') {
            flushText(out, true);
            TermEvent e; e.kind = TermEvent::Directory;
            e.text = percentDecode(url.substr(pathStart));
            out.push_back(e);
        }
    }
    osc_.clear();
}

std::vector<TermEvent> TerminalStream::feed(const char* data, size_t length) {
    std::vector<TermEvent> out;
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)data[i];

        // A \r followed by \n is just a line ending (the pty turns every \n
        // into \r\n). Any other \r means "back to the start of the line".
        if (pendingCR_ && state_ == State::Ground) {
            pendingCR_ = false;
            if (c != '\n') {
                flushText(out, true);
                TermEvent e; e.kind = TermEvent::CarriageReturn;
                out.push_back(e);
            }
        }

        switch (state_) {
        case State::Ground:
            if (c == 0x1B) { state_ = State::Escape; }
            else if (c == '\r') { pendingCR_ = true; }
            else if (c == '\n' || c == '\t') { text_ += (char)c; }
            else if (c == '\b') {
                flushText(out, true);
                TermEvent e; e.kind = TermEvent::Backspace; out.push_back(e);
            }
            else if (c < 0x20 || c == 0x7F) { /* other controls: ignore */ }
            else { text_ += (char)c; }
            break;

        case State::Escape:
            if (c == '[') state_ = State::Csi;
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
            if (c >= 0x40 && c <= 0x7E) state_ = State::Ground;
            else if (c == 0x1B) state_ = State::Escape;
            else if (c < 0x20) { /* controls inside CSI are ignored */ }
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
    flushText(out, false);
    return out;
}
