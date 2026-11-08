// Snippet.cpp — see Snippet.h.
#include "Snippet.h"
#include "LineComments.h"
#include "Regex.h"
#include "TextFind.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>

namespace Snippet {

namespace {

// ---- text conversions

std::u32string toU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        char32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = (char32_t)(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); len = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = (char32_t)(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F));
            len = 3;
        } else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = (char32_t)(((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                            ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F));
            len = 4;
        }
        out += cp;
        i += len;
    }
    return out;
}

std::u16string u32to16(const std::u32string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out += (char16_t)(0xD800 + (c >> 10));
            out += (char16_t)(0xDC00 + (c & 0x3FF));
        } else {
            out += (char16_t)c;
        }
    }
    return out;
}

std::u32string u16to32(const std::u16string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        out += c;
    }
    return out;
}

std::u16string u16(const std::string& s) { return u32to16(toU32(s)); }

char32_t lowerOf(char32_t c) { return TextFind::foldCase(c); }
char32_t upperOf(char32_t c) { return TextFind::upperCase(c); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isVarStart(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isVarChar(char c) { return isVarStart(c) || isDigit(c); }

// ---- the parser

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    std::vector<Node> all() {
        std::vector<Node> out;
        any(out, false);
        return out;
    }

private:
    // Nodes until the end, or (inside a placeholder or a default) until an
    // unescaped }, which is left for the caller. False when the } is missing.
    bool any(std::vector<Node>& out, bool nested) {
        std::string text;
        auto flush = [&] {
            if (text.empty()) return;
            Node n;
            n.kind = Node::Kind::Text;
            n.text = std::move(text);
            text.clear();
            out.push_back(std::move(n));
        };
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size() &&
                (s_[i_ + 1] == '$' || s_[i_ + 1] == '}' || s_[i_ + 1] == '\\')) {
                text += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == '}' && nested) {
                flush();
                return true;
            }
            if (c == '$') {
                const size_t save = i_;
                Node n;
                if (dollar(n)) {
                    flush();
                    out.push_back(std::move(n));
                    continue;
                }
                i_ = save + 1;   // as written
                text += '$';
                continue;
            }
            text += c;
            i_++;
        }
        flush();
        return !nested;
    }

    bool number(int& out) {
        if (i_ >= s_.size() || !isDigit(s_[i_])) return false;
        long v = 0;
        while (i_ < s_.size() && isDigit(s_[i_])) {
            if (v < 100000000) v = v * 10 + (s_[i_] - '0');
            i_++;
        }
        out = (int)v;
        return true;
    }

    std::string name() {
        const size_t a = i_;
        while (i_ < s_.size() && isVarChar(s_[i_])) i_++;
        return s_.substr(a, i_ - a);
    }

    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }

    // At a $: a tab stop, placeholder, choice or variable.
    bool dollar(Node& n) {
        i_++;   // $
        if (i_ >= s_.size()) return false;
        if (isDigit(s_[i_])) {
            n.kind = Node::Kind::Tabstop;
            return number(n.index);
        }
        if (isVarStart(s_[i_])) {
            n.kind = Node::Kind::Variable;
            n.text = name();
            return true;
        }
        if (s_[i_] != '{') return false;
        i_++;
        if (i_ >= s_.size()) return false;
        if (isDigit(s_[i_])) {
            number(n.index);
            if (at('}')) {
                i_++;
                n.kind = Node::Kind::Tabstop;
                return true;
            }
            if (at(':')) {
                i_++;
                n.kind = Node::Kind::Placeholder;
                if (!any(n.children, true)) return false;
                i_++;   // }
                return true;
            }
            if (at('|')) {
                i_++;
                n.kind = Node::Kind::Choice;
                return choice(n);
            }
            if (at('/')) {
                n.kind = Node::Kind::Tabstop;
                n.transform = std::make_shared<Transform>();
                if (!transform(*n.transform) || !at('}')) return false;
                i_++;
                return true;
            }
            return false;
        }
        if (isVarStart(s_[i_])) {
            n.kind = Node::Kind::Variable;
            n.text = name();
            if (at('}')) {
                i_++;
                return true;
            }
            if (at(':')) {
                i_++;
                n.hasDefault = true;
                if (!any(n.children, true)) return false;
                i_++;
                return true;
            }
            if (at('/')) {
                n.transform = std::make_shared<Transform>();
                if (!transform(*n.transform) || !at('}')) return false;
                i_++;
                return true;
            }
        }
        return false;
    }

    // After ${n|: options split by commas, ending in |}.
    bool choice(Node& n) {
        std::string cur;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size() &&
                std::string(",|\\$}").find(s_[i_ + 1]) != std::string::npos) {
                cur += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == ',') {
                n.choices.push_back(cur);
                cur.clear();
                i_++;
                continue;
            }
            if (c == '|') {
                if (i_ + 1 >= s_.size() || s_[i_ + 1] != '}') return false;
                n.choices.push_back(cur);
                i_ += 2;
                return true;
            }
            cur += c;
            i_++;
        }
        return false;
    }

    // Text up to an unescaped character in `ends` (left for the caller);
    // a backslash escapes $, \, / and the ends. False at the end.
    bool textUntil(const std::string& ends, std::string& out) {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size() &&
                (ends.find(s_[i_ + 1]) != std::string::npos ||
                 std::string("$\\/").find(s_[i_ + 1]) != std::string::npos)) {
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (ends.find(c) != std::string::npos) return true;
            out += c;
            i_++;
        }
        return false;
    }

    // At the first /: regex/format/options, stopping before the closing }.
    bool transform(Transform& t) {
        i_++;   // /
        // The regex keeps its escapes for the regex engine, except \/.
        bool closed = false;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                if (s_[i_ + 1] == '/') t.regex += '/';
                else {
                    t.regex += c;
                    t.regex += s_[i_ + 1];
                }
                i_ += 2;
                continue;
            }
            if (c == '/') {
                closed = true;
                i_++;
                break;
            }
            t.regex += c;
            i_++;
        }
        if (!closed) return false;
        // The format.
        std::string text;
        auto flush = [&] {
            if (text.empty()) return;
            FormatItem f;
            f.kind = FormatItem::Kind::Text;
            f.text = std::move(text);
            text.clear();
            t.format.push_back(std::move(f));
        };
        closed = false;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size() &&
                std::string("$\\/").find(s_[i_ + 1]) != std::string::npos) {
                text += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == '/') {
                closed = true;
                i_++;
                break;
            }
            if (c == '$') {
                const size_t save = i_;
                FormatItem f;
                if (formatItem(f)) {
                    flush();
                    t.format.push_back(std::move(f));
                    continue;
                }
                i_ = save + 1;
                text += '$';
                continue;
            }
            text += c;
            i_++;
        }
        flush();
        if (!closed) return false;
        while (i_ < s_.size() && ((s_[i_] >= 'a' && s_[i_] <= 'z') || (s_[i_] >= 'A' && s_[i_] <= 'Z')))
            t.options += s_[i_++];
        return true;
    }

    bool formatItem(FormatItem& f) {
        i_++;   // $
        if (i_ < s_.size() && isDigit(s_[i_])) {
            f.kind = FormatItem::Kind::Group;
            return number(f.group);
        }
        if (!at('{')) return false;
        i_++;
        if (!number(f.group)) return false;
        if (at('}')) {
            i_++;
            f.kind = FormatItem::Kind::Group;
            return true;
        }
        if (!at(':')) return false;
        i_++;
        if (at('/')) {
            i_++;
            const std::string word = name();
            if (!at('}')) return false;
            i_++;
            if (word == "upcase") f.kind = FormatItem::Kind::Upcase;
            else if (word == "downcase") f.kind = FormatItem::Kind::Downcase;
            else if (word == "capitalize") f.kind = FormatItem::Kind::Capitalize;
            else if (word == "camelcase") f.kind = FormatItem::Kind::Camelcase;
            else if (word == "pascalcase") f.kind = FormatItem::Kind::Pascalcase;
            else return false;
            return true;
        }
        f.kind = FormatItem::Kind::Conditional;
        if (at('+')) {
            i_++;
            f.hasIf = true;
            if (!textUntil("}", f.ifText)) return false;
        } else if (at('?')) {
            i_++;
            f.hasIf = f.hasElse = true;
            if (!textUntil(":", f.ifText)) return false;
            i_++;
            if (!textUntil("}", f.elseText)) return false;
        } else {
            if (at('-')) i_++;
            f.hasElse = true;
            if (!textUntil("}", f.elseText)) return false;
        }
        i_++;   // }
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
};

