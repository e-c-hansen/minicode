// MarkdownParser.cpp — a compact Markdown parser, pure C++, following
// CommonMark and GitHub's extensions where real files need them.
//
// Two passes. The block pass reads the source a line at a time into blocks
// (paragraphs, ATX and setext headings, fenced and indented code, block
// quotes with GitHub's alerts, nested lists with task boxes, rules, GitHub
// tables, front matter) and classifies every line; MarkdownEdit uses that
// classification, so what the preview shows and what a double-click edits
// agree. Link reference definitions and HTML comments are hidden.
//
// The inline pass turns a block's text into runs: emphasis by CommonMark's
// delimiter rules (so snake_case and 2 * 3 stay as written), ~~strike~~,
// code spans of any backtick count, backslash escapes, entities, inline and
// reference links and images, <autolinks> and bare web addresses, hard
// breaks, and a little inline HTML (<br>, <img>, <b>, <a href>, <kbd>...;
// other tags and comments are dropped, their text kept).
//
// Math is read before any other inline rule, as GitHub and pandoc read it:
// $...$ (the opening $ not followed by a space, the closing one neither
// after a space nor before a digit, so "$5 and $10" stays text), $$...$$ and
// \(...\). \$ is a dollar sign, and a $ inside a code span is not math.
// Display math also comes as a block: $$ (or \[) starting a line and $$ (or
// \]) ending one, or a ```math fence. Nothing inside math is escaped,
// emphasised or linked.
#include "MarkdownParser.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>

namespace {

// ------------------------------------------------------------ characters

std::string encodeUtf8(uint32_t cp) {
    std::string s;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
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

// The code point starting at byte i, and its length in bytes. A stray or
// truncated byte reads as itself, one byte long.
uint32_t decodeAt(const std::string& s, size_t i, size_t* len = nullptr) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { if (len) *len = 1; return c; }
    for (size_t k = 1; k < n; k++) {
        if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
            if (len) *len = 1;
            return c;
        }
        cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    }
    if (len) *len = n;
    return cp;
}

