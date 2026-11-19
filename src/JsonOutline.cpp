// JsonOutline.cpp — see JsonOutline.h.
#include "JsonOutline.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace JsonOutline {

namespace {

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
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

// Four hex digits at `at`, or -1.
int hex4(const std::string &s, size_t at) {
    if (at + 4 > s.size()) return -1;
    int v = 0;
    for (size_t i = at; i < at + 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// The UTF-8 character at s[i]: its code point and length, or U+FFFD and
// 1 for a byte that does not start a whole, shortest-form character.
uint32_t utf8At(const std::string &s, size_t i, size_t *len) {
    const unsigned char c = (unsigned char)s[i];
    *len = 1;
    if (c < 0x80) return c;
    const size_t n = c >= 0xF5 ? 0 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (n == 0 || i + n > s.size()) return 0xFFFD;
    uint32_t cp = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (size_t k = 1; k < n; k++) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD;
    *len = n;
    return cp;
}

const char *const kEllipsis = "\xE2\x80\xA6";   // …

struct Parser {
    const std::string &s;
    size_t pos = 0;
    size_t maxNodes;
    Outline &out;
    bool jsonc;
    bool failed = false;

    Parser(const std::string &text, size_t max, Outline &o, bool comments)
        : s(text), maxNodes(max), out(o), jsonc(comments) {}

    bool fail(const char *msg) {
        if (!failed) {
            failed = true;
            out.error = msg;
            out.errorOffset = pos;
        }
        return false;
    }

    // Whitespace, and with jsonc comments too. An unclosed /* fails.
    bool ws() {
        while (pos < s.size()) {
            const char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos++;
            } else if (jsonc && c == '/' && pos + 1 < s.size() && s[pos + 1] == '/') {
                while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') pos++;
            } else if (jsonc && c == '/' && pos + 1 < s.size() && s[pos + 1] == '*') {
                const size_t close = s.find("*/", pos + 2);
                if (close == std::string::npos) return fail("Unterminated comment");
                pos = close + 2;
            } else {
                break;
            }
        }
        return true;
    }

    int hex4(size_t at) const { return JsonOutline::hex4(s, at); }

    // A string at pos (the opening quote). Decodes up to `keep` bytes into
    // `decoded` (all of it when keep is SIZE_MAX); `cut` says more followed.
    bool string(std::string *decoded, size_t keep, bool *cut) {
        pos++;   // "
        if (cut) *cut = false;
        int pending = 0;   // continuation bytes still owed by the last character kept
        while (true) {
            if (pos >= s.size()) return fail("Unterminated string");
            unsigned char c = (unsigned char)s[pos];
            if (c == '"') { pos++; return true; }
            if (c < 0x20) return fail("Control character in string");
            // Past `keep`, a character already begun is still finished.
            const bool cont = (c & 0xC0) == 0x80;
            const bool room = decoded && (cont ? pending > 0 : decoded->size() < keep);
            if (decoded && !room && cut) *cut = true;
            if (c != '\\') {
                if (room) {
                    *decoded += (char)c;
                    pending = cont ? pending - 1 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                }
                pos++;
                continue;
            }
            if (pos + 1 >= s.size()) return fail("Unterminated string");
            char e = s[pos + 1];
            uint32_t cp = 0;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u': {
                int h = hex4(pos + 2);
                if (h < 0) return fail("Bad \\u escape");
                cp = (uint32_t)h;
                size_t used = 6;
                if (cp >= 0xD800 && cp <= 0xDBFF && pos + 7 < s.size() && s[pos + 6] == '\\' &&
                    s[pos + 7] == 'u') {
                    int lo = hex4(pos + 8);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                        used = 12;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                if (room) appendUtf8(*decoded, cp);
                pos += used;
                continue;
            }
            default:
                pos++;
                return fail("Bad escape in string");
            }
            if (room) *decoded += (char)cp;
            pos += 2;
        }
    }

    bool number() {
        if (pos < s.size() && s[pos] == '-') pos++;
        if (pos >= s.size() || !(s[pos] >= '0' && s[pos] <= '9')) return fail("Bad number");
        if (s[pos] == '0') pos++;
        else while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            if (pos >= s.size() || !(s[pos] >= '0' && s[pos] <= '9')) return fail("Bad number");
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            if (pos >= s.size() || !(s[pos] >= '0' && s[pos] <= '9')) return fail("Bad number");
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        return true;
    }

    bool literal(const char *word) {
        size_t n = 0;
        while (word[n]) n++;
        if (s.compare(pos, n, word) != 0) return fail("Unexpected text");
        pos += n;
        return true;
    }

    // One value at pos (whitespace skipped); its node's index in *made.
    bool value(long parent, int depth, size_t *made) {
        if (!ws()) return false;
        if (pos >= s.size()) return fail("Unexpected end of text");
        if (depth > 512) return fail("Nested too deeply");
        if (out.nodes.size() >= maxNodes) {
            out.truncated = true;
            return fail("Too many values to outline");
        }
        const size_t me = out.nodes.size();
        out.nodes.emplace_back();
        out.nodes[me].parent = parent;
        out.nodes[me].start = out.nodes[me].memberStart = pos;
        *made = me;
        const char c = s[pos];
        bool ok = true;
        if (c == '{') {
            out.nodes[me].kind = Kind::Object;
            pos++;
            if (!ws()) ok = false;
            else if (pos < s.size() && s[pos] == '}') {
                pos++;
            } else {
                while (true) {
                    if (!ws()) { ok = false; break; }
                    if (jsonc && pos < s.size() && s[pos] == '}' && !out.nodes[me].children.empty()) {
                        pos++;   // a trailing comma
                        break;
                    }
                    if (pos >= s.size() || s[pos] != '"') { ok = fail("Expected a string key"); break; }
                    const size_t keyAt = pos;
                    std::string key;
                    if (!string(&key, SIZE_MAX, nullptr)) { ok = false; break; }
                    if (!ws()) { ok = false; break; }
                    if (pos >= s.size() || s[pos] != ':') { ok = fail("Expected ':'"); break; }
                    pos++;
                    size_t child = 0;
                    const bool good = value((long)me, depth + 1, &child);
                    if (child < out.nodes.size() && child != me && out.nodes[child].parent == (long)me) {
                        out.nodes[child].key = std::move(key);
                        out.nodes[child].isMember = true;
                        out.nodes[child].memberStart = keyAt;
                        out.nodes[me].children.push_back(child);
                    }
                    if (!good) { ok = false; break; }
                    if (!ws()) { ok = false; break; }
                    if (pos < s.size() && s[pos] == ',') { pos++; continue; }
                    if (pos < s.size() && s[pos] == '}') { pos++; break; }
                    ok = fail("Expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            out.nodes[me].kind = Kind::Array;
            pos++;
            if (!ws()) ok = false;
            else if (pos < s.size() && s[pos] == ']') {
                pos++;
            } else {
                long index = 0;
                while (true) {
                    if (jsonc && index > 0) {
                        if (!ws()) { ok = false; break; }
                        if (pos < s.size() && s[pos] == ']') { pos++; break; }   // a trailing comma
                    }
                    size_t child = 0;
                    const bool good = value((long)me, depth + 1, &child);
                    if (child < out.nodes.size() && child != me && out.nodes[child].parent == (long)me) {
                        out.nodes[child].index = index++;
                        out.nodes[me].children.push_back(child);
                    }
                    if (!good) { ok = false; break; }
                    if (!ws()) { ok = false; break; }
                    if (pos < s.size() && s[pos] == ',') { pos++; continue; }
                    if (pos < s.size() && s[pos] == ']') { pos++; break; }
                    ok = fail("Expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            out.nodes[me].kind = Kind::String;
            std::string text;
            bool cut = false;
            ok = string(&text, kPreviewChars * 4, &cut);
            if (cut) text += "\xE2\x80\xA6";   // …
            out.nodes[me].preview = std::move(text);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            out.nodes[me].kind = Kind::Number;
            ok = number();
            // Cut as a string's is: a 300,000-digit number made a label
            // GTK took half a second to lay out.
            const size_t n = pos - out.nodes[me].start;
            out.nodes[me].preview = s.substr(out.nodes[me].start, std::min(n, kPreviewChars * 4));
            if (n > kPreviewChars * 4) out.nodes[me].preview += "\xE2\x80\xA6";   // …
        } else if (c == 't' || c == 'f') {
            out.nodes[me].kind = Kind::Bool;
            ok = literal(c == 't' ? "true" : "false");
            out.nodes[me].preview = c == 't' ? "true" : "false";
        } else if (c == 'n') {
            out.nodes[me].kind = Kind::Null;
            ok = literal("null");
            out.nodes[me].preview = "null";
        } else {
            ok = fail("Unexpected character");
        }
        out.nodes[me].end = pos;
        return ok;
    }
};

// Byte offsets to UTF-16 units, all in one pass over the text.
void fillUtf16(const std::string &s, Outline &o) {
    std::vector<size_t> offs;
    offs.reserve(o.nodes.size() * 3);
    for (const Node &n : o.nodes) {
        offs.push_back(n.start);
        offs.push_back(n.end);
        offs.push_back(n.memberStart);
    }
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    std::vector<size_t> u16(offs.size());
    size_t byte = 0, units = 0;
    for (size_t i = 0; i < offs.size(); i++) {
        const size_t target = std::min(offs[i], s.size());
        while (byte < target) {
            unsigned char c = (unsigned char)s[byte];
            if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
            byte++;
        }
        u16[i] = units;
    }
    auto map = [&](size_t off) {
        size_t i = (size_t)(std::lower_bound(offs.begin(), offs.end(), off) - offs.begin());
        return i < u16.size() ? u16[i] : units;
    };
    for (Node &n : o.nodes) {
        n.start16 = map(n.start);
        n.end16 = map(n.end);
        n.memberStart16 = map(n.memberStart);
    }
}

}  // namespace

Outline parse(const std::string &text, size_t maxNodes, bool jsonc) {
    Outline o;
    Parser p(text, maxNodes, o, jsonc);
    size_t root = 0;
    bool ok = p.value(-1, 0, &root);
    if (ok) {
        ok = p.ws();
        if (ok && p.pos < text.size()) ok = p.fail("Unexpected text after the value");
    }
    o.ok = ok;
    if (!ok) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < o.errorOffset && i < text.size(); i++) {
            unsigned char c = (unsigned char)text[i];
            if (c == '\n') { line++; col = 1; }
            else if ((c & 0xC0) != 0x80) col++;
        }
        o.errorLine = line;
        o.errorColumn = col;
    }
    fillUtf16(text, o);
    return o;
}

bool allowsComments(const std::string &path) {
    auto lower = [](std::string x) {
        for (char &c : x) c = (char)std::tolower((unsigned char)c);
        return x;
    };
    const std::string p = lower(path);
    const size_t slash = p.rfind('/');
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const std::string dir = slash == std::string::npos ? "" : p.substr(0, slash);
    const size_t dirSlash = dir.rfind('/');
    const std::string parent = dirSlash == std::string::npos ? dir : dir.substr(dirSlash + 1);
    auto ends = [](const std::string &x, const std::string &tail) {
        return x.size() >= tail.size() && x.compare(x.size() - tail.size(), tail.size(), tail) == 0;
    };
    auto starts = [](const std::string &x, const std::string &head) { return x.rfind(head, 0) == 0; };
    if (ends(name, ".jsonc") || ends(name, ".code-workspace")) return true;
    if (!ends(name, ".json")) return false;
    if (starts(name, "tsconfig") || starts(name, "jsconfig")) return true;
    return parent == ".vscode" || parent == ".devcontainer";
}

bool checksAsJson(const std::string &path) {
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == "json" || ext == "jsonc" || ext == "geojson" || ext == "har" ||
           ext == "webmanifest" || ext == "code-workspace";
}

namespace {

// A byte offset's zero-based line and UTF-16 column, lines broken by \n,
// \r\n or a lone \r (LSP's rule), from a known earlier place.
struct Place {
    size_t byte = 0, line = 0, column = 0;
};
Place advance(const std::string &s, Place at, size_t to) {
    to = std::min(to, s.size());
    while (at.byte < to) {
        const unsigned char c = (unsigned char)s[at.byte];
        if (c == '\n' || (c == '\r' && !(at.byte + 1 < s.size() && s[at.byte + 1] == '\n'))) {
            at.line++;
            at.column = 0;
        } else if (c != '\r' && (c & 0xC0) != 0x80) {
            at.column += c >= 0xF0 ? 2 : 1;
        }
        at.byte++;
    }
    return at;
}

bool blankByte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

std::vector<Problem> problems(const Outline &o, const std::string &s) {
    if (o.ok || o.truncated) return {};
    // No value at all: blank, or only comments.
    if (o.nodes.empty() && o.errorOffset >= s.size()) return {};
    const size_t at = std::min(o.errorOffset, s.size());
    size_t from = at, to = at;
    std::string message = o.error;
    const char c = at < s.size() ? s[at] : '\0';
    size_t back = at;
    while (back > 0 && blankByte(s[back - 1])) back--;
    if ((c == '}' || c == ']') && back > 0 && s[back - 1] == ',') {
        from = back - 1;
        to = back;
        message = "Trailing comma";
    } else if (c == '/' && at + 1 < s.size() && (s[at + 1] == '/' || s[at + 1] == '*') &&
               o.error != "Unterminated comment") {
        if (s[at + 1] == '/') {
            to = at;
            while (to < s.size() && s[to] != '\n' && s[to] != '\r') to++;
        } else {
            const size_t close = s.find("*/", at + 2);
            to = close == std::string::npos ? s.size() : close + 2;
        }
        message = "Comments are not permitted in JSON";
    } else if (at < s.size() && c != '\n' && c != '\r') {
        // The token there: a word (a misspelled true, a bare key) whole,
        // else one character.
        auto wordByte = [](char b) {
            return std::isalnum((unsigned char)b) || b == '_' || b == '-' || b == '+' ||
                   b == '.' || (unsigned char)b >= 0x80;
        };
        to = at + 1;
        if (wordByte(c))
            while (to < s.size() && wordByte(s[to])) to++;
        while (to < s.size() && ((unsigned char)s[to] & 0xC0) == 0x80) to++;
    }
    const Place a = advance(s, Place{}, from);
    const Place b = advance(s, a, to);
    Problem p;
    p.line = a.line;
    p.column = a.column;
    p.endLine = b.line;
    p.endColumn = b.column;
    p.message = message;
    return {p};
}

std::vector<Problem> problems(const std::string &text, bool jsonc) {
    return problems(parse(text, SIZE_MAX, jsonc), text);
}

long nodeAt16(const Outline &o, size_t offset16) {
    if (o.nodes.empty()) return -1;
    const Node &root = o.nodes[0];
    if (offset16 < root.start16 || offset16 > root.end16) return -1;
    long at = 0;
    while (true) {
        long next = -1;
        for (size_t c : o.nodes[(size_t)at].children) {
            const Node &n = o.nodes[c];
            if (offset16 >= n.memberStart16 && offset16 < n.end16) { next = (long)c; break; }
        }
        if (next < 0) return at;
        at = next;
    }
}

std::string label(const Node &n) {
    if (n.isMember) return n.key;
    if (n.index >= 0) return "[" + std::to_string(n.index) + "]";
    return "root";
}

std::string summary(const Node &n) {
    switch (n.kind) {
    case Kind::Object: return "{" + std::to_string(n.children.size()) + "}";
    case Kind::Array: return "[" + std::to_string(n.children.size()) + "]";
    case Kind::String: return "\"" + n.preview + "\"";
    default: return n.preview;
    }
}

Line oneLine(const std::string &s, size_t maxChars) {
    Line out;
    size_t chars = 0, i = 0;
    out.text.reserve(std::min(s.size(), maxChars * 4) + 3);
    while (i < s.size()) {
        size_t len = 1;
        const uint32_t cp = utf8At(s, i, &len);
        std::string esc;
        switch (cp) {
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            // The rest of C0, DEL, C1 (NEL is a line break to AppKit), and
            // the line and paragraph separators.
            if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029) {
                static const char *hex = "0123456789abcdef";
                esc = "\\u";
                for (int shift = 12; shift >= 0; shift -= 4) esc += hex[(cp >> shift) & 0xF];
            }
        }
        const size_t width = esc.empty() ? 1 : esc.size();
        if (chars + width > maxChars) {
            out.cut = true;
            break;
        }
        chars += width;
        if (!esc.empty()) {
            out.escapes.emplace_back(out.text.size(), out.text.size() + esc.size());
            out.text += esc;
        } else if (cp == 0xFFFD && len == 1) {
            out.text += "\xEF\xBF\xBD";
        } else {
            out.text.append(s, i, len);
        }
        i += len;
    }
    if (out.cut) out.text += kEllipsis;
    out.changed = out.cut || out.text != s;
    return out;
}

std::string decodeString(const std::string &lit, size_t maxChars) {
    std::string out;
    size_t i = !lit.empty() && lit[0] == '"' ? 1 : 0, chars = 0;
    while (i < lit.size()) {
        if (lit[i] == '"') return out;
        if (chars >= maxChars) break;
        chars++;
        if (lit[i] != '\\') {
            size_t len = 1;
            const uint32_t cp = utf8At(lit, i, &len);
            if (cp == 0xFFFD && len == 1) appendUtf8(out, cp);
            else out.append(lit, i, len);
            i += len;
            continue;
        }
        if (i + 1 >= lit.size()) break;
        const char e = lit[i + 1];
        uint32_t cp = (unsigned char)e;
        size_t used = 2;
        switch (e) {
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u': {
            const int h = hex4(lit, i + 2);
            if (h < 0) { cp = 0xFFFD; break; }
            cp = (uint32_t)h;
            used = 6;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 < lit.size() && lit[i + 6] == '\\' &&
                lit[i + 7] == 'u') {
                const int lo = hex4(lit, i + 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                    used = 12;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            break;
        }
        default: break;   // \" \\ \/ and anything else: the character itself
        }
        appendUtf8(out, cp);
        i += used;
    }
    // Cut, or the text stopped before the closing quote: more followed.
    return out + kEllipsis;
}

std::string tooltip(const Node &n, const std::string &value) {
    std::string out;
    if (n.isMember && oneLine(n.key).changed) {
        // The key as it is, cut as a value is.
        size_t i = 0, chars = 0;
        while (i < n.key.size() && chars < kTooltipChars) {
            size_t len = 1;
            utf8At(n.key, i, &len);
            i += len;
            chars++;
        }
        out = n.key.substr(0, i);
        if (i < n.key.size()) out += kEllipsis;
    }
    if (n.kind == Kind::String) {
        const std::string &v = value.empty() ? n.preview : value;
        if (!out.empty() && !v.empty()) out += "\n\n";
        out += v;
    }
    return out;
}

}  // namespace JsonOutline