// ---- transforms

std::u32string mapCase(const std::u32string& s, char32_t (*f)(char32_t)) {
    std::u32string out = s;
    for (char32_t& c : out) c = f(c);
    return out;
}

bool isWordCp(char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// VS Code's: the runs of [a-z0-9] (any case), joined, each but the first
// (camel) or every one (pascal) with its first letter capital.
std::u32string joinWords(const std::u32string& s, bool pascal) {
    std::u32string out;
    bool any = false;
    size_t i = 0;
    while (i < s.size()) {
        if (!isWordCp(s[i])) { i++; continue; }
        size_t j = i;
        while (j < s.size() && isWordCp(s[j])) j++;
        std::u32string w = s.substr(i, j - i);
        w[0] = (pascal || any) ? upperOf(w[0]) : lowerOf(w[0]);
        out += w;
        any = true;
        i = j;
    }
    return any ? out : s;
}

}  // namespace

std::vector<Node> parse(const std::string& snippet) { return Parser(snippet).all(); }

std::string plainText(const std::vector<Node>& nodes) {
    std::string out;
    for (const Node& n : nodes) {
        switch (n.kind) {
        case Node::Kind::Text: out += n.text; break;
        case Node::Kind::Tabstop: break;
        case Node::Kind::Placeholder: out += plainText(n.children); break;
        case Node::Kind::Choice: if (!n.choices.empty()) out += n.choices[0]; break;
        case Node::Kind::Variable: if (n.hasDefault) out += plainText(n.children); break;
        }
    }
    return out;
}