// The code point that ends just before byte i.
uint32_t decodeBefore(const std::string& s, size_t i) {
    size_t k = i - 1;
    while (k > 0 && i - k < 4 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    size_t len = 0;
    uint32_t cp = decodeAt(s, k, &len);
    return k + len == i ? cp : (unsigned char)s[i - 1];
}

bool isSpaceCp(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x0B ||
           c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Punctuation, for the emphasis rules: ASCII punctuation and Unicode's
// punctuation blocks (not symbols such as × or →, as in GitHub's parser).
bool isPunctCp(uint32_t c) {
    if (c < 0x80) return std::ispunct((int)c) != 0;
    return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB ||
           c == 0xBF || (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
           (c >= 0x2E00 && c <= 0x2E7F) || (c >= 0x3001 && c <= 0x3003) ||
           (c >= 0x3008 && c <= 0x3011) || (c >= 0x3014 && c <= 0x301F) ||
           (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
           (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);
}

bool isAsciiPunct(char c) { return (unsigned char)c < 0x80 && std::ispunct((unsigned char)c); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool startsWithNoCase(const std::string& s, size_t at, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (at + n > s.size()) return false;
    for (size_t k = 0; k < n; k++)
        if (std::tolower((unsigned char)s[at + k]) != prefix[k]) return false;
    return true;
}

// ------------------------------------------------------------ entities

// U+00A0..U+00FF by their HTML names.
const char* const kLatin1[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect",
    "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide",
    "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};

// U+0391..U+03A9 and U+03B1..U+03C9 (U+03A2 has no capital).
const char* const kGreek[25] = {
    "lpha", "eta", "amma", "elta", "psilon", "eta", "ta", "heta", "ota", "appa",
    "ambda", "u", "u", "i", "micron", "i", "ho", nullptr, "igma", "au", "psilon",
    "hi", "hi", "si", "mega",
};
const char kGreekFirst[25] = {'a', 'b', 'g', 'd', 'e', 'z', 'e', 't', 'i', 'k', 'l', 'm', 'n',
                              'x', 'o', 'p', 'r', 0, 's', 't', 'u', 'p', 'c', 'p', 'o'};

const struct { const char* name; uint32_t cp; } kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C},
    {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F}, {"hyphen", 0x2010},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E},
    {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026},
    {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A}, {"oline", 0x203E}, {"frasl", 0x2044}, {"euro", 0x20AC},
    {"trade", 0x2122}, {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192},
    {"darr", 0x2193}, {"harr", 0x2194}, {"lArr", 0x21D0}, {"uArr", 0x21D1},
    {"rArr", 0x21D2}, {"dArr", 0x21D3}, {"hArr", 0x21D4}, {"forall", 0x2200},
    {"part", 0x2202}, {"exist", 0x2203}, {"empty", 0x2205}, {"nabla", 0x2207},
    {"isin", 0x2208}, {"notin", 0x2209}, {"ni", 0x220B}, {"prod", 0x220F},
    {"sum", 0x2211}, {"minus", 0x2212}, {"lowast", 0x2217}, {"radic", 0x221A},
    {"prop", 0x221D}, {"infin", 0x221E}, {"ang", 0x2220}, {"and", 0x2227},
    {"or", 0x2228}, {"cap", 0x2229}, {"cup", 0x222A}, {"int", 0x222B},
    {"there4", 0x2234}, {"sim", 0x223C}, {"cong", 0x2245}, {"asymp", 0x2248},
    {"ne", 0x2260}, {"equiv", 0x2261}, {"le", 0x2264}, {"ge", 0x2265},
    {"sub", 0x2282}, {"sup", 0x2283}, {"nsub", 0x2284}, {"sube", 0x2286},
    {"supe", 0x2287}, {"oplus", 0x2295}, {"otimes", 0x2297}, {"perp", 0x22A5},
    {"sdot", 0x22C5}, {"lceil", 0x2308}, {"rceil", 0x2309}, {"lfloor", 0x230A},
    {"rfloor", 0x230B}, {"lang", 0x27E8}, {"rang", 0x27E9}, {"loz", 0x25CA},
    {"spades", 0x2660}, {"clubs", 0x2663}, {"hearts", 0x2665}, {"diams", 0x2666},
    {"check", 0x2713}, {"cross", 0x2717}, {"star", 0x2606}, {"starf", 0x2605},
    {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161},
    {"Yuml", 0x178}, {"fnof", 0x192}, {"circ", 0x2C6}, {"tilde", 0x2DC},
    {"thetasym", 0x3D1}, {"upsih", 0x3D2}, {"piv", 0x3D6}, {"sigmaf", 0x3C2},
};

// The code point an entity name stands for, or 0.
uint32_t namedEntity(const std::string& name) {
    for (int k = 0; k < 96; k++)
        if (name == kLatin1[k]) return 0xA0 + (uint32_t)k;
    for (const auto& e : kEntities)
        if (name == e.name) return e.cp;
    if (name.size() >= 2) {
        for (int k = 0; k < 25; k++) {
            if (!kGreek[k]) continue;
            const char first = kGreekFirst[k];
            if (name.substr(1) != kGreek[k]) continue;
            if (name[0] == first) return 0x3B1 + (uint32_t)k;
            if (name[0] == (char)std::toupper((unsigned char)first)) return 0x391 + (uint32_t)k;
        }
    }
    return 0;
}

// An entity or numeric character reference at `i` ('&'): its text and the
// index past it, or 0.
size_t readEntity(const std::string& s, size_t i, size_t to, std::string& text) {
    size_t k = i + 1;
    if (k < to && s[k] == '#') {
        k++;
        const bool hex = k < to && (s[k] == 'x' || s[k] == 'X');
        if (hex) k++;
        const size_t digits = k;
        uint32_t cp = 0;
        while (k < to && k - digits < (hex ? 6u : 7u) &&
               (hex ? std::isxdigit((unsigned char)s[k]) : std::isdigit((unsigned char)s[k]))) {
            const char c = (char)std::tolower((unsigned char)s[k]);
            cp = cp * (hex ? 16 : 10) + (uint32_t)(c >= 'a' ? c - 'a' + 10 : c - '0');
            k++;
        }
        if (k == digits || k >= to || s[k] != ';') return 0;
        text = encodeUtf8(cp);
        return k + 1;
    }
    const size_t name = k;
    while (k < to && k - name < 32 && std::isalnum((unsigned char)s[k])) k++;
    if (k == name || k >= to || s[k] != ';') return 0;
    const uint32_t cp = namedEntity(s.substr(name, k - name));
    if (!cp) return 0;
    text = encodeUtf8(cp);
    return k + 1;
}

// Backslash escapes and entities resolved, as in a link destination.
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && i + 1 < s.size() && isAsciiPunct(s[i + 1])) {
            out += s[i + 1];
            i += 2;
            continue;
        }
        std::string ent;
        if (s[i] == '&') {
            if (size_t next = readEntity(s, i, s.size(), ent)) {
                out += ent;
                i = next;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// A link label as it is looked up: trimmed, inner whitespace collapsed, case
// ignored.
std::string normalizeLabel(const std::string& label) {
    std::string out;
    bool space = false;
    for (char c : trim(label)) {
        if (std::isspace((unsigned char)c)) { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

using Refs = std::map<std::string, std::string>;

// ------------------------------------------------------------ inline HTML

struct Tag {
    std::string name;           // lowercased; empty for a comment or declaration
    bool closing = false;
    std::map<std::string, std::string> attrs;
};

// Reads an HTML tag, comment or declaration at `i` ('<'): the index past
// it, or 0.
size_t readHtml(const std::string& s, size_t i, size_t to, Tag* tag) {
    if (i + 1 >= to) return 0;
    if (s.compare(i, 4, "<!--") == 0) {
        const size_t e = s.find("-->", i + 4);
        return e == std::string::npos || e + 3 > to ? 0 : e + 3;
    }
    if (s[i + 1] == '?' || (s[i + 1] == '!' && i + 2 < to && std::isalpha((unsigned char)s[i + 2]))) {
        const size_t e = s.find('>', i + 2);
        return e == std::string::npos || e >= to ? 0 : e + 1;
    }
    size_t k = i + 1;
    const bool closing = s[k] == '/';
    if (closing) k++;
    if (k >= to || !std::isalpha((unsigned char)s[k])) return 0;
    const size_t name = k;
    while (k < to && (std::isalnum((unsigned char)s[k]) || s[k] == '-')) k++;
    Tag t;
    t.name = lowerAscii(s.substr(name, k - name));
    t.closing = closing;
    auto skipSpace = [&] {
        size_t before = k;
        while (k < to && std::isspace((unsigned char)s[k])) k++;
        return k > before;
    };
    if (closing) {
        skipSpace();
        if (k >= to || s[k] != '>') return 0;
        if (tag) *tag = t;
        return k + 1;
    }
    for (;;) {
        const bool spaced = skipSpace();
        if (k >= to) return 0;
        if (s[k] == '>') { k++; break; }
        if (s[k] == '/' && k + 1 < to && s[k + 1] == '>') { k += 2; break; }
        if (!spaced) return 0;
        const char c = s[k];
        if (!(std::isalpha((unsigned char)c) || c == '_' || c == ':')) return 0;
        const size_t an = k;
        while (k < to && (std::isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '.' ||
                          s[k] == ':' || s[k] == '-'))
            k++;
        std::string attr = lowerAscii(s.substr(an, k - an)), value;
        const size_t save = k;
        skipSpace();
        if (k < to && s[k] == '=') {
            k++;
            skipSpace();
            if (k >= to) return 0;
            if (s[k] == '"' || s[k] == '\'') {
                const char q = s[k];
                const size_t e = s.find(q, k + 1);
                if (e == std::string::npos || e >= to) return 0;
                value = s.substr(k + 1, e - k - 1);
                k = e + 1;
            } else {
                const size_t v = k;
                while (k < to && !std::isspace((unsigned char)s[k]) &&
                       std::strchr("\"'=<>`", s[k]) == nullptr)
                    k++;
                if (k == v) return 0;
                value = s.substr(v, k - v);
            }
        } else {
            k = save;
        }
        t.attrs[attr] = unescape(value);
    }
    if (tag) *tag = t;
    return k;
}

// An autolink at `i` ('<'): <scheme:...> or <user@host>. The index past it,
// or 0.
size_t readAutolink(const std::string& s, size_t i, size_t to, std::string* url,
                    std::string* text) {
    size_t k = i + 1;
    const size_t start = k;
    if (k < to && std::isalpha((unsigned char)s[k])) {
        while (k < to && k - start < 32 &&
               (std::isalnum((unsigned char)s[k]) || s[k] == '+' || s[k] == '.' || s[k] == '-'))
            k++;
        if (k - start >= 2 && k < to && s[k] == ':') {
            while (k < to && s[k] != '>' && s[k] != '<' && (unsigned char)s[k] > ' ') k++;
            if (k < to && s[k] == '>') {
                if (text) *text = s.substr(start, k - start);
                if (url) *url = s.substr(start, k - start);
                return k + 1;
            }
        }
    }
    // An email address.
    k = start;
    while (k < to && (std::isalnum((unsigned char)s[k]) ||
                      std::strchr(".!#$%&'*+/=?^_`{|}~-", s[k]) != nullptr))
        k++;
    if (k == start || k >= to || s[k] != '@') return 0;
    const size_t host = ++k;
    while (k < to && (std::isalnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '.')) k++;
    if (k == host || k >= to || s[k] != '>' || s[k - 1] == '.' || s[k - 1] == '-') return 0;
    if (text) *text = s.substr(start, k - start);
    if (url) *url = "mailto:" + s.substr(start, k - start);
    return k + 1;
}

// ------------------------------------------------------------ inline pass

struct Piece {
    enum Kind { Text, Delim, Code, Image, Soft, Hard, TagOpen, TagClose, Math };
    Kind kind = Text;
    std::string text;       // what shows (a delimiter's remaining characters)
    size_t at = 0;          // where it starts in the block's text
    int bold = 0, italic = 0, strike = 0;
    bool codeTag = false;   // inside <code> or <kbd>
    bool display = false;   // Math: display math, not inline
    bool link = false;
    std::string url;        // a link's target (for a tag, <a>'s href)
    std::string src;        // an image's source
    // A run of * _ or ~ still to be matched.
    char dc = 0;
    int count = 0, orig = 0;
    bool canOpen = false, canClose = false;
    int tag = 0;            // TagOpen/TagClose: 1 bold, 2 italic, 3 strike, 4 code, 5 link
};

// A character the inline pass looks at twice: everything else is copied
// through as it is ('h' and 'w' may start a web address).
bool special(unsigned char c) {
    static const struct Table {
        bool is[256] = {};
        Table() {
            for (const char* p = "\\\n`*_~[!<&hHwW$"; *p; p++) is[(unsigned char)*p] = true;
        }
    } table;
    return table.is[c];
}

// CommonMark's "process emphasis": match delimiter runs, innermost first,
// and mark what lies between. Unmatched runs become text.
void processEmphasis(std::vector<Piece>& p) {
    // No opener for this kind of closer (character, length mod 3, whether
    // it could open) lies above this index.
    int bottom[3][3][2];
    for (auto& a : bottom)
        for (auto& b : a) b[0] = b[1] = -1;
    for (size_t c = 0; c < p.size(); c++) {
        Piece& cl = p[c];
        if (cl.kind != Piece::Delim || !cl.canClose) continue;
        while (cl.count > 0) {
            int& lo = bottom[cl.dc == '*' ? 0 : cl.dc == '_' ? 1 : 2][cl.orig % 3]
                            [cl.canOpen ? 1 : 0];
            int found = -1;
            for (int o = (int)c - 1; o > lo; o--) {
                const Piece& op = p[(size_t)o];
                if (op.kind != Piece::Delim || op.dc != cl.dc || !op.canOpen || op.count == 0)
                    continue;
                if (cl.dc == '~') {
                    if (op.count != cl.count) continue;
                } else if ((op.canClose || cl.canOpen) && (op.orig + cl.orig) % 3 == 0 &&
                           !(op.orig % 3 == 0 && cl.orig % 3 == 0)) {
                    continue;
                }
                found = o;
                break;
            }
            if (found < 0) {
                lo = (int)c - 1;
                break;
            }
            Piece& op = p[(size_t)found];
            const int use = cl.dc == '~' ? cl.count : (op.count >= 2 && cl.count >= 2 ? 2 : 1);
            for (size_t k = (size_t)found + 1; k < c; k++) {
                if (cl.dc == '~') p[k].strike++;
                else if (use == 2) p[k].bold++;
                else p[k].italic++;
                if (p[k].kind == Piece::Delim) p[k].kind = Piece::Text;   // unmatched inside
            }
            op.count -= use;
            op.text.erase(op.text.size() - (size_t)use);
            cl.count -= use;
            cl.text.erase(0, (size_t)use);
        }
    }
    for (Piece& q : p)
        if (q.kind == Piece::Delim) q.kind = Piece::Text;
}

class Inline {
public:
    Inline(const std::string& s, const Refs& refs) : s_(s), refs_(refs) { matchBrackets(); }

    // Pieces of s[from, to). Inside a link's text no further links are made.
    void parse(size_t from, size_t to, bool inLink, std::vector<Piece>& out) {
        std::map<size_t, size_t> noCloser;   // backtick run length -> no closer from here on
        MathMemo memo;
        std::string pending;
        size_t pendingAt = from;
        auto flush = [&] {
            if (pending.empty()) return;
            Piece t;
            t.text = pending;
            t.at = pendingAt;
            out.push_back(std::move(t));
            pending.clear();
        };
        auto addText = [&](size_t at, const std::string& text) {
            if (pending.empty()) pendingAt = at;
            pending += text;
        };
        size_t i = from;
        while (i < to) {
            // Ordinary text, up to the next character that may mean something.
            size_t plain = i;
            while (plain < to && !special((unsigned char)s_[plain])) plain++;
            if (plain > i) {
                if (pending.empty()) pendingAt = i;
                pending.append(s_, i, plain - i);
                i = plain;
                continue;
            }
            const char c = s_[i];
            // Math first, so nothing inside it is read as Markdown.
            if (c == '$' || (c == '\\' && i + 1 < to && s_[i + 1] == '(')) {
                Piece m;
                if (size_t next = math(i, to, noCloser, memo, m)) {
                    flush();
                    out.push_back(std::move(m));
                    i = next;
                    continue;
                }
                if (c == '$') {
                    // A dollar sign, or two that close nothing.
                    const size_t n = i + 1 < to && s_[i + 1] == '$' ? 2 : 1;
                    addText(i, std::string(n, '$'));
                    i += n;
                    continue;
                }
            }
            if (c == '\\' && i + 1 < to && s_[i + 1] == '\n') {
                flush();
                Piece h;
                h.kind = Piece::Hard;
                h.at = i;
                out.push_back(std::move(h));
                i += 2;
                continue;
            }
            if (c == '\\' && i + 1 < to && isAsciiPunct(s_[i + 1])) {
                addText(i, std::string(1, s_[i + 1]));
                i += 2;
                continue;
            }
            if (c == '\n') {
                // Two or more spaces before the line's end make a hard break.
                size_t spaces = 0;
                while (spaces < pending.size() && pending[pending.size() - 1 - spaces] == ' ')
                    spaces++;
                pending.erase(pending.size() - spaces);
                flush();
                Piece b;
                b.kind = spaces >= 2 ? Piece::Hard : Piece::Soft;
                b.at = i;
                out.push_back(std::move(b));
                i++;
                continue;
            }
            if (c == '`') {
                size_t n = 0;
                while (i + n < to && s_[i + n] == '`') n++;
                const size_t close = findBackticks(i + n, to, n, noCloser);
                if (close == std::string::npos) {
                    addText(i, std::string(n, '`'));
                    i += n;
                    continue;
                }
                flush();
                std::string code = s_.substr(i + n, close - i - n);
                for (char& ch : code)
                    if (ch == '\n') ch = ' ';
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' &&
                    code.find_first_not_of(' ') != std::string::npos)
                    code = code.substr(1, code.size() - 2);
                Piece k;
                k.kind = Piece::Code;
                k.text = code;
                k.at = i;
                out.push_back(std::move(k));
                i = close + n;
                continue;
            }
            if (c == '*' || c == '_' || c == '~') {
                size_t n = 0;
                while (i + n < to && s_[i + n] == c) n++;
                if (c == '~' && n > 2) {
                    addText(i, std::string(n, '~'));
                    i += n;
                    continue;
                }
                flush();
                const uint32_t before = i == 0 ? ' ' : decodeBefore(s_, i);
                const uint32_t after = i + n >= s_.size() ? ' ' : decodeAt(s_, i + n);
                const bool left = !isSpaceCp(after) &&
                                  (!isPunctCp(after) || isSpaceCp(before) || isPunctCp(before));
                const bool right = !isSpaceCp(before) &&
                                   (!isPunctCp(before) || isSpaceCp(after) || isPunctCp(after));
                Piece d;
                d.kind = Piece::Delim;
                d.text = std::string(n, c);
                d.at = i;
                d.dc = c;
                d.count = d.orig = (int)n;
                if (c == '_') {
                    d.canOpen = left && (!right || isPunctCp(before));
                    d.canClose = right && (!left || isPunctCp(after));
                } else {
                    d.canOpen = left;
                    d.canClose = right;
                }
                out.push_back(std::move(d));
                i += n;
                continue;
            }
            if ((c == '[' || (c == '!' && i + 1 < to && s_[i + 1] == '[')) && !inLink) {
                const bool image = c == '!';
                std::vector<Piece> made;
                if (size_t next = link(image ? i + 1 : i, to, image, made)) {
                    flush();
                    out.insert(out.end(), std::make_move_iterator(made.begin()),
                               std::make_move_iterator(made.end()));
                    i = next;
                    continue;
                }
            } else if (c == '!' && inLink && i + 1 < to && s_[i + 1] == '[') {
                // An image inside a link's text (a badge) is still an image.
                std::vector<Piece> made;
                if (size_t next = link(i + 1, to, true, made)) {
                    flush();
                    out.insert(out.end(), std::make_move_iterator(made.begin()),
                               std::make_move_iterator(made.end()));
                    i = next;
                    continue;
                }
            }
            if (c == '<') {
                std::string url, text;
                if (size_t next = readAutolink(s_, i, to, &url, &text)) {
                    flush();
                    Piece l;
                    l.text = text;
                    l.at = i;
                    l.link = !inLink;
                    l.url = url;
                    out.push_back(std::move(l));
                    i = next;
                    continue;
                }
                Tag tag;
                if (size_t next = readHtml(s_, i, to, &tag)) {
                    flush();
                    html(tag, i, out);
                    i = next;
                    continue;
                }
            }
            if (c == '&') {
                std::string text;
                if (size_t next = readEntity(s_, i, to, text)) {
                    addText(i, text);
                    i = next;
                    continue;
                }
            }
            if (!inLink && (c == 'h' || c == 'H' || c == 'w' || c == 'W')) {
                if (size_t next = bareLink(i, to, from)) {
                    flush();
                    Piece l;
                    l.text = s_.substr(i, next - i);
                    l.at = i;
                    l.link = true;
                    l.url = (c == 'w' || c == 'W') ? "http://" + l.text : l.text;
                    out.push_back(std::move(l));
                    i = next;
                    continue;
                }
            }
            if (pending.empty()) pendingAt = i;
            pending += c;
            i++;
        }
        flush();
    }

private:
    const std::string& s_;
    const Refs& refs_;
    std::vector<std::pair<size_t, size_t>> close_;   // '[' and its ']', by '['
    int depth_ = 0;                    // links and images being read, nested

    // Where a search for a closing "$$" or "\)" already failed: a later
    // search from there on, in the same region, fails too. It keeps a line
    // of openers with no closers linear.
    struct MathMemo {
        size_t noDisplay = std::string::npos;
        size_t noParen = std::string::npos;
    };

    // Past a code span starting at `k` (a backtick), or past its backticks
    // when nothing closes them.
    size_t skipCode(size_t k, size_t to, std::map<size_t, size_t>& ticks) {
        size_t n = 0;
        while (k + n < to && s_[k + n] == '`') n++;
        const size_t close = findBackticks(k + n, to, n, ticks);
        return close == std::string::npos ? k + n : close + n;
    }

    // Math at `i`: "$...$", "$$...$$" or "\(...\)". Fills `m` (a Math piece
    // holding the TeX) and returns the index past the closing delimiter, or
    // 0 when there is none. A backslash and the character after it are
    // skipped together, so \$ never closes; so is a code span, so a $ in
    // one is not math. Inline $ follows pandoc: the first $ after the
    // opener closes it, and only when it has no space before it and no
    // digit after it, or there is no math at all.
    size_t math(size_t i, size_t to, std::map<size_t, size_t>& ticks, MathMemo& memo, Piece& m) {
        const size_t npos = std::string::npos;
        size_t open = 0, close = npos, next = 0;
        bool display = false;
        if (s_[i] == '\\') {
            open = i + 2;
            if (open >= memo.noParen) return 0;
            for (size_t k = open; k + 1 < to; k++) {
                if (s_[k] != '\\') continue;
                if (s_[k + 1] == ')') {
                    close = k;
                    next = k + 2;
                    break;
                }
                k++;
            }
            if (close == npos) {
                memo.noParen = std::min(memo.noParen, open);
                return 0;
            }
        } else if (i + 1 < to && s_[i + 1] == '$') {
            display = true;
            open = i + 2;
            if (open >= memo.noDisplay) return 0;
            for (size_t k = open; k < to;) {
                const char c = s_[k];
                if (c == '\\') { k += 2; continue; }
                if (c == '`') { k = skipCode(k, to, ticks); continue; }
                if (c == '$' && k + 1 < to && s_[k + 1] == '$') {
                    close = k;
                    next = k + 2;
                    break;
                }
                k++;
            }
            if (close == npos) {
                memo.noDisplay = std::min(memo.noDisplay, open);
                return 0;
            }
        } else {
            open = i + 1;
            if (open >= to || std::isspace((unsigned char)s_[open])) return 0;
            for (size_t k = open; k < to;) {
                const char c = s_[k];
                if (c == '\\') { k += 2; continue; }
                if (c == '`') { k = skipCode(k, to, ticks); continue; }
                if (c == '$') {
                    if (std::isspace((unsigned char)s_[k - 1]) ||
                        (k + 1 < s_.size() && std::isdigit((unsigned char)s_[k + 1])))
                        return 0;
                    close = k;
                    next = k + 1;
                    break;
                }
                k++;
            }
            if (close == npos) return 0;
        }
        std::string tex = s_.substr(open, close - open);
        if (tex.find_first_not_of(" \t\n") == npos) return 0;
        if (display || s_[i] == '\\') {
            tex.erase(0, tex.find_first_not_of(" \t\n"));
            tex.erase(tex.find_last_not_of(" \t\n") + 1);
        }
        // Display TeX keeps its lines (an aligned block is written over
        // several); an inline formula is one line of text.
        if (!display)
            for (char& ch : tex)
                if (ch == '\n') ch = ' ';
        m.kind = Piece::Math;
        m.text = std::move(tex);
        m.at = i;
        m.display = display;
        return next;
    }

    // Pairs every '[' with the ']' that closes it, in one pass, skipping
    // escapes, code spans, math, autolinks and tags as the inline pass does.
    void matchBrackets() {
        std::map<size_t, size_t> no;
        MathMemo memo;
        std::vector<size_t> open;
        const size_t to = s_.size();
        for (size_t k = 0; k < to;) {
            const char c = s_[k];
            if (c == '$' || (c == '\\' && k + 1 < to && s_[k + 1] == '(')) {
                Piece m;
                if (size_t e = math(k, to, no, memo, m)) { k = e; continue; }
                if (c == '$') {   // as the inline pass reads them: plain dollars
                    k += k + 1 < to && s_[k + 1] == '$' ? 2 : 1;
                    continue;
                }
            }
            if (c == '\\' && k + 1 < to) { k += 2; continue; }
            if (c == '`') {
                size_t n = 0;
                while (k + n < to && s_[k + n] == '`') n++;
                const size_t close = findBackticks(k + n, to, n, no);
                k = close == std::string::npos ? k + n : close + n;
                continue;
            }
            if (c == '<') {
                if (size_t e = readAutolink(s_, k, to, nullptr, nullptr)) { k = e; continue; }
                if (size_t e = readHtml(s_, k, to, nullptr)) { k = e; continue; }
            }
            if (c == '[') {
                open.push_back(k);
            } else if (c == ']' && !open.empty()) {
                close_.push_back({open.back(), k});
                open.pop_back();
            }
            k++;
        }
        std::sort(close_.begin(), close_.end());
    }

    // A run of exactly n backticks at or after `from`, or npos.
    size_t findBackticks(size_t from, size_t to, size_t n, std::map<size_t, size_t>& no) {
        const auto known = no.find(n);
        if (known != no.end() && from >= known->second) return std::string::npos;
        for (size_t k = from; k < to;) {
            if (s_[k] != '`') { k++; continue; }
            size_t m = 0;
            while (k + m < to && s_[k + m] == '`') m++;
            if (m == n) return k;
            k += m;
        }
        no[n] = known == no.end() ? from : std::min(from, known->second);
        return std::string::npos;
    }

    // The ']' closing the bracket at `open` before `to`, or npos.
    size_t closeBracket(size_t open, size_t to) {
        const auto m = std::lower_bound(close_.begin(), close_.end(),
                                        std::make_pair(open, (size_t)0));
        return m == close_.end() || m->first != open || m->second >= to ? std::string::npos
                                                                         : m->second;
    }

    // "(destination "title")" after a link's text, `p` just past the '('.
    bool inlineTarget(size_t p, size_t to, std::string& dest, size_t& end) {
        auto skip = [&] { while (p < to && std::isspace((unsigned char)s_[p])) p++; };
        skip();
        if (p < to && s_[p] == '<') {
            const size_t start = ++p;
            while (p < to && s_[p] != '>' && s_[p] != '\n' && s_[p] != '<') {
                if (s_[p] == '\\' && p + 1 < to) p++;
                p++;
            }
            if (p >= to || s_[p] != '>') return false;
            dest = s_.substr(start, p - start);
            p++;
        } else {
            const size_t start = p;
            int depth = 0;
            while (p < to) {
                const char c = s_[p];
                if (c == '\\' && p + 1 < to && isAsciiPunct(s_[p + 1])) { p += 2; continue; }
                if (c == '(') depth++;
                else if (c == ')') { if (depth == 0) break; depth--; }
                else if ((unsigned char)c <= ' ') break;
                p++;
            }
            if (depth != 0) return false;
            dest = s_.substr(start, p - start);
        }
        const size_t beforeTitle = p;
        skip();
        if (p < to && p > beforeTitle && (s_[p] == '"' || s_[p] == '\'' || s_[p] == '(')) {
            const char close = s_[p] == '(' ? ')' : s_[p];
            p++;
            while (p < to && s_[p] != close) {
                if (s_[p] == '\\' && p + 1 < to) p++;
                p++;
            }
            if (p >= to) return false;
            p++;
            skip();
        }
        if (p >= to || s_[p] != ')') return false;
        dest = unescape(dest);
        end = p + 1;
        return true;
    }

    // A link or image whose text opens at `open` ('['): its pieces and the
    // index past it, or 0 when it is not one.
    size_t link(size_t open, size_t to, bool image, std::vector<Piece>& made) {
        const size_t close = closeBracket(open, to);
        if (close == std::string::npos || depth_ >= 8) return 0;
        struct Nest {
            int& d;
            explicit Nest(int& depth) : d(depth) { d++; }
            ~Nest() { d--; }
        } nest(depth_);
        std::string dest;
        size_t end = 0;
        bool found = false;
        if (close + 1 < to && s_[close + 1] == '(')
            found = inlineTarget(close + 2, to, dest, end);
        if (!found) {
            // A reference: labels are at most 999 characters.
            if (refs_.empty() || close - open > 1000) return 0;
            std::string label = s_.substr(open + 1, close - open - 1);
            end = close + 1;
            if (close + 1 < to && s_[close + 1] == '[') {
                const size_t lc = s_.find(']', close + 2);
                if (lc != std::string::npos && lc < to) {
                    const std::string full = s_.substr(close + 2, lc - close - 2);
                    if (!trim(full).empty()) label = full;
                    end = lc + 1;
                }
            }
            const auto ref = refs_.find(normalizeLabel(label));
            if (ref == refs_.end()) return 0;
            dest = ref->second;
        }
        std::vector<Piece> inner;
        parse(open + 1, close, true, inner);
        processEmphasis(inner);
        if (image) {
            if (dest.empty()) return 0;
            Piece im;
            im.kind = Piece::Image;
            im.at = open - 1;
            im.src = dest;
            for (const Piece& q : inner)
                if (q.kind == Piece::Text || q.kind == Piece::Code || q.kind == Piece::Math)
                    im.text += q.text;
                else if (q.kind == Piece::Image) im.text += q.text;
                else if (q.kind == Piece::Soft || q.kind == Piece::Hard) im.text += ' ';
            made.push_back(std::move(im));
            return end;
        }
        for (Piece& q : inner) {
            if (q.kind == Piece::TagOpen || q.kind == Piece::TagClose) {
                if (q.tag == 5) continue;   // no link inside a link
            }
            q.link = true;
            q.url = dest;
            made.push_back(std::move(q));
        }
        return end;
    }

    // A web address written out ("https://...", "www..."), GitHub's
    // extended autolink: the index past it, or 0.
    size_t bareLink(size_t i, size_t to, size_t from) {
        if (i > from) {
            const char b = s_[i - 1];
            if (!std::isspace((unsigned char)b) && b != '*' && b != '_' && b != '~' && b != '(')
                return 0;
        }
        size_t host;
        if (startsWithNoCase(s_, i, "https://")) host = i + 8;
        else if (startsWithNoCase(s_, i, "http://")) host = i + 7;
        else if (startsWithNoCase(s_, i, "www.")) host = i + 4;
        else return 0;
        size_t k = host;
        while (k < to && !std::isspace((unsigned char)s_[k]) && s_[k] != '<') k++;
        for (;;) {
            if (k <= host) return 0;
            const char last = s_[k - 1];
            if (std::strchr("?!.,:*_~'\"", last) != nullptr) { k--; continue; }
            if (last == ')') {
                long opens = 0, closes = 0;
                for (size_t q = i; q < k; q++) {
                    if (s_[q] == '(') opens++;
                    else if (s_[q] == ')') closes++;
                }
                if (closes > opens) { k--; continue; }
            }
            if (last == ';') {
                size_t a = k - 1;
                while (a > host && std::isalnum((unsigned char)s_[a - 1])) a--;
                if (a > host && s_[a - 1] == '&' && a < k - 1) { k = a - 1; continue; }
            }
            break;
        }
        if (!std::isalnum((unsigned char)s_[host])) return 0;
        return k;
    }

    // What an HTML tag does in the text: <br> breaks the line, <img> is a
    // picture, a few tags style what they enclose, and the rest vanish.
    void html(const Tag& tag, size_t at, std::vector<Piece>& out) {
        if (tag.name.empty()) return;   // a comment or declaration
        if (tag.name == "br") {
            Piece h;
            h.kind = Piece::Hard;
            h.at = at;
            out.push_back(std::move(h));
            return;
        }
        if (tag.name == "img" && !tag.closing) {
            const auto src = tag.attrs.find("src");
            if (src == tag.attrs.end() || src->second.empty()) return;
            Piece im;
            im.kind = Piece::Image;
            im.at = at;
            im.src = src->second;
            const auto alt = tag.attrs.find("alt");
            if (alt != tag.attrs.end()) im.text = alt->second;
            out.push_back(std::move(im));
            return;
        }
        int style = 0;
        const std::string& n = tag.name;
        if (n == "b" || n == "strong") style = 1;
        else if (n == "i" || n == "em") style = 2;
        else if (n == "s" || n == "del" || n == "strike") style = 3;
        else if (n == "code" || n == "kbd" || n == "tt" || n == "samp") style = 4;
        else if (n == "a") style = 5;
        if (!style) return;
        Piece t;
        t.kind = tag.closing ? Piece::TagClose : Piece::TagOpen;
        t.tag = style;
        t.at = at;
        if (style == 5 && !tag.closing) {
            const auto href = tag.attrs.find("href");
            if (href == tag.attrs.end()) return;
            t.url = href->second;
        }
        out.push_back(std::move(t));
    }
};

// The runs for a block's text: `lines` holds each source line's content
// (indentation and markers already taken off) and its line number. Soft
// line breaks become a space run of their own, so every other run comes
// from one source line.
void inlineRuns(const std::vector<std::pair<std::string, int>>& lines, const MdRun& base,
                const Refs& refs, std::vector<MdRun>& out) {
    if (lines.empty()) return;
    std::string text;
    std::vector<size_t> starts;
    for (size_t k = 0; k < lines.size(); k++) {
        if (k) text += '\n';
        starts.push_back(text.size());
        std::string content = lines[k].first;
        size_t a = 0;
        while (a < content.size() && (content[a] == ' ' || content[a] == '\t')) a++;
        content.erase(0, a);
        if (k + 1 == lines.size()) {
            while (!content.empty() && std::isspace((unsigned char)content.back()))
                content.pop_back();
        }
        text += content;
    }
    auto lineOf = [&](size_t at) {
        const size_t k = (size_t)(std::upper_bound(starts.begin(), starts.end(), at) -
                                  starts.begin());
        return lines[k == 0 ? 0 : k - 1].second;
    };

    std::vector<Piece> pieces;
    Inline(text, refs).parse(0, text.size(), false, pieces);
    processEmphasis(pieces);

    // HTML tags style what lies between them.
    int tb = 0, ti = 0, ts = 0, tc = 0;
    std::vector<std::string> hrefs;
    for (Piece& p : pieces) {
        if (p.kind == Piece::TagOpen || p.kind == Piece::TagClose) {
            const int d = p.kind == Piece::TagOpen ? 1 : -1;
            switch (p.tag) {
            case 1: tb = std::max(0, tb + d); break;
            case 2: ti = std::max(0, ti + d); break;
            case 3: ts = std::max(0, ts + d); break;
            case 4: tc = std::max(0, tc + d); break;
            case 5:
                if (d > 0) hrefs.push_back(p.url);
                else if (!hrefs.empty()) hrefs.pop_back();
                break;
            }
            continue;
        }
        p.bold += tb;
        p.italic += ti;
        p.strike += ts;
        if (tc) p.codeTag = true;
        if (!hrefs.empty() && !p.link) {
            p.link = true;
            p.url = hrefs.back();
        }
    }

    // Line breaks at either end of the text show nothing.
    auto shows = [](const Piece& p) {
        return p.kind == Piece::Image || p.kind == Piece::Code || p.kind == Piece::Math ||
               (p.kind == Piece::Text && !p.text.empty());
    };
    size_t first = 0, last = pieces.size();
    while (first < last && !shows(pieces[first])) first++;
    while (last > first && !shows(pieces[last - 1])) last--;

    // Display math stands on a line of its own, except in a heading or a
    // table cell, which are one line.
    const bool ownLine = base.heading == 0 && !base.table;
    const size_t begin = out.size();
    bool lineAfterMath = false;   // display math ended the line: the next text starts one
    auto newline = [&](int line) {
        MdRun nl = base;
        nl.text = "\n";
        nl.line = line;
        out.push_back(std::move(nl));
    };

    bool mergeable = false, afterBreak = false;
    for (size_t k = first; k < last; k++) {
        const Piece& p = pieces[k];
        if (p.kind == Piece::TagOpen || p.kind == Piece::TagClose) continue;
        if (ownLine && p.kind == Piece::Math && p.display) {
            // The spaces and breaks before it end in a line break instead.
            while (out.size() > begin && !out.back().image && !out.back().math &&
                   out.back().text.find_first_not_of(" \n") == std::string::npos)
                out.pop_back();
            if (out.size() > begin) {
                MdRun& prev = out.back();
                if (!prev.code && !prev.image)
                    prev.text.erase(prev.text.find_last_not_of(' ') + 1);
                newline(lineOf(p.at));
            }
            lineAfterMath = true;
        } else if (lineAfterMath) {
            if (p.kind == Piece::Soft || p.kind == Piece::Hard) continue;
            if (!shows(p)) continue;
            newline(lineOf(p.at));
            lineAfterMath = false;
            afterBreak = true;
        }
        MdRun r = base;
        r.bold = base.bold || p.bold > 0;
        r.italic = base.italic || p.italic > 0;
        r.strike = base.strike || p.strike > 0;
        r.line = lineOf(p.at);
        if (p.link) {
            r.link = true;
            r.url = p.url;
        }
        bool canMerge = true;
        switch (p.kind) {
        case Piece::Code:
            r.code = true;
            r.text = p.text;
            break;
        case Piece::Math:
            r.math = p.display ? 2 : 1;
            r.code = true;   // verbatim, where math is not typeset
            r.text = p.text;
            canMerge = false;
            break;
        case Piece::Image:
            r.image = true;
            r.text = p.text;
            r.src = p.src;
            canMerge = false;
            break;
        case Piece::Soft:
            r.text = " ";
            canMerge = false;
            break;
        case Piece::Hard:
            r.text = "\n";
            r.hardBreak = true;
            canMerge = false;
            break;
        default:
            r.text = p.text;
            if (p.codeTag) r.code = true;
            // Spaces after a line break (left where a tag or comment was
            // dropped) would show as a wider gap; HTML collapses them.
            if (afterBreak) r.text.erase(0, r.text.find_first_not_of(' '));
            break;
        }
        if (r.text.empty() && !r.image) continue;
        afterBreak = p.kind == Piece::Soft || p.kind == Piece::Hard;
        if (canMerge && mergeable && !out.empty()) {
            MdRun& prev = out.back();
            if (prev.bold == r.bold && prev.italic == r.italic && prev.strike == r.strike &&
                prev.code == r.code && prev.link == r.link && prev.url == r.url &&
                prev.line == r.line) {
                prev.text += r.text;
                continue;
            }
        }
        out.push_back(std::move(r));
        mergeable = canMerge;
    }
}

// ------------------------------------------------------------ tables

// Column alignment taken from a table's separator row.
enum class Align { Left, Center, Right };

// Split "| a | b |" into {"a","b"}, trimming cells and outer pipes. An escaped
// pipe (\|) is a literal character inside a cell, not a column boundary.
std::vector<std::string> splitTableRow(const std::string& line) {
    std::string s = trim(line);
    if (!s.empty() && s.front() == '|') s.erase(s.begin());
    if (!s.empty() && s.back() == '|' &&
        !(s.size() >= 2 && s[s.size() - 2] == '\\'))
        s.pop_back();
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

// A GitHub table separator row, e.g. "| --- | :--: | --: |". It needs at least
// one pipe, and every cell must be dashes with optional colons at either end.
// On success the per-column alignments are written to `aligns`.
bool parseTableSeparator(const std::string& line, std::vector<Align>& aligns) {
    if (line.find('|') == std::string::npos) return false;
    aligns.clear();
    for (const std::string& cell : splitTableRow(line)) {
        if (cell.empty()) return false;
        bool left = cell.front() == ':';
        bool right = cell.size() > 1 && cell.back() == ':';
        size_t a = left ? 1 : 0, b = cell.size() - (right ? 1 : 0);
        if (a >= b) return false;
        for (size_t i = a; i < b; i++)
            if (cell[i] != '-') return false;
        aligns.push_back(left && right ? Align::Center
                         : right       ? Align::Right
                                       : Align::Left);
    }
    return true;
}

// How many monospace columns a UTF-8 string occupies. Counts code points, not
// bytes; combining marks and variation selectors take no space, while East
// Asian wide characters and most emoji take two.
size_t displayWidth(const std::string& s) {
    size_t width = 0, i = 0, n = s.size();
    while (i < n) {
        size_t len = 1;
        const uint32_t cp = decodeAt(s, i, &len);
        i += len;
        if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F))
            continue;                           // zero width
        bool wide =
            (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
            (cp >= 0x20000 && cp <= 0x3FFFD);
        width += wide ? 2 : 1;
    }
    return width;
}

// Render a GitHub table as aligned monospace rows. Cells keep their inline
// styling (bold, code, links); padding is computed from the visible text, so
// markup characters that don't display don't throw the columns off. Every
// run carries the table's first source line.
void emitTable(const std::vector<std::vector<std::string>>& rows,
               const std::vector<Align>& aligns, int id, int line, const Refs& refs,
               std::vector<MdRun>& out) {
    size_t cols = aligns.size();

    // Inline-parse every cell once, and measure what will actually show.
    std::vector<std::vector<std::vector<MdRun>>> cells(rows.size());
    std::vector<size_t> width(cols, 1);
    for (size_t ri = 0; ri < rows.size(); ri++) {
        cells[ri].resize(cols);
        for (size_t c = 0; c < cols; c++) {
            MdRun base; base.table = true; base.bold = (ri == 0);
            base.tableId = id;
            base.tableRow = (int)ri;
            base.tableCol = (int)c;
            base.tableCols = (int)cols;
            base.tableAlign = (int)aligns[c];
            if (c < rows[ri].size())
                inlineRuns({{rows[ri][c], line}}, base, refs, cells[ri][c]);
            size_t w = 0;
            for (MdRun& r : cells[ri][c]) {
                r.line = line;
                w += displayWidth(r.text);
            }
            width[c] = std::max(width[c], w);
        }
    }

    auto plain = [&](const std::string& text) {
        MdRun r; r.table = true; r.text = text;
        r.tableId = id; r.tableCols = (int)cols;
        r.line = line;
        out.push_back(std::move(r));
    };

    for (size_t ri = 0; ri < rows.size(); ri++) {
        for (size_t c = 0; c < cols; c++) {
            if (c > 0) plain(" \xE2\x94\x82 ");          // " │ "
            size_t w = 0;
            for (const MdRun& r : cells[ri][c]) w += displayWidth(r.text);
            size_t pad = width[c] - w, before = 0;
            if (aligns[c] == Align::Right) before = pad;
            else if (aligns[c] == Align::Center) before = pad / 2;
            if (before) plain(std::string(before, ' '));
            for (MdRun& r : cells[ri][c]) out.push_back(std::move(r));
            if (pad - before) plain(std::string(pad - before, ' '));
        }
        plain("\n");
        if (ri == 0) {                                  // rule under the header
            std::string sep;
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) sep += "\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80";  // ─┼─
                for (size_t k = 0; k < width[c]; k++) sep += "\xE2\x94\x80";
            }
            plain(sep + "\n");
        }
    }
}

// ------------------------------------------------------------ block pass

struct Block {
    enum Kind { Para, Heading, Item, Code, Rule, Table, Math };
    Kind kind = Para;
    bool math = false;           // a ```math fence: display math, not code
    std::string tex;             // a Math block's TeX
    int first = 0, last = 0;     // source lines
    int edit = -1;               // MdLine::block of its lines
    int level = 0;               // a heading's
    int depth = 0;               // list nesting of an item, or of a paragraph in one
    bool listish = false;        // an item, or a paragraph inside an item
    std::string marker;          // an item's, as shown
    bool ordered = false;
    bool quote = false;
    int quoteGroup = -1;         // which block quote
    int quoteDepth = 0;          // how many > deep
    bool title = false;          // an alert's title: bold
    bool blankBefore = false;    // a blank line separates it from the block before
    std::vector<std::pair<std::string, int>> text;   // content lines
    std::vector<std::pair<std::string, int>> code;   // a code block's lines
    std::vector<std::vector<std::string>> rows;      // a table's
    std::vector<Align> aligns;
};

struct Doc {
    std::vector<MdLine> lines;
    std::vector<Block> blocks;
    Refs refs;
};

bool isRule(const std::string& s) {
    int count = 0; char first = 0;
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (!first) first = c;
        if (c != first) return false;
        count++;
    }
    return count >= 3;
}

// "# Title ##": the level, and the text's byte range within `body`.
bool parseAtx(const std::string& body, int& level, size_t& a, size_t& b) {
    size_t n = 0;
    while (n < body.size() && body[n] == '#') n++;
    if (n == 0 || n > 6) return false;
    if (n < body.size() && body[n] != ' ' && body[n] != '\t') return false;
    level = (int)n;
    a = n;
    while (a < body.size() && (body[a] == ' ' || body[a] == '\t')) a++;
    b = body.size();
    while (b > a && (body[b - 1] == ' ' || body[b - 1] == '\t')) b--;
    // A closing sequence of #s, after a space, is not part of the text.
    size_t h = b;
    while (h > a && body[h - 1] == '#') h--;
    if (h < b && (h == a || body[h - 1] == ' ' || body[h - 1] == '\t')) {
        b = h;
        while (b > a && (body[b - 1] == ' ' || body[b - 1] == '\t')) b--;
    }
    return true;
}

// "<h2 align="center">Title</h2>" on a line of its own.
bool parseHtmlHeading(const std::string& body, int& level, size_t& a, size_t& b) {
    if (body.size() < 9 || !startsWithNoCase(body, 0, "<h") || body[2] < '1' || body[2] > '6')
        return false;
    if (body[3] != '>' && body[3] != ' ') return false;
    const size_t gt = body.find('>', 3);
    if (gt == std::string::npos) return false;
    size_t e = body.size();
    while (e > 0 && std::isspace((unsigned char)body[e - 1])) e--;
    const std::string close = std::string("</h") + body[2] + ">";
    if (e < gt + 1 + close.size() || !startsWithNoCase(body, e - close.size(), close.c_str()))
        return false;
    level = body[2] - '0';
    a = gt + 1;
    b = e - close.size();
    return true;
}

// A setext underline: all '=' (level 1) or all '-' (level 2), trailing
// spaces allowed.
bool isSetextUnderline(const std::string& body, int& level) {
    const std::string t = trim(body);
    if (t.empty() || (t[0] != '=' && t[0] != '-')) return false;
    for (char c : t)
        if (c != t[0]) return false;
    level = t[0] == '=' ? 1 : 2;
    return true;
}

// ``` or ~~~ (three or more) opening a code block. `info` gets the first
// word after the fence, lowercased ("math" for GitHub's display math).
bool isFenceOpen(const std::string& body, char& ch, size_t& len, std::string* info = nullptr) {
    if (body.empty() || (body[0] != '`' && body[0] != '~')) return false;
    size_t n = 0;
    while (n < body.size() && body[n] == body[0]) n++;
    if (n < 3) return false;
    if (body[0] == '`' && body.find('`', n) != std::string::npos) return false;
    ch = body[0];
    len = n;
    if (info) {
        const std::string rest = trim(body.substr(n));
        *info = lowerAscii(rest.substr(0, rest.find_first_of(" \t{")));
    }
    return true;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string trimRight(const std::string& s) {
    size_t b = s.size();
    while (b > 0 && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(0, b);
}

// Display math on lines of its own: a line starting with "$$" (or "\["),
// then the line ending with "$$" (or "\]"), perhaps the same one, with no
// blank line between them (TeX would stop at one, and pandoc does). `last`
// gets the closing line and `tex` the TeX between the delimiters, trimmed.
// "$$ a $$ and $$ b $$" is a paragraph, not a block.
bool mathBlock(const std::vector<std::string>& raw, size_t i, const std::string& body,
               size_t& last, std::string& tex) {
    std::string close;
    if (body.compare(0, 2, "$$") == 0) close = "$$";
    else if (body.compare(0, 2, "\\[") == 0) close = "\\]";
    else return false;
    const std::string first = trimRight(body.substr(2));
    std::string content;
    if (endsWith(first, close)) {
        content = first.substr(0, first.size() - 2);
        if (content.find(close) != std::string::npos || content.find("$$") != std::string::npos)
            return false;
        last = i;
    } else {
        if (first.find(close) != std::string::npos) return false;
        content = first;
        size_t j = i + 1;
        for (; j < raw.size() && j < i + 2000; j++) {
            const std::string t = trimRight(raw[j]);
            if (trim(t).empty()) return false;
            if (endsWith(t, close)) {
                const std::string before = t.substr(0, t.size() - 2);
                if (before.find(close) != std::string::npos) return false;
                content += "\n" + before;
                break;
            }
            if (t.find(close) != std::string::npos) return false;
            content += "\n" + t;
        }
        if (j >= raw.size() || j >= i + 2000) return false;
        last = j;
    }
    tex = trim(content);
    return !tex.empty();
}

bool isFenceClose(const std::string& body, char ch, size_t len) {
    size_t n = 0;
    while (n < body.size() && body[n] == ch) n++;
    return n >= len && trim(body.substr(n)).empty();
}

struct ListMarker {
    bool ordered = false;
    long number = 0;
    std::string written;     // "-", "3." as in the source
    size_t textStart = 0;    // bytes from the marker to its text
    bool empty = false;      // nothing after the marker
    bool task = false, checked = false;
};

bool parseListMarker(const std::string& body, ListMarker& m) {
    if (body.empty()) return false;
    size_t k = 0;
    if (body[0] == '-' || body[0] == '*' || body[0] == '+') {
        k = 1;
    } else {
        while (k < body.size() && k < 9 && std::isdigit((unsigned char)body[k])) k++;
        if (k == 0 || k >= body.size() || (body[k] != '.' && body[k] != ')')) return false;
        m.ordered = true;
        m.number = std::stol(body.substr(0, k));
        k++;
    }
    if (k < body.size() && body[k] != ' ' && body[k] != '\t') return false;
    m.written = body.substr(0, k);
    size_t spaces = 0;
    while (k + spaces < body.size() && (body[k + spaces] == ' ' || body[k + spaces] == '\t'))
        spaces++;
    m.empty = k + spaces >= body.size();
    m.textStart = m.empty ? k + (spaces ? 1 : 0) : (spaces >= 5 ? k + 1 : k + spaces);
    const std::string rest = body.substr(std::min(m.textStart, body.size()));
    if (rest.size() >= 3 && rest[0] == '[' && rest[2] == ']' &&
        (rest[1] == ' ' || rest[1] == 'x' || rest[1] == 'X') &&
        (rest.size() == 3 || rest[3] == ' ' || rest[3] == '\t')) {
        m.task = true;
        m.checked = rest[1] != ' ';
    }
    return true;
}

// "[label]: destination" on a line of its own.
bool parseLinkDef(const std::string& body, std::string& label, std::string& dest) {
    if (body.empty() || body[0] != '[') return false;
    size_t k = 1;
    while (k < body.size() && body[k] != ']') {
        if (body[k] == '[') return false;
        if (body[k] == '\\') k++;
        k++;
    }
    if (k >= body.size() || k == 1 || k + 1 >= body.size() || body[k + 1] != ':') return false;
    label = body.substr(1, k - 1);
    if (trim(label).empty()) return false;
    size_t p = k + 2;
    while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
    if (p >= body.size()) return false;
    size_t e;
    if (body[p] == '<') {
        e = body.find('>', p);
        if (e == std::string::npos) return false;
        dest = body.substr(p + 1, e - p - 1);
        e++;
    } else {
        e = p;
        while (e < body.size() && !std::isspace((unsigned char)body[e])) e++;
        dest = body.substr(p, e - p);
    }
    const std::string rest = trim(body.substr(e));
    if (!rest.empty() && rest[0] != '"' && rest[0] != '\'' && rest[0] != '(') return false;
    dest = unescape(dest);
    return true;
}

// "> [!NOTE]": the alert's title, or "".
std::string alertTitle(const std::string& content) {
    const std::string t = lowerAscii(trim(content));
    if (t == "[!note]") return "Note";
    if (t == "[!tip]") return "Tip";
    if (t == "[!important]") return "Important";
    if (t == "[!warning]") return "Warning";
    if (t == "[!caution]") return "Caution";
    return "";
}

const char* bulletFor(int depth) {
    return depth <= 1 ? "\xE2\x80\xA2"          // •
         : depth == 2 ? "\xE2\x97\xA6"          // ◦
                      : "\xE2\x96\xAA";         // ▪
}

std::string markerText(const ListMarker& m, int depth) {
    if (m.task) return m.checked ? "\xE2\x98\x91" : "\xE2\x98\x90";   // ☑ ☐
    return m.ordered ? m.written : bulletFor(depth);
}

Doc analyze(const std::string& md) {
    Doc d;
    // Physical lines, with their offsets.
    std::vector<std::string> raw;
    for (size_t a = 0; a <= md.size();) {
        size_t e = md.find('\n', a);
        if (e == std::string::npos) e = md.size();
        size_t end = e;
        if (end > a && md[end - 1] == '\r') end--;
        MdLine L;
        L.lineStart = a;
        L.lineEnd = end;
        d.lines.push_back(L);
        raw.push_back(md.substr(a, end - a));
        a = e + 1;
        if (e == md.size()) break;
    }
    const size_t n = raw.size();

    int editIds = 0, quoteGroups = 0;
    int open = -1;                 // the block lazy lines continue
    bool blank = false;            // a blank line since the last block line
    struct Level { size_t indent, content; };
    std::vector<Level> list;
    bool inFence = false, inComment = false;
    char fenceChar = 0;
    size_t fenceLen = 0, fenceIndent = 0;
    int fenceBlock = -1;
    int codeBlock = -1;            // an indented code block being read
    std::vector<size_t> codeBlanks;
    int quoteGroup = -1, quoteEdit = -1;
    bool quoteFirst = false;

    auto newBlock = [&](Block::Kind kind, int line) -> Block& {
        Block b;
        b.kind = kind;
        b.first = b.last = line;
        b.blankBefore = blank;
        d.blocks.push_back(std::move(b));
        blank = false;
        return d.blocks.back();
    };
    auto settleList = [&](size_t indent) {
        while (!list.empty() && indent < list.back().content) list.pop_back();
    };

    size_t start = 0;
    // Front matter: "---", YAML keys, "---" (or "..."), shown as code.
    if (n > 2 && trim(raw[0]) == "---") {
        const std::string& k = raw[1];
        size_t c = 0;
        while (c < k.size() && (std::isalnum((unsigned char)k[c]) || k[c] == '_' || k[c] == '-'))
            c++;
        if (c > 0 && c < k.size() && k[c] == ':') {
            for (size_t j = 1; j < n && j < 200; j++) {
                const std::string t = trim(raw[j]);
                if (t != "---" && t != "...") continue;
                Block& b = newBlock(Block::Code, 0);
                b.last = (int)j;
                const int id = editIds++;
                for (size_t q = 1; q < j; q++) {
                    b.code.push_back({raw[q], (int)q});
                    d.lines[q].kind = MdLine::Code;
                    d.lines[q].block = id;
                    d.lines[q].start = d.lines[q].lineStart;
                    d.lines[q].end = d.lines[q].lineEnd;
                }
                d.lines[0].kind = d.lines[j].kind = MdLine::Fence;
                start = j + 1;
                break;
            }
        }
    }

    for (size_t i = start; i < n; i++) {
        const std::string& t = raw[i];
        MdLine& L = d.lines[i];
        size_t cols = 0, pos = 0;
        while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) {
            cols = t[pos] == '\t' ? cols + 4 - cols % 4 : cols + 1;
            pos++;
        }
        const std::string body = t.substr(pos);
        L.start = L.lineStart + pos;
        L.end = L.lineEnd;

        if (inFence) {
            if (isFenceClose(body, fenceChar, fenceLen)) {
                L.kind = MdLine::Fence;
                inFence = false;
                continue;
            }
            if (i + 1 == n && t.empty()) break;   // after the file's last newline
            // Content loses as much indentation as the opening fence had.
            size_t strip = 0, c = 0;
            while (strip < t.size() && c < fenceIndent && t[strip] == ' ') { strip++; c++; }
            Block& b = d.blocks[(size_t)fenceBlock];
            b.code.push_back({t.substr(strip), (int)i});
            b.last = (int)i;
            L.kind = MdLine::Code;
            L.block = b.edit;
            L.start = L.lineStart;
            continue;
        }
        if (inComment) {
            L.kind = MdLine::Hidden;
            if (t.find("-->") != std::string::npos) inComment = false;
            continue;
        }
        if (body.empty()) {
            L.kind = MdLine::Blank;
            if (codeBlock >= 0) codeBlanks.push_back(i);
            open = -1;
            blank = true;
            quoteGroup = -1;
            continue;
        }
        if (codeBlock >= 0) {
            if (cols >= 4) {
                Block& b = d.blocks[(size_t)codeBlock];
                for (size_t q : codeBlanks) {
                    b.code.push_back({"", (int)q});
                    d.lines[q].kind = MdLine::Code;
                    d.lines[q].block = b.edit;
                    d.lines[q].start = d.lines[q].lineStart;
                }
                codeBlanks.clear();
                size_t strip = 0, c = 0;
                while (strip < t.size() && c < 4) {
                    c = t[strip] == '\t' ? c + 4 - c % 4 : c + 1;
                    strip++;
                }
                b.code.push_back({t.substr(strip), (int)i});
                b.last = (int)i;
                L.kind = MdLine::Code;
                L.block = b.edit;
                L.start = L.lineStart;
                continue;
            }
            codeBlock = -1;
            codeBlanks.clear();
        }

        // Indented code: four spaces, with no paragraph to continue and no
        // list item the line could belong to.
        if (cols >= 4 && open < 0 && list.empty()) {
            quoteGroup = -1;
            Block& b = newBlock(Block::Code, (int)i);
            b.edit = editIds++;
            size_t strip = 0, c = 0;
            while (strip < t.size() && c < 4) {
                c = t[strip] == '\t' ? c + 4 - c % 4 : c + 1;
                strip++;
            }
            b.code.push_back({t.substr(strip), (int)i});
            codeBlock = (int)d.blocks.size() - 1;
            L.kind = MdLine::Code;
            L.block = b.edit;
            L.start = L.lineStart;
            continue;
        }

        // Up to three spaces of indentation, counted from the list item the
        // line belongs to, leave it able to start a block.
        size_t owner = 0;
        for (const Level& lv : list)
            if (cols >= lv.content) owner = lv.content;
        const bool shallow = cols - owner < 4;
        const bool plainPara = open >= 0 && d.blocks[(size_t)open].kind == Block::Para &&
                               !d.blocks[(size_t)open].quote &&
                               !d.blocks[(size_t)open].listish;
        int level = 0;
        size_t a = 0, b = 0;

        // A setext underline turns the paragraph above into a heading.
        if (shallow && plainPara && isSetextUnderline(body, level)) {
            Block& h = d.blocks[(size_t)open];
            h.kind = Block::Heading;
            h.level = level;
            for (int k = h.first; k <= h.last; k++) d.lines[(size_t)k].kind = MdLine::Heading;
            h.last = (int)i;
            L.kind = MdLine::SetextUnderline;
            open = -1;
            continue;
        }
        std::string info;
        if (shallow && isFenceOpen(body, fenceChar, fenceLen, &info)) {
            open = -1;
            quoteGroup = -1;
            settleList(cols);
            Block& f = newBlock(Block::Code, (int)i);
            f.edit = editIds++;
            f.math = info == "math";
            fenceBlock = (int)d.blocks.size() - 1;
            fenceIndent = cols;
            inFence = true;
            L.kind = MdLine::Fence;
            continue;
        }
        // Display math on lines of its own: one block, delimiters and all.
        size_t mathLast = 0;
        std::string tex;
        if (shallow && (body[0] == '$' || body[0] == '\\') &&
            mathBlock(raw, i, body, mathLast, tex)) {
            open = -1;
            quoteGroup = -1;
            settleList(cols);
            Block& mb = newBlock(Block::Math, (int)i);
            mb.edit = editIds++;
            mb.last = (int)mathLast;
            mb.depth = (int)list.size();
            mb.tex = std::move(tex);
            for (size_t j = i; j <= mathLast; j++) {
                MdLine& M = d.lines[j];
                M.kind = MdLine::Math;
                M.block = mb.edit;
                size_t q = 0;
                while (q < raw[j].size() && (raw[j][q] == ' ' || raw[j][q] == '\t')) q++;
                M.start = M.lineStart + q;
                M.end = M.lineEnd;
            }
            i = mathLast;
            continue;
        }
        // An HTML comment starting a line hides it, and ends any paragraph.
        if (shallow && body.compare(0, 4, "<!--") == 0) {
            const size_t close = body.find("-->", 4);
            if (close == std::string::npos) {
                inComment = true;
                L.kind = MdLine::Hidden;
                open = -1;
                continue;
            }
            if (trim(body.substr(close + 3)).empty()) {
                L.kind = MdLine::Hidden;
                open = -1;
                continue;
            }
        }
        if (shallow && (parseAtx(body, level, a, b) || parseHtmlHeading(body, level, a, b))) {
            open = -1;
            quoteGroup = -1;
            settleList(cols);
            Block& h = newBlock(Block::Heading, (int)i);
            h.level = level;
            h.edit = editIds++;
            h.text.push_back({body.substr(a, b - a), (int)i});
            L.kind = MdLine::Heading;
            L.block = h.edit;
            L.start = L.lineStart + pos + a;
            L.end = L.lineStart + pos + b;
            continue;
        }
        if (shallow && isRule(body)) {
            open = -1;
            quoteGroup = -1;
            settleList(cols);
            newBlock(Block::Rule, (int)i);
            L.kind = MdLine::Rule;
            continue;
        }
        // A GitHub table: a line with pipes, then a separator row with the
        // same number of columns.
        std::vector<Align> aligns;
        if (shallow && body.find('|') != std::string::npos && i + 1 < n &&
            parseTableSeparator(trim(raw[i + 1]), aligns) &&
            aligns.size() == splitTableRow(body).size()) {
            open = -1;
            quoteGroup = -1;
            settleList(cols);
            Block& tb = newBlock(Block::Table, (int)i);
            tb.edit = editIds++;
            tb.aligns = aligns;
            tb.rows.push_back(splitTableRow(body));
            L.kind = MdLine::TableHead;
            L.block = tb.edit;
            d.lines[i + 1].kind = MdLine::TableSep;
            size_t j = i + 2;
            for (; j < n; j++) {
                const std::string row = trim(raw[j]);
                if (row.empty() || row.find('|') == std::string::npos) break;
                tb.rows.push_back(splitTableRow(row));
                MdLine& R = d.lines[j];
                R.kind = MdLine::TableRow;
                R.block = tb.edit;
                size_t q = 0;
                while (q < raw[j].size() && (raw[j][q] == ' ' || raw[j][q] == '\t')) q++;
                R.start = R.lineStart + q;
                R.end = R.lineEnd;
            }
            tb.last = (int)j - 1;
            i = j - 1;
            continue;
        }
        if (shallow && body[0] == '>') {
            if (quoteGroup < 0) {
                open = -1;
                settleList(cols);
                quoteGroup = ++quoteGroups;
                quoteEdit = editIds++;
                quoteFirst = true;
            }
            size_t p = 0;
            int depth = 0;
            while (p < body.size() && body[p] == '>') {
                p++;
                depth++;
                if (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
                size_t q = p;
                while (q < body.size() && body[q] == ' ') q++;
                if (q < body.size() && body[q] == '>') p = q;
            }
            const std::string content = body.substr(p);
            size_t inner = 0;
            while (inner < content.size() && (content[inner] == ' ' || content[inner] == '\t'))
                inner++;
            const std::string ct = content.substr(inner);
            L.kind = MdLine::Quote;
            L.block = quoteEdit;
            blank = false;
            const bool firstLine = quoteFirst;
            quoteFirst = false;
            if (ct.empty()) {                   // a paragraph break inside the quote
                open = -1;
                continue;
            }
            auto quoted = [&](Block::Kind kind) -> Block& {
                Block& qb = newBlock(kind, (int)i);
                qb.quote = true;
                qb.quoteGroup = quoteGroup;
                qb.quoteDepth = depth;
                qb.edit = quoteEdit;
                return qb;
            };
            const std::string title = firstLine ? alertTitle(ct) : "";
            ListMarker m;
            if (!title.empty()) {
                Block& qb = quoted(Block::Para);
                qb.title = true;
                qb.text.push_back({title, (int)i});
                open = -1;
            } else if (parseAtx(ct, level, a, b)) {
                Block& qb = quoted(Block::Heading);
                qb.level = level;
                qb.text.push_back({ct.substr(a, b - a), (int)i});
                open = -1;
            } else if (parseListMarker(ct, m) &&
                       !(open >= 0 && d.blocks[(size_t)open].kind == Block::Para &&
                         !d.blocks[(size_t)open].listish &&
                         (m.empty || (m.ordered && m.number != 1)))) {
                Block& qb = quoted(Block::Item);
                qb.listish = true;
                qb.depth = 1 + (int)(inner / 2);
                qb.ordered = m.ordered;
                qb.marker = markerText(m, qb.depth);
                std::string text = ct.substr(std::min(m.textStart, ct.size()));
                if (m.task) text = text.size() > 3 ? text.substr(4) : "";
                if (!text.empty()) qb.text.push_back({text, (int)i});
                open = (int)d.blocks.size() - 1;
            } else if (open >= 0 && d.blocks[(size_t)open].quote &&
                       d.blocks[(size_t)open].quoteGroup == quoteGroup &&
                       d.blocks[(size_t)open].quoteDepth == depth) {
                Block& ob = d.blocks[(size_t)open];
                ob.text.push_back({ct, (int)i});
                ob.last = (int)i;
            } else {
                Block& qb = quoted(Block::Para);
                qb.text.push_back({ct, (int)i});
                open = (int)d.blocks.size() - 1;
            }
            continue;
        }

        ListMarker m;
        if (shallow && parseListMarker(body, m)) {
            // Only a non-empty bullet or a list starting at 1 may interrupt
            // a paragraph; inside a list any item starts the next one.
            const bool interrupts = !plainPara || (!m.empty && (!m.ordered || m.number == 1));
            if (interrupts) {
                open = -1;
                quoteGroup = -1;
                settleList(cols);
                list.push_back({cols, cols + (m.empty ? m.written.size() + 1 : m.textStart)});
                Block& it = newBlock(Block::Item, (int)i);
                it.edit = editIds++;
                it.listish = true;
                it.depth = (int)list.size();
                it.ordered = m.ordered;
                it.marker = markerText(m, it.depth);
                std::string text = body.substr(std::min(m.textStart, body.size()));
                if (m.task) text = text.size() > 3 ? text.substr(4) : "";
                if (!text.empty()) it.text.push_back({text, (int)i});
                L.kind = MdLine::ListItem;
                L.block = it.edit;
                L.start = L.lineStart + pos + std::min(m.textStart, body.size());
                open = (int)d.blocks.size() - 1;
                continue;
            }
        }
        std::string label, dest;
        if (shallow && open < 0 && parseLinkDef(body, label, dest)) {
            d.refs.emplace(normalizeLabel(label), dest);   // the first definition wins
            L.kind = MdLine::Hidden;
            continue;
        }

        // A lazy continuation of the paragraph, item or quote above.
        if (open >= 0) {
            Block& ob = d.blocks[(size_t)open];
            ob.text.push_back({body, (int)i});
            ob.last = (int)i;
            if (!ob.quote) quoteGroup = -1;
            L.kind = MdLine::Text;
            L.block = ob.edit;
            blank = false;
            continue;
        }
        quoteGroup = -1;
        settleList(cols);
        Block& p = newBlock(Block::Para, (int)i);
        p.edit = editIds++;
        if (!list.empty()) {
            p.listish = true;
            p.depth = (int)list.size();
        }
        p.text.push_back({body, (int)i});
        L.kind = MdLine::Text;
        L.block = p.edit;
        open = (int)d.blocks.size() - 1;
    }
    return d;
}

// Appends the runs of one block, without the space around it.
void emitBlock(const Block& b, const Refs& refs, int& tables, std::vector<MdRun>& out) {
    const size_t start = out.size();
    MdRun base;
    base.quote = b.quote;
    base.listDepth = b.depth;
    base.ordered = b.ordered;
    MdRun end = base;
    end.text = "\n";
    end.line = b.last;
    switch (b.kind) {
    case Block::Para:
        if (b.title) base.bold = true;
        inlineRuns(b.text, base, refs, out);
        if (out.size() > start) out.push_back(std::move(end));
        break;
    case Block::Heading: {
        base.heading = b.level;
        inlineRuns(b.text, base, refs, out);
        for (size_t k = start; k < out.size(); k++)
            out[k].line = b.first;   // one heading, one line to go to
        MdRun e;
        e.quote = b.quote;
        e.text = "\n";
        e.line = b.first;
        if (out.size() > start) out.push_back(std::move(e));
        break;
    }
    case Block::Item: {
        MdRun m = base;
        m.marker = true;
        m.text = std::string((size_t)b.depth * 2, ' ') + b.marker + " ";
        m.line = b.first;
        out.push_back(std::move(m));
        inlineRuns(b.text, base, refs, out);
        out.push_back(std::move(end));
        break;
    }
    case Block::Math: {
        MdRun r = base;
        r.math = 2;
        r.code = true;
        r.text = b.tex;
        r.line = b.first;
        out.push_back(std::move(r));
        out.push_back(std::move(end));
        break;
    }
    case Block::Code:
        if (b.math) {
            // A ```math fence: its lines are one display formula, stamped
            // with the first of them, which a double-click edits.
            std::string tex;
            for (const auto& line : b.code) tex += (tex.empty() ? "" : "\n") + line.first;
            tex = trim(tex);
            if (!tex.empty()) {
                MdRun r = base;
                r.math = 2;
                r.code = true;
                r.text = tex;
                r.line = b.code.front().second;
                out.push_back(std::move(r));
                MdRun e = base;
                e.text = "\n";
                e.line = b.code.back().second;
                out.push_back(std::move(e));
                break;
            }
        }
        for (const auto& line : b.code) {
            MdRun r;
            r.code = r.codeBlock = true;
            r.quote = b.quote;
            r.text = line.first + "\n";
            r.line = line.second;
            out.push_back(std::move(r));
        }
        break;
    case Block::Rule: {
        MdRun r;
        r.rule = true;
        r.text = "\n";
        r.line = b.first;
        out.push_back(std::move(r));
        break;
    }
    case Block::Table:
        emitTable(b.rows, b.aligns, ++tables, b.first, refs, out);
        break;
    }
}

// Whether runs[from...] show anything.
bool visible(const std::vector<MdRun>& runs, size_t from) {
    for (size_t k = from; k < runs.size(); k++) {
        const MdRun& r = runs[k];
        if (r.image || r.rule || r.codeBlock || r.table ||
            r.text.find_first_not_of(" \t\n") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

std::vector<MdRun> MarkdownParser::parse(const std::string& markdown) {
    const Doc d = analyze(markdown);
    std::vector<MdRun> out;
    int tables = 0;
    const Block* prev = nullptr;
    for (const Block& b : d.blocks) {
        const size_t mark = out.size();
        // The items of a list sit together unless a blank line parts them.
        if (prev && !(prev->listish && b.listish && !b.blankBefore)) {
            MdRun g;
            g.text = "\n";
            g.gap = true;
            g.line = std::min(prev->last + 1, b.first);
            g.quote = prev->quote && b.quote && prev->quoteGroup == b.quoteGroup;
            out.push_back(std::move(g));
        }
        const size_t begin = out.size();
        int t = tables;
        emitBlock(b, d.refs, t, out);
        if (!visible(out, begin)) {   // a block that shows nothing takes no space
            out.resize(mark);
            continue;
        }
        tables = t;
        prev = &b;
    }
    return out;
}

std::vector<MdLine> MarkdownParser::lines(const std::string& markdown) {
    return analyze(markdown).lines;
}

std::string MarkdownParser::anchor(const std::string& headingText) {
    const std::string t = trim(headingText);
    std::string out;
    for (size_t i = 0; i < t.size();) {
        size_t len = 1;
        uint32_t c = decodeAt(t, i, &len);
        i += len;
        if (c < 0x80) {
            if (std::isalnum((int)c)) out += (char)std::tolower((int)c);
            else if (c == ' ') out += '-';
            else if (c == '-' || c == '_') out += (char)c;
            continue;
        }
        // Punctuation and symbols go, as GitHub drops them; letters, marks
        // and digits of any script stay, lowercased where that is simple.
        const bool drop =
            (c >= 0xA0 && c <= 0xBF && c != 0xAA && c != 0xB2 && c != 0xB3 && c != 0xB5 &&
             c != 0xB9 && c != 0xBA && c != 0xBC && c != 0xBD && c != 0xBE) ||
            c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x206F) ||
            (c >= 0x20A0 && c <= 0x20CF) || (c >= 0x2190 && c <= 0x245F) ||
            (c >= 0x2500 && c <= 0x2BFF) || (c >= 0x2E00 && c <= 0x2E7F) ||
            (c >= 0x3000 && c <= 0x3004) || (c >= 0x3008 && c <= 0x3020) || c == 0x3030 ||
            (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFF01 && c <= 0xFF0F) ||
            (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
            (c >= 0xFF5B && c <= 0xFF65) || (c >= 0x1F000 && c <= 0x1FAFF);
        if (drop) continue;
        if ((c >= 0xC0 && c <= 0xDE) || (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) ||
            (c >= 0x410 && c <= 0x42F))
            c += 0x20;
        else if (c >= 0x400 && c <= 0x40F)
            c += 0x50;
        else if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && c % 2 == 0)
            c += 1;
        else if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && c % 2 == 1)
            c += 1;
        else if (c == 0x178)
            c = 0xFF;
        out += encodeUtf8(c);
    }
    return out;
}
