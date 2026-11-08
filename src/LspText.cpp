// LspText.cpp — see LspClient.h. Text and positions: LSP positions (lines
// and UTF-16 units) to offsets and back, PositionIndex, UTF-8 and UTF-16
// conversion, file URIs, Position and Range to and from JSON, and lineOf.
// Everything else that reads a position goes through these.
#include "LspClient.h"
#include "LspInternal.h"
#include <algorithm>

namespace Lsp {

// ------------------------------------------------------ text and positions
size_t offsetForPosition(const std::u16string& text, Position p) {
    size_t i = 0;
    int line = 0;
    while (line < p.line) {
        while (i < text.size() && text[i] != u'\n' && text[i] != u'\r') i++;
        if (i >= text.size()) return text.size();
        if (text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
        i++;
        line++;
    }
    size_t end = i;
    while (end < text.size() && text[end] != u'\n' && text[end] != u'\r') end++;
    size_t ch = p.character < 0 ? 0 : (size_t)p.character;
    return std::min(i + ch, end);
}

Position positionForOffset(const std::u16string& text, size_t offset) {
    offset = std::min(offset, text.size());
    Position p;
    size_t lineStart = 0;
    for (size_t i = 0; i < offset; i++) {
        char16_t c = text[i];
        if (c == u'\n' || c == u'\r') {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') {
                if (i + 1 >= offset) break;   // offset sits between \r and \n
                i++;
            }
            p.line++;
            lineStart = i + 1;
        }
    }
    p.character = (int)(offset - lineStart);
    return p;
}

PositionIndex::PositionIndex(const std::u16string& text) : text_(text) {
    starts_.push_back(0);
    for (size_t i = 0; i < text.size(); i++) {
        char16_t c = text[i];
        if (c != u'\n' && c != u'\r') continue;
        ends_.push_back(i);
        if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
        starts_.push_back(i + 1);
    }
    ends_.push_back(text.size());
}

size_t PositionIndex::offset(Position p) const {
    size_t line = p.line < 0 ? 0 : (size_t)p.line;
    if (line >= starts_.size()) return text_.size();
    size_t ch = p.character < 0 ? 0 : (size_t)p.character;
    return std::min(starts_[line] + ch, ends_[line]);
}

Position PositionIndex::position(size_t offset) const {
    offset = std::min(offset, text_.size());
    // The last line starting at or before the offset. Between \r and \n is
    // not a line start, so it stays on the \r's line, as positionForOffset.
    auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    size_t line = (size_t)(it - starts_.begin()) - 1;
    return Position{(int)line, (int)(offset - starts_[line])};
}

std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out += (char16_t)0xFFFD; i++; continue; }
        if (i + n > s.size()) { out += (char16_t)0xFFFD; break; }
        bool bad = false;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out += (char16_t)0xFFFD; i++; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

std::string toUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string uriFromPath(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (char ch : path) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string pathFromUri(const std::string& uri) {
    if (lower(uri.substr(0, 7)) != "file://") return "";
    std::string rest = uri.substr(7);
    if (lower(rest.substr(0, 9)) == "localhost") rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/') return "";
    auto val = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '%' && i + 2 < rest.size() &&
            val(rest[i + 1]) >= 0 && val(rest[i + 2]) >= 0) {
            out += (char)(val(rest[i + 1]) * 16 + val(rest[i + 2]));
            i += 2;
        } else if (rest[i] == '?' || rest[i] == '#') {
            break;   // query or fragment: not part of the path
        } else {
            out += rest[i];
        }
    }
    return out;
}

Json positionJson(Position p) {
    return Json::object({{"line", p.line}, {"character", p.character}});
}

Position parsePosition(const Json& j) {
    Position p;
    p.line = (int)j["line"].asInt();
    p.character = (int)j["character"].asInt();
    return p;
}

Range parseRange(const Json& j) {
    return Range{parsePosition(j["start"]), parsePosition(j["end"])};
}

// ------------------------------------------------------------- editing
bool positionBefore(Position a, Position b) {
    return a.line < b.line || (a.line == b.line && a.character < b.character);
}

std::u16string lineOf(const std::u16string& text, int line) {
    return lineOf(PositionIndex(text), line);
}

std::u16string lineOf(const PositionIndex& index, int line) {
    if (line < 0 || (size_t)line >= index.lineCount()) return u"";
    const std::u16string& text = index.text();
    size_t start = index.offset(Position{line, 0});
    size_t end = start;
    while (end < text.size() && text[end] != u'\n' && text[end] != u'\r') end++;
    return text.substr(start, end - start);
}

}  // namespace Lsp