std::u16string applyTransform(const Transform& t, const std::u16string& value) {
    const bool ignoreCase = t.options.find('i') != std::string::npos;
    const bool global = t.options.find('g') != std::string::npos;
    auto program = Regex::compile(toU32(t.regex), ignoreCase);
    if (!program) return value;
    const std::u32string original = u16to32(value);
    std::u32string folded = original;
    if (ignoreCase) folded = mapCase(original, &lowerOf);
    std::u32string out;
    std::vector<size_t> caps;
    size_t pos = 0;
    while (pos <= original.size()) {
        if (Regex::search(*program, folded.data(), folded.size(), pos, {}, caps) !=
            Regex::Result::Found)
            break;
        const size_t a = caps[0], b = caps[1];
        out += original.substr(pos, a - pos);
        auto group = [&](int g) -> std::u32string {
            const size_t k = (size_t)g * 2;
            if (g < 0 || k + 1 >= caps.size() || caps[k] == Regex::npos) return U"";
            return original.substr(caps[k], caps[k + 1] - caps[k]);
        };
        for (const FormatItem& f : t.format) {
            switch (f.kind) {
            case FormatItem::Kind::Text: out += toU32(f.text); break;
            case FormatItem::Kind::Group: out += group(f.group); break;
            case FormatItem::Kind::Upcase: out += mapCase(group(f.group), &upperOf); break;
            case FormatItem::Kind::Downcase: out += mapCase(group(f.group), &lowerOf); break;
            case FormatItem::Kind::Capitalize: {
                std::u32string g = group(f.group);
                if (!g.empty()) g[0] = upperOf(g[0]);
                out += g;
                break;
            }
            case FormatItem::Kind::Camelcase: out += joinWords(group(f.group), false); break;
            case FormatItem::Kind::Pascalcase: out += joinWords(group(f.group), true); break;
            case FormatItem::Kind::Conditional: {
                const std::u32string g = group(f.group);
                if (!g.empty()) out += f.hasIf ? toU32(f.ifText) : g;
                else if (f.hasElse) out += toU32(f.elseText);
                break;
            }
            }
        }
        pos = b;
        if (!global) break;
        if (b == a) {
            // An empty match: step past a character so the search moves on.
            if (pos < original.size()) out += original[pos];
            pos++;
        }
    }
    if (pos < original.size()) out += original.substr(pos);
    return u32to16(out);
}

void commentsFor(const std::string& filename, std::string& line, std::string& blockStart,
                 std::string& blockEnd) {
    line = LineComments::markerFor(filename);
    std::string ext;
    const size_t dot = filename.find_last_of('.');
    const size_t slash = filename.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        ext = filename.substr(dot + 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    static const std::set<std::string> cLike = {
        "c", "h", "cc", "cpp", "cxx", "hpp", "hh", "hxx", "m", "mm", "java", "js", "jsx",
        "mjs", "cjs", "ts", "tsx", "mts", "cts", "go", "rs", "swift", "kt", "kts", "scala",
        "cs", "dart", "css", "scss", "less", "php", "groovy", "jsonc", "zig", "sql"};
    static const std::set<std::string> markup = {"html", "htm", "xml", "md", "markdown", "svg",
                                                 "vue", "svelte"};
    blockStart.clear();
    blockEnd.clear();
    if (cLike.count(ext)) {
        blockStart = "/*";
        blockEnd = "*/";
    } else if (markup.count(ext)) {
        blockStart = "<!--";
        blockEnd = "-->";
    } else if (ext == "py" || ext == "pyi") {
        blockStart = blockEnd = "\"\"\"";
    } else if (ext == "lua") {
        blockStart = "--[[";
        blockEnd = "]]";
    } else if (ext == "hs") {
        blockStart = "{-";
        blockEnd = "-}";
    }
}

bool variable(const Context& ctx, const std::string& name, std::string& value) {
    auto baseName = [](const std::string& p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos ? p : p.substr(s + 1);
    };
    auto two = [](int v) {
        char b[8];
        std::snprintf(b, sizeof b, "%02d", v);
        return std::string(b);
    };
    static const char* const kMonths[] = {"January", "February", "March", "April", "May",
                                          "June", "July", "August", "September", "October",
                                          "November", "December"};
    static const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
    auto random32 = [&]() -> uint32_t {
        if (ctx.random) return ctx.random();
        static std::random_device rd;
        return rd();
    };
    const std::tm& t = ctx.time;
    if (name == "TM_SELECTED_TEXT") value = ctx.selectedText;
    else if (name == "TM_CURRENT_LINE") value = ctx.currentLine;
    else if (name == "TM_CURRENT_WORD") value = ctx.currentWord;
    else if (name == "TM_LINE_INDEX") value = std::to_string(ctx.lineIndex);
    else if (name == "TM_LINE_NUMBER") value = std::to_string(ctx.lineIndex + 1);
    else if (name == "TM_FILENAME") value = baseName(ctx.filePath);
    else if (name == "TM_FILENAME_BASE") {
        value = baseName(ctx.filePath);
        const size_t dot = value.find_last_of('.');
        if (dot != std::string::npos && dot > 0) value.resize(dot);
    } else if (name == "TM_DIRECTORY") {
        const size_t s = ctx.filePath.find_last_of('/');
        value = s == std::string::npos ? "" : (s == 0 ? "/" : ctx.filePath.substr(0, s));
    } else if (name == "TM_FILEPATH") value = ctx.filePath;
    else if (name == "RELATIVE_FILEPATH") {
        const std::string& w = ctx.workspaceFolder;
        if (!w.empty() && ctx.filePath.size() > w.size() &&
            ctx.filePath.compare(0, w.size(), w) == 0 && ctx.filePath[w.size()] == '/')
            value = ctx.filePath.substr(w.size() + 1);
        else value = ctx.filePath;
    } else if (name == "CLIPBOARD") value = ctx.clipboard;
    else if (name == "WORKSPACE_NAME") value = baseName(ctx.workspaceFolder);
    else if (name == "WORKSPACE_FOLDER") value = ctx.workspaceFolder;
    else if (name == "CURSOR_INDEX") value = "0";
    else if (name == "CURSOR_NUMBER") value = "1";
    else if (name == "CURRENT_YEAR") value = std::to_string(t.tm_year + 1900);
    else if (name == "CURRENT_YEAR_SHORT") value = two((t.tm_year + 1900) % 100);
    else if (name == "CURRENT_MONTH") value = two(t.tm_mon + 1);
    else if (name == "CURRENT_MONTH_NAME") value = kMonths[((t.tm_mon % 12) + 12) % 12];
    else if (name == "CURRENT_MONTH_NAME_SHORT")
        value = std::string(kMonths[((t.tm_mon % 12) + 12) % 12]).substr(0, 3);
    else if (name == "CURRENT_DATE") value = two(t.tm_mday);
    else if (name == "CURRENT_DAY_NAME") value = kDays[((t.tm_wday % 7) + 7) % 7];
    else if (name == "CURRENT_DAY_NAME_SHORT")
        value = std::string(kDays[((t.tm_wday % 7) + 7) % 7]).substr(0, 3);
    else if (name == "CURRENT_HOUR") value = two(t.tm_hour);
    else if (name == "CURRENT_MINUTE") value = two(t.tm_min);
    else if (name == "CURRENT_SECOND") value = two(t.tm_sec);
    else if (name == "CURRENT_SECONDS_UNIX") value = std::to_string((long long)ctx.unixSeconds);
    else if (name == "CURRENT_TIMEZONE_OFFSET") {
        const long off = ctx.timezoneOffsetSeconds;
        const long m = (off < 0 ? -off : off) / 60;
        value = std::string(off < 0 ? "-" : "+") + two((int)(m / 60)) + ":" + two((int)(m % 60));
    } else if (name == "RANDOM") {
        char b[16];
        std::snprintf(b, sizeof b, "%06u", (unsigned)(random32() % 1000000));
        value = b;
    } else if (name == "RANDOM_HEX") {
        char b[16];
        std::snprintf(b, sizeof b, "%06x", (unsigned)(random32() & 0xFFFFFF));
        value = b;
    } else if (name == "UUID") {
        uint8_t bytes[16];
        for (int k = 0; k < 16; k += 4) {
            const uint32_t r = random32();
            for (int j = 0; j < 4; j++) bytes[k + j] = (uint8_t)(r >> (8 * j));
        }
        bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);   // version 4
        bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);   // the RFC's variant
        char b[40];
        std::snprintf(b, sizeof b,
                      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                      bytes[14], bytes[15]);
        value = b;
    } else if (name == "LINE_COMMENT") value = ctx.lineComment;
    else if (name == "BLOCK_COMMENT_START") value = ctx.blockCommentStart;
    else if (name == "BLOCK_COMMENT_END") value = ctx.blockCommentEnd;
    else return false;
    return true;
}

// ---- expansion

namespace {

struct Place {
    int index = 0;
    Range range;
    std::shared_ptr<Transform> transform;
    size_t seq = 0;
};

class Emitter {
public:
    Emitter(const Context& ctx, const std::vector<Node>& nodes) : ctx_(ctx) {
        int highest = 0;
        findDefining(nodes, highest);
        nextUnknown_ = highest + 1;
    }

    void emit(const std::vector<Node>& nodes) {
        for (const Node& n : nodes) emitNode(n);
    }

    std::u16string out;
    std::vector<Place> places;

private:
    void findDefining(const std::vector<Node>& nodes, int& highest) {
        for (const Node& n : nodes) {
            if (n.index > highest) highest = n.index;
            if ((n.kind == Node::Kind::Placeholder || n.kind == Node::Kind::Choice) &&
                !defining_.count(n.index))
                defining_[n.index] = &n;
            findDefining(n.children, highest);
        }
    }

    // Text the snippet wrote: its line breaks become the file's, followed
    // by the line's indentation, and tabs at a line's start the unit.
    void emitText(const std::string& s) {
        const std::u16string t = u16(s);
        for (size_t i = 0; i < t.size(); i++) {
            const char16_t c = t[i];
            if (c == u'\r' && i + 1 < t.size() && t[i + 1] == u'\n') continue;
            if (c == u'\n' || c == u'\r') {
                out += ctx_.eol;
                out += ctx_.indent;
                lineStart_ = true;
                continue;
            }
            if (c == u'\t' && lineStart_) {
                out += ctx_.unit;
                continue;
            }
            lineStart_ = false;
            out += c;
        }
    }

    // A value from outside (a variable, a choice): as it is.
    void emitRaw(const std::u16string& s) {
        if (s.empty()) return;
        out += s;
        lineStart_ = false;
    }

    // The text a mirror of `index` shows: its placeholder's, as plain text.
    std::u16string mirrorText(int index) {
        auto it = defining_.find(index);
        if (it == defining_.end() || depth_ > 8) return u"";
        Emitter sub(*this);
        sub.out.clear();
        sub.places.clear();
        sub.recording_ = false;
        sub.lineStart_ = false;
        sub.depth_ = depth_ + 1;
        const Node& d = *it->second;
        if (d.kind == Node::Kind::Choice) {
            if (!d.choices.empty()) sub.emitRaw(u16(d.choices[0]));
        } else {
            sub.emit(d.children);
        }
        return sub.out;
    }

    void record(int index, size_t start, size_t seq, std::shared_ptr<Transform> t) {
        if (!recording_) return;
        Place p;
        p.index = index;
        p.range = {start, out.size()};
        p.transform = std::move(t);
        p.seq = seq;
        places.push_back(std::move(p));
    }

    void emitNode(const Node& n) {
        switch (n.kind) {
        case Node::Kind::Text:
            emitText(n.text);
            return;
        case Node::Kind::Tabstop:
        case Node::Kind::Placeholder:
        case Node::Kind::Choice: {
            const size_t seq = seq_++;
            const size_t start = out.size();
            auto it = defining_.find(n.index);
            if (it != defining_.end() && it->second == &n && recording_) {
                if (n.kind == Node::Kind::Choice) {
                    if (!n.choices.empty()) emitRaw(u16(n.choices[0]));
                } else {
                    emit(n.children);
                }
                record(n.index, start, seq, nullptr);
                return;
            }
            std::u16string text = mirrorText(n.index);
            if (n.transform) text = applyTransform(*n.transform, text);
            emitRaw(text);
            record(n.index, start, seq, n.transform);
            return;
        }
        case Node::Kind::Variable: {
            std::string value;
            if (variable(ctx_, n.text, value)) {
                if (n.transform) {
                    emitRaw(applyTransform(*n.transform, u16(value)));
                } else if (value.empty() && n.hasDefault) {
                    emit(n.children);
                } else {
                    emitRaw(u16(value));
                }
                return;
            }
            // Unknown: a placeholder holding its default, else its name.
            const size_t seq = seq_++;
            const size_t start = out.size();
            if (n.hasDefault) emit(n.children);
            else emitRaw(u16(n.text));
            if (recording_) record(nextUnknown_++, start, seq, nullptr);
            return;
        }
        }
    }

    const Context& ctx_;
    std::map<int, const Node*> defining_;
    bool lineStart_ = false;
    bool recording_ = true;
    int depth_ = 0;
    int nextUnknown_ = 1;
    size_t seq_ = 0;
};

}  // namespace

Expansion expand(const std::vector<Node>& nodes, const Context& ctx) {
    Emitter e(ctx, nodes);
    e.emit(nodes);
    std::vector<Place> places = std::move(e.places);
    bool hasFinal = false;
    for (const Place& p : places)
        if (p.index == 0) hasFinal = true;
    if (!hasFinal) {
        Place p;
        p.index = 0;
        p.range = {e.out.size(), e.out.size()};
        p.seq = (size_t)-1;
        places.push_back(p);
    }
    std::stable_sort(places.begin(), places.end(), [](const Place& a, const Place& b) {
        if (a.range.start != b.range.start) return a.range.start < b.range.start;
        return a.seq < b.seq;
    });
    std::map<int, Stop> byIndex;
    for (const Place& p : places) {
        Stop& s = byIndex[p.index];
        s.index = p.index;
        s.ranges.push_back(p.range);
        s.transforms.push_back(p.transform);
    }
    // Choices from the stop's choice node.
    std::function<void(const std::vector<Node>&)> choices = [&](const std::vector<Node>& ns) {
        for (const Node& n : ns) {
            if (n.kind == Node::Kind::Choice && byIndex.count(n.index) &&
                byIndex[n.index].choices.empty())
                for (const std::string& c : n.choices) byIndex[n.index].choices.push_back(u16(c));
            choices(n.children);
        }
    };
    choices(nodes);
    Expansion out;
    out.text = std::move(e.out);
    for (auto& kv : byIndex)
        if (kv.first != 0) out.stops.push_back(std::move(kv.second));
    out.stops.push_back(std::move(byIndex[0]));
    return out;
}

// ---- the session

Session::Session(const Expansion& e, size_t base) {
    // Each range's place in the text, for ties between ranges at one point.
    struct Ref { size_t stop, range, start, end; };
    std::vector<Ref> refs;
    stops_ = e.stops;
    for (size_t s = 0; s < stops_.size(); s++) {
        order_.emplace_back(stops_[s].ranges.size(), 0);
        for (size_t r = 0; r < stops_[s].ranges.size(); r++) {
            Range& rg = stops_[s].ranges[r];
            refs.push_back({s, r, rg.start, rg.end});
            rg.start += base;
            rg.end += base;
        }
    }
    // Text order: by start; at one start the enclosing (longer) one first,
    // so an outer placeholder comes before what it holds; the final stop
    // last among empty ranges at its point.
    std::stable_sort(refs.begin(), refs.end(), [&](const Ref& a, const Ref& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end > b.end;
        const bool fa = stops_[a.stop].index == 0, fb = stops_[b.stop].index == 0;
        if (fa != fb) return fb;
        return a.stop < b.stop;
    });
    for (size_t k = 0; k < refs.size(); k++) order_[refs[k].stop][refs[k].range] = k;
    for (const Stop& s : stops_) dead_.push_back(std::vector<bool>(s.ranges.size(), false));
    span_ = {base, base + e.text.size()};
    current_ = 0;
    active_ = !stops_.empty();
}

std::vector<Range> Session::caretRanges() const {
    std::vector<Range> out;
    if (stops_.empty()) return out;
    const Stop& s = stops_[current_];
    for (size_t r = 0; r < s.ranges.size(); r++)
        if (!s.transforms[r] && !dead_[current_][r]) out.push_back(s.ranges[r]);
    return out;
}

bool Session::dead(size_t stop) const {
    for (bool d : dead_[stop])
        if (!d) return false;
    return true;
}

bool Session::next() {
    for (size_t j = current_ + 1; j < stops_.size(); j++) {
        if (dead(j)) continue;
        if (stops_[j].index != 0) {
            bool carets = false;   // a stop of transformed copies only has nowhere to type
            for (size_t r = 0; r < stops_[j].ranges.size(); r++)
                if (!stops_[j].transforms[r] && !dead_[j][r]) carets = true;
            if (!carets) continue;
        }
        current_ = j;
        return true;
    }
    return false;
}

bool Session::prev() {
    for (size_t j = current_; j-- > 0;) {
        if (dead(j) || stops_[j].index == 0) continue;
        bool carets = false;
        for (size_t r = 0; r < stops_[j].ranges.size(); r++)
            if (!stops_[j].transforms[r] && !dead_[j][r]) carets = true;
        if (!carets) continue;
        current_ = j;
        return true;
    }
    return false;
}

bool Session::holds(const std::vector<Range>& selections) const {
    if (!active_ || stops_.empty()) return false;
    const std::vector<Range> places = caretRanges();
    for (const Range& sel : selections) {
        bool in = false;
        for (const Range& r : places)
            if (r.start <= sel.start && sel.end <= r.end) in = true;
        if (!in) return false;
    }
    return true;
}

// Whether a range grows when typing lands at its edges: the current stop's,
// and those enclosing one of them (an outer placeholder holding the one
// being typed in).
bool Session::sticky(size_t stop, size_t range) const {
    if (stop == current_) return true;
    const Range& R = stops_[stop].ranges[range];
    const Stop& cur = stops_[current_];
    for (size_t c = 0; c < cur.ranges.size(); c++) {
        if (dead_[current_][c]) continue;
        const Range& C = cur.ranges[c];
        if (R.start <= C.start && C.end <= R.end &&
            (R.end - R.start > C.end - C.start || order_[stop][range] < order_[current_][c]))
            return true;
    }
    return false;
}

void Session::edited(size_t a, size_t removed, size_t inserted) {
    if (!active_) return;
    const size_t b = a + removed;
    const long d = (long)inserted - (long)removed;
    auto shift = [d](size_t p) { return (size_t)((long)p + d); };
    // Decided on the ranges as they were.
    std::vector<std::vector<bool>> grows(stops_.size());
    std::vector<std::vector<size_t>> endsBefore(stops_.size());
    for (size_t s = 0; s < stops_.size(); s++)
        for (const Range& r : stops_[s].ranges) endsBefore[s].push_back(r.end);
    for (size_t s = 0; s < stops_.size(); s++)
        for (size_t r = 0; r < stops_[s].ranges.size(); r++)
            grows[s].push_back(!dead_[s][r] && sticky(s, r));
    // An empty range where typing lands follows the typing when a growing
    // range ends there and comes before it in the text ($1$0, ${1:x}$0).
    auto followsTyping = [&](size_t s, size_t r) {
        for (size_t s2 = 0; s2 < stops_.size(); s2++)
            for (size_t r2 = 0; r2 < stops_[s2].ranges.size(); r2++)
                if (grows[s2][r2] && endsBefore[s2][r2] == a &&
                    order_[s2][r2] < order_[s][r])
                    return true;
        return false;
    };
    for (size_t s = 0; s < stops_.size(); s++) {
        for (size_t r = 0; r < stops_[s].ranges.size(); r++) {
            if (dead_[s][r]) continue;
            Range& R = stops_[s].ranges[r];
            const bool g = grows[s][r];
            size_t st = R.start, en = R.end;
            if (removed == 0) {
                if (a < st) {
                    st = shift(st);
                    en = shift(en);
                } else if (a > en) {
                    // after it: unchanged
                } else if (a > st && a < en) {
                    en = shift(en);
                } else if (g) {
                    en = shift(en);   // at an edge: grows
                } else if (st == en) {
                    if (followsTyping(s, r)) st = en = shift(st);
                } else if (a == st) {
                    st = shift(st);
                    en = shift(en);
                }
            } else {
                // Inside what was replaced: gone, unless it is the stop
                // being typed over itself (or one around it), exactly.
                const bool within = st == en ? (a < st && st < b) : (a <= st && en <= b);
                const bool exactly = a == st && en == b;
                if (within && (!g || !exactly)) {
                    dead_[s][r] = true;
                    continue;
                }
                if (st == en) {
                    if (st >= b) st = en = shift(st);
                } else {
                    st = st < a ? st : (st >= b ? shift(st) : (g ? a : a + inserted));
                    en = en <= a ? en : (en >= b ? shift(en) : a + inserted);
                    if (en < st) en = st;
                }
            }
            R.start = st;
            R.end = en;
        }
    }
    // The whole snippet's span grows with typing at its edges.
    if (removed > 0 && a <= span_.start && span_.end <= b && (a < span_.start || b > span_.end)) {
        active_ = false;
    } else {
        if (b <= span_.start && !(removed == 0 && a == span_.start)) {
            span_.start = shift(span_.start);
            span_.end = shift(span_.end);
        } else if (a < span_.end || (removed == 0 && a == span_.end)) {
            if (a < span_.start) span_.start = a;
            span_.end = b >= span_.end ? a + inserted : shift(span_.end);
        }
    }
    if (dead(current_)) active_ = false;
    bool anyAlive = false;
    for (size_t s = 0; s < stops_.size(); s++)
        if (!dead(s)) anyAlive = true;
    if (!anyAlive) active_ = false;
}

std::vector<Session::Replacement> Session::transformsFor(const std::u16string& value) const {
    std::vector<Replacement> out;
    if (stops_.empty()) return out;
    const Stop& s = stops_[current_];
    for (size_t r = 0; r < s.ranges.size(); r++) {
        if (!s.transforms[r] || dead_[current_][r]) continue;
        Replacement rep;
        rep.start = s.ranges[r].start;
        rep.length = s.ranges[r].end - s.ranges[r].start;
        rep.text = applyTransform(*s.transforms[r], value);
        out.push_back(std::move(rep));
    }
    std::sort(out.begin(), out.end(),
              [](const Replacement& x, const Replacement& y) { return x.start < y.start; });
    return out;
}

}  // namespace Snippet
