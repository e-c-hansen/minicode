// SymbolOutline.cpp — see SymbolOutline.h.
#include "SymbolOutline.h"

#include "MarkdownParser.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace Outline {

namespace {

using Lsp::Position;
using Lsp::Range;
using Lsp::Symbol;

bool isAsciiLetter(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char16_t c) { return isAsciiLetter(c) || c == '_' || c == '$' || c >= 0x80; }
bool isIdent(char16_t c) { return isIdentStart(c) || isDigit(c); }
bool isSpace(char16_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

std::string lowerExt(const std::string& name) {
    size_t slash = name.find_last_of('/');
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string e = base.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string extOf(const std::string& name) { return lowerExt(name); }

// Line starts, by LSP's rules (\n, \r\n and \r each end a line).
struct Lines {
    std::vector<size_t> starts{0};
    size_t size = 0;
    explicit Lines(const std::u16string& t) : size(t.size()) {
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i] == '\r') {
                if (i + 1 < t.size() && t[i + 1] == '\n') i++;
                starts.push_back(i + 1);
            } else if (t[i] == '\n') {
                starts.push_back(i + 1);
            }
        }
    }
    int count() const { return (int)starts.size(); }
    Position pos(size_t offset) const {
        auto it = std::upper_bound(starts.begin(), starts.end(), offset);
        int line = (int)(it - starts.begin()) - 1;
        return Position{line, (int)(offset - starts[(size_t)line])};
    }
    // The end of line `line`, before its line break, as a position.
    Position lineEnd(const std::u16string& t, int line) const {
        size_t a = starts[(size_t)line];
        size_t b = (size_t)line + 1 < starts.size() ? starts[(size_t)line + 1] : t.size();
        while (b > a && (t[b - 1] == '\n' || t[b - 1] == '\r')) b--;
        return Position{line, (int)(b - a)};
    }
    std::u16string text(const std::u16string& t, int line) const {
        Position e = lineEnd(t, line);
        return t.substr(starts[(size_t)line], (size_t)e.character);
    }
};

std::string oneLine(const std::u16string& s) {
    std::u16string out;
    bool space = false;
    for (char16_t c : s) {
        if (isSpace(c) || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return Lsp::toUtf8(out);
}

// Headings (Markdown's, LaTeX's sections) by level: each one's range runs to
// the line before the next heading of its level or a higher one, and it
// nests under the nearest heading above it of a higher level.
struct Heading {
    int level = 0;
    Symbol sym;
};
std::vector<Symbol> nestHeadings(std::vector<Heading> hs, const Lines& lines,
                                 const std::u16string& text) {
    std::vector<Symbol> out;
    std::vector<size_t> stack;   // indices into hs
    for (size_t i = 0; i < hs.size(); i++) {
        // Where it ends.
        int endLine = lines.count() - 1;
        for (size_t j = i + 1; j < hs.size(); j++)
            if (hs[j].level <= hs[i].level) {
                endLine = std::max(hs[i].sym.range.start.line, hs[j].sym.range.start.line - 1);
                break;
            }
        hs[i].sym.range.end = lines.lineEnd(text, endLine);
        while (!stack.empty() && hs[stack.back()].level >= hs[i].level) stack.pop_back();
        hs[i].sym.depth = (int)stack.size();
        hs[i].sym.container = stack.empty() ? "" : hs[stack.back()].sym.name;
        stack.push_back(i);
        out.push_back(hs[i].sym);
    }
    return out;
}

// UTF-16 units in a slice of UTF-8.
int utf16Length(const std::string& s, size_t a, size_t b) {
    int n = 0;
    for (size_t i = a; i < b && i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

}  // namespace

Language languageFor(const std::string& fileName) {
    static const std::set<std::string> md = {"md", "markdown", "mdown", "mkd", "mkdn", "mdx"};
    static const std::set<std::string> tex = {"tex", "ltx", "latex", "sty", "cls"};
    static const std::set<std::string> py = {"py", "pyi", "pyw"};
    static const std::set<std::string> c = {
        "c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx", "h++", "ipp", "inl", "tpp",
        "m", "mm", "java", "cs", "js", "jsx", "mjs", "cjs", "ts", "tsx", "mts", "cts", "go",
        "rs", "swift", "kt", "kts", "scala", "sc", "dart", "php", "groovy"};
    const std::string e = extOf(fileName);
    if (md.count(e)) return Language::Markdown;
    if (tex.count(e)) return Language::Latex;
    if (py.count(e)) return Language::Python;
    if (c.count(e)) return Language::CLike;
    return Language::None;
}

std::vector<Symbol> outline(const std::string& fileName, const std::u16string& text) {
    switch (languageFor(fileName)) {
    case Language::Markdown: return markdown(text);
    case Language::Latex: return latex(text);
    case Language::Python: return python(text);
    case Language::CLike: {
        const std::string e = extOf(fileName);
        return clike(text, e == "go" ? Dialect::Go : e == "rs" ? Dialect::Rust : Dialect::Plain);
    }
    case Language::None: break;
    }
    return {};
}

// ------------------------------------------------------------------ Markdown
std::vector<Symbol> markdown(const std::u16string& text) {
    const std::string src = Lsp::toUtf8(text);
    const std::vector<MdLine> md = MarkdownParser::lines(src);
    const Lines lines(text);
    std::vector<Heading> hs;
    std::string pending;     // a setext heading's text so far
    int pendingLine = -1;
    Range pendingSel;
    for (size_t i = 0; i < md.size() && (int)i < lines.count(); i++) {
        const MdLine& l = md[i];
        if (l.kind == MdLine::Heading) {
            size_t first = l.lineStart;
            while (first < l.lineEnd && (src[first] == ' ' || src[first] == '\t')) first++;
            const bool atx = first < l.lineEnd && src[first] == '#';
            const std::string body = l.end > l.start ? src.substr(l.start, l.end - l.start) : "";
            const int c0 = utf16Length(src, l.lineStart, l.start);
            const int c1 = utf16Length(src, l.lineStart, std::max(l.end, l.start));
            if (atx) {
                int level = 0;
                for (size_t k = first; k < l.lineEnd && src[k] == '#'; k++) level++;
                Heading h;
                h.level = level;
                h.sym.name = oneLine(Lsp::toUtf16(body));
                if (h.sym.name.empty()) continue;
                h.sym.kind = Lsp::kSymString;
                h.sym.range.start = Position{(int)i, 0};
                h.sym.selectionRange = Range{Position{(int)i, c0}, Position{(int)i, c1}};
                hs.push_back(h);
            } else {
                if (pendingLine < 0) {
                    pendingLine = (int)i;
                    pendingSel = Range{Position{(int)i, c0}, Position{(int)i, c1}};
                    pending = body;
                } else {
                    pending += " " + body;
                }
            }
            continue;
        }
        if (l.kind == MdLine::SetextUnderline && pendingLine >= 0) {
            size_t first = l.lineStart;
            while (first < l.lineEnd && (src[first] == ' ' || src[first] == '\t')) first++;
            Heading h;
            h.level = first < l.lineEnd && src[first] == '=' ? 1 : 2;
            h.sym.name = oneLine(Lsp::toUtf16(pending));
            h.sym.kind = Lsp::kSymString;
            h.sym.range.start = Position{pendingLine, 0};
            h.sym.selectionRange = pendingSel;
            if (!h.sym.name.empty()) hs.push_back(h);
        }
        pendingLine = -1;
        pending.clear();
    }
    return nestHeadings(hs, lines, text);
}

// ------------------------------------------------------------------- LaTeX
std::vector<Symbol> latex(const std::u16string& text) {
    static const std::pair<const char16_t*, int> kCommands[] = {
        {u"part", 0}, {u"chapter", 1}, {u"section", 2}, {u"subsection", 3},
        {u"subsubsection", 4}, {u"paragraph", 5}, {u"subparagraph", 6},
    };
    static const char16_t* kVerbatim[] = {u"verbatim", u"verbatim*", u"lstlisting", u"minted",
                                          u"comment", u"Verbatim", u"alltt"};
    const Lines lines(text);
    std::vector<Heading> hs;
    std::u16string inVerbatim;   // the environment's name while inside one
    for (int ln = 0; ln < lines.count(); ln++) {
        const std::u16string line = lines.text(text, ln);
        if (!inVerbatim.empty()) {
            if (line.find(u"\\end{" + inVerbatim + u"}") != std::u16string::npos)
                inVerbatim.clear();
            continue;
        }
        // The comment starts at a % no backslash escapes.
        size_t end = line.size();
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '\\') { i++; continue; }
            if (line[i] == '%') { end = i; break; }
        }
        for (size_t i = 0; i < end; i++) {
            if (line[i] != '\\') continue;
            size_t j = i + 1;
            while (j < end && isAsciiLetter(line[j])) j++;
            const std::u16string cmd = line.substr(i + 1, j - i - 1);
            if (cmd == u"begin" && j < end && line[j] == '{') {
                size_t close = line.find('}', j);
                if (close != std::u16string::npos) {
                    std::u16string env = line.substr(j + 1, close - j - 1);
                    for (const char16_t* v : kVerbatim)
                        if (env == v) {
                            inVerbatim = env;
                            // Closed on the same line?
                            if (line.find(u"\\end{" + env + u"}", close) != std::u16string::npos)
                                inVerbatim.clear();
                        }
                }
                if (!inVerbatim.empty()) break;
            }
            int level = -1;
            for (const auto& c : kCommands)
                if (cmd == c.first) level = c.second;
            if (level < 0) { i = j > i + 1 ? j - 1 : i; continue; }
            size_t k = j;
            if (k < end && line[k] == '*') k++;
            while (k < end && isSpace(line[k])) k++;
            if (k < end && line[k] == '[') {   // the short title
                int depth = 0;
                for (; k < end; k++) {
                    if (line[k] == '[') depth++;
                    else if (line[k] == ']' && --depth == 0) { k++; break; }
                }
                while (k < end && isSpace(line[k])) k++;
            }
            if (k >= end || line[k] != '{') { i = j - 1; continue; }
            size_t open = k, close = end;
            int depth = 0;
            for (; k < end; k++) {
                if (line[k] == '\\') { k++; continue; }
                if (line[k] == '{') depth++;
                else if (line[k] == '}' && --depth == 0) { close = k; break; }
            }
            Heading h;
            h.level = level;
            h.sym.name = oneLine(line.substr(open + 1, close - open - 1));
            if (h.sym.name.empty()) { i = k; continue; }
            h.sym.kind = Lsp::kSymString;
            h.sym.range.start = Position{ln, (int)i};
            h.sym.selectionRange = Range{Position{ln, (int)open + 1}, Position{ln, (int)close}};
            hs.push_back(h);
            i = k;
        }
    }
    return nestHeadings(hs, lines, text);
}

// ------------------------------------------------------------------ Python
std::vector<Symbol> python(const std::u16string& text) {
    const Lines lines(text);
    std::vector<Symbol> out;
    struct Open {
        int indent;
        size_t index;   // into out
    };
    std::vector<Open> stack;
    char16_t triple = 0;     // the quote of an open triple-quoted string
    int brackets = 0;        // open ( [ { at the line's start
    bool continued = false;  // the line before ended with a backslash
    int lastLine = 0;        // the last line with code on it
    auto closeTo = [&](int indent) {
        while (!stack.empty() && stack.back().indent >= indent) {
            out[stack.back().index].range.end = lines.lineEnd(text, lastLine);
            stack.pop_back();
        }
    };
    for (int ln = 0; ln < lines.count(); ln++) {
        const std::u16string line = lines.text(text, ln);
        const bool statement = !triple && brackets == 0 && !continued;
        size_t i = 0;
        int indent = 0;
        while (i < line.size() && isSpace(line[i])) {
            indent = line[i] == '\t' ? (indent / 8 + 1) * 8 : indent + 1;
            i++;
        }
        const bool blank = i >= line.size() || line[i] == '#';
        if (statement && !blank) {
            closeTo(indent);
            // async def, def, class
            size_t k = i;
            auto word = [&](const char16_t* w) {
                size_t n = std::char_traits<char16_t>::length(w);
                if (line.compare(k, n, w) != 0) return false;
                if (k + n < line.size() && isIdent(line[k + n])) return false;
                k += n;
                while (k < line.size() && isSpace(line[k])) k++;
                return true;
            };
            int kind = 0;
            if (word(u"class")) {
                kind = Lsp::kSymClass;
            } else {
                size_t save = k;
                if (!word(u"async")) k = save;
                if (word(u"def")) kind = Lsp::kSymFunction;
                else k = save;
            }
            if (kind && k < line.size() && isIdentStart(line[k])) {
                size_t n0 = k;
                while (k < line.size() && isIdent(line[k])) k++;
                Symbol s;
                s.name = Lsp::toUtf8(line.substr(n0, k - n0));
                s.kind = kind;
                if (kind == Lsp::kSymFunction && !stack.empty() &&
                    out[stack.back().index].kind == Lsp::kSymClass)
                    s.kind = Lsp::kSymMethod;
                s.depth = (int)stack.size();
                s.container = stack.empty() ? "" : out[stack.back().index].name;
                s.range.start = Position{ln, (int)i};
                s.range.end = lines.lineEnd(text, ln);
                s.selectionRange = Range{Position{ln, (int)n0}, Position{ln, (int)k}};
                stack.push_back(Open{indent, out.size()});
                out.push_back(s);
            }
        }
        // The line's strings, comments and brackets, for the lines after it.
        bool code = false;
        continued = false;
        for (size_t c = 0; c < line.size(); c++) {
            const char16_t ch = line[c];
            if (triple) {
                code = true;
                if (ch == '\\') { c++; continue; }
                if (ch == triple && line.compare(c, 3, std::u16string(3, triple)) == 0) {
                    triple = 0;
                    c += 2;
                }
                continue;
            }
            if (ch == '#') break;
            if (!isSpace(ch)) code = true;
            if (ch == '"' || ch == '\'') {
                if (line.compare(c, 3, std::u16string(3, ch)) == 0) {
                    triple = ch;
                    c += 2;
                    continue;
                }
                for (c++; c < line.size() && line[c] != ch; c++)
                    if (line[c] == '\\') c++;
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{') brackets++;
            else if ((ch == ')' || ch == ']' || ch == '}') && brackets > 0) brackets--;
            else if (ch == '\\' && c + 1 == line.size()) continued = true;
        }
        if (code) lastLine = ln;
    }
    closeTo(-1);
    return out;
}

// ------------------------------------------------------------- the C family
namespace {

struct Tok {
    std::u16string text;
    size_t at = 0;    // UTF-16 offset in the file
    int line = 0;     // its line, for splitting a header into lines
    char kind = 'p';  // i identifier, n number, s string, p punctuation
    bool is(const char16_t* s) const { return text == s; }
};

const std::set<std::u16string>& controlWords() {
    static const std::set<std::u16string> k = {
        u"if", u"for", u"while", u"switch", u"catch", u"return", u"sizeof", u"do", u"else",
        u"try", u"synchronized", u"using", u"lock", u"foreach", u"fixed", u"unchecked",
        u"checked", u"defer", u"go", u"select", u"match", u"loop", u"guard", u"with", u"when",
        u"typeof", u"new", u"delete", u"throw", u"case", u"assert", u"static_assert",
        u"decltype", u"alignof", u"noexcept", u"requires", u"function", u"elif", u"until",
        u"repeat", u"await", u"yield", u"in", u"is", u"as", u"and", u"or", u"not", u"forEach",
        u"__attribute__", u"__declspec", u"alignas", u"_Pragma", u"__pragma"};
    return k;
}

int containerKind(const std::u16string& w) {
    if (w == u"class" || w == u"record") return Lsp::kSymClass;
    if (w == u"struct" || w == u"union") return Lsp::kSymStruct;
    if (w == u"enum") return Lsp::kSymEnum;
    if (w == u"interface" || w == u"protocol" || w == u"trait") return Lsp::kSymInterface;
    if (w == u"namespace") return Lsp::kSymNamespace;
    if (w == u"mod" || w == u"module") return Lsp::kSymModule;
    if (w == u"impl" || w == u"extension" || w == u"object" || w == u"implementation")
        return Lsp::kSymObject;
    return 0;
}

bool classLike(int kind) {
    return kind == Lsp::kSymClass || kind == Lsp::kSymStruct || kind == Lsp::kSymInterface ||
           kind == Lsp::kSymObject || kind == Lsp::kSymEnum;
}

bool allCapsMacro(const std::u16string& w) {
    if (w.size() < 2) return false;
    bool underscore = false;
    for (char16_t c : w) {
        if (c >= 'a' && c <= 'z') return false;
        if (c == '_') underscore = true;
    }
    return underscore || w.size() > 3;
}

struct Decl {
    enum Type { None, Container, Transparent, Function } type = None;
    int kind = 0;
    std::u16string name, qualifier;
    size_t nameAt = 0, nameLen = 0, startAt = 0;
};

std::u16string joined(const std::vector<Tok>& t, size_t a, size_t b) {
    std::u16string out;
    for (size_t i = a; i < b; i++) {
        bool word = t[i].kind == 'i' || t[i].kind == 'n';
        if (!out.empty() && word && i > a && (t[i - 1].kind == 'i' || t[i - 1].kind == 'n'))
            out.push_back(' ');
        out += t[i].text;
    }
    return out;
}

// What the text before an opening brace declares. `h` is the header's
// tokens, balanced in their brackets.
Decl classify(const std::vector<Tok>& h, Dialect dialect) {
    Decl d;
    if (h.empty()) return d;
    // Drop attributes and annotations: [[x]], [Attr], #[derive(..)], @Override,
    // @Test(timeout = 5); keep Objective-C's @interface and @implementation.
    std::vector<Tok> t;
    std::vector<int> depth;   // bracket depth before each kept token
    int dep = 0;
    for (size_t i = 0; i < h.size(); i++) {
        const Tok& k = h[i];
        if (dep == 0 && (k.is(u"[") || (k.is(u"#") && i + 1 < h.size() && h[i + 1].is(u"[")))) {
            if (k.is(u"#")) i++;
            int b = 0;
            for (; i < h.size(); i++) {
                if (h[i].is(u"[")) b++;
                else if (h[i].is(u"]") && --b == 0) break;
            }
            continue;
        }
        if (dep == 0 && k.is(u"@") && i + 1 < h.size() && h[i + 1].kind == 'i') {
            if (containerKind(h[i + 1].text) && h[i + 1].text != u"object") continue;
            i++;
            if (i + 1 < h.size() && h[i + 1].is(u"(")) {
                int b = 0;
                for (i++; i < h.size(); i++) {
                    if (h[i].is(u"(")) b++;
                    else if (h[i].is(u")") && --b == 0) break;
                }
            }
            continue;
        }
        depth.push_back(dep);
        t.push_back(k);
        if (k.is(u"(") || k.is(u"[")) dep++;
        else if ((k.is(u")") || k.is(u"]")) && dep > 0) dep--;
    }
    if (t.empty()) return d;
    d.startAt = t[0].at;
    const size_t n = t.size();
    // An assignment declares nothing here: an initializer, a lambda, an
    // object literal (the = of a template's default and of operator= aside).
    for (size_t i = 0; i < n; i++) {
        if (t[i].is(u"template") && i + 1 < n && t[i + 1].is(u"<")) {
            int b = 0;
            for (i++; i < n; i++) {
                if (t[i].is(u"<")) b++;
                else if (t[i].is(u">") && --b == 0) break;
            }
            continue;
        }
        if (depth[i] != 0 || !t[i].is(u"=")) continue;
        bool op = false;
        for (size_t q = (i >= 3 ? i - 3 : 0); q < i; q++) op |= t[q].is(u"operator");
        if (!op) return d;
    }
    bool kotlinFun = false;   // fun String.ext(): a receiver before the name
    for (size_t i = 0; i < n; i++) kotlinFun |= t[i].is(u"fun");

    // Objective-C: - (void)name:(int)a with:(int)b
    if ((t[0].is(u"-") || t[0].is(u"+")) && n > 1 && t[1].is(u"(")) {
        size_t i = 1;
        int b = 0;
        for (; i < n; i++) {
            if (t[i].is(u"(")) b++;
            else if (t[i].is(u")") && --b == 0) break;
        }
        std::u16string name;
        size_t first = 0;
        for (i++; i < n; i++) {
            if (t[i].kind != 'i') continue;
            if (!first) first = i;
            if (i + 1 < n && t[i + 1].is(u":")) {
                name += t[i].text + u":";
                i += 2;   // past the colon; then (type) and the argument
                if (i < n && t[i].is(u"(")) {
                    int bb = 0;
                    for (; i < n; i++) {
                        if (t[i].is(u"(")) bb++;
                        else if (t[i].is(u")") && --bb == 0) break;
                    }
                }
                i++;   // the argument's name
            } else if (name.empty()) {
                name = t[i].text;
                break;
            }
        }
        if (name.empty() || !first) return d;
        d.type = Decl::Function;
        d.kind = Lsp::kSymMethod;
        d.name = name;
        d.nameAt = t[first].at;
        d.nameLen = t[first].text.size();
        return d;
    }

    // Go: type Name struct / interface
    if (dialect == Dialect::Go && t[0].is(u"type") && n >= 3 && t[1].kind == 'i') {
        int k = t[n - 1].is(u"struct") ? Lsp::kSymStruct
              : t[n - 1].is(u"interface") ? Lsp::kSymInterface : 0;
        if (k) {
            d.type = Decl::Container;
            d.kind = k;
            d.name = t[1].text;
            d.nameAt = t[1].at;
            d.nameLen = t[1].text.size();
            return d;
        }
    }

    // A type, namespace or the like.
    for (size_t i = 0; i < n; i++) {
        if (depth[i] != 0 || t[i].kind != 'i') continue;
        if (t[i].is(u"template") && i + 1 < n && t[i + 1].is(u"<")) {   // template <class T>
            int b = 0;
            for (i++; i < n; i++) {
                if (t[i].is(u"<")) b++;
                else if (t[i].is(u">") && --b == 0) break;
            }
            continue;
        }
        if (t[i].is(u"extern") && i + 1 < n && t[i + 1].kind == 's') {
            d.type = Decl::Transparent;
            return d;
        }
        const int kind = containerKind(t[i].text);
        if (!kind) continue;
        if (t[i].is(u"impl")) {
            size_t a = i + 1;
            if (a < n && t[a].is(u"<")) {
                int b = 0;
                for (; a < n; a++) {
                    if (t[a].is(u"<")) b++;
                    else if (t[a].is(u">") && --b == 0) { a++; break; }
                }
            }
            size_t e = a;
            while (e < n && !t[e].is(u"where")) e++;
            if (a >= e) { d.type = Decl::Transparent; return d; }
            d.type = Decl::Container;
            d.kind = kind;
            d.name = u"impl " + joined(t, a, e);
            d.nameAt = t[a].at;
            d.nameLen = t[a].text.size();
            return d;
        }
        size_t j = i + 1;
        // enum class X, enum struct X
        while (j < n && (t[j].is(u"class") || t[j].is(u"struct"))) j++;
        if (j >= n || t[j].kind != 'i') {   // anonymous
            d.type = Decl::Transparent;
            return d;
        }
        if (allCapsMacro(t[j].text) && j + 1 < n && t[j + 1].kind == 'i' &&
            !t[j + 1].is(u"final") && !t[j + 1].is(u"extends") && !t[j + 1].is(u"implements"))
            j++;   // class EXPORT_MACRO Name
        // struct S *f(int x) { ... } in C returns a struct; it is a function.
        if ((t[i].is(u"struct") || t[i].is(u"union") || t[i].is(u"enum")) && j + 1 < n &&
            (t[j + 1].kind == 'i' || t[j + 1].is(u"*") || t[j + 1].is(u"&"))) {
            bool paren = false;
            for (size_t q = j + 1; q < n; q++) paren |= t[q].is(u"(");
            if (paren) break;
        }
        d.type = Decl::Container;
        d.kind = kind;
        d.name = t[j].text;
        d.nameAt = t[j].at;
        d.nameLen = t[j].text.size();
        if (t[i].is(u"namespace")) {   // namespace a::b
            size_t e = j + 1;
            while (e + 2 < n && t[e].is(u":") && t[e + 1].is(u":") && t[e + 2].kind == 'i') e += 3;
            d.name = joined(t, j, e);
        }
        return d;
    }

    // A function: the name just before the first opening parenthesis.
    size_t open = n;
    for (size_t i = 0; i < n; i++)
        if (depth[i] == 0 && t[i].is(u"(")) { open = i; break; }
    if (open == n || open == 0) return d;
    // Go's method receiver: func (r *T) Name(...)
    if (dialect == Dialect::Go && t[0].is(u"func") && open == 1) {
        size_t i = 1;
        int b = 0;
        std::u16string recv;
        for (; i < n; i++) {
            if (t[i].is(u"(")) b++;
            else if (t[i].is(u")") && --b == 0) break;
            else if (t[i].kind == 'i') recv = t[i].text;
        }
        if (i + 2 < n && t[i + 1].kind == 'i' && (t[i + 2].is(u"(") || t[i + 2].is(u"["))) {
            d.type = Decl::Function;
            d.kind = Lsp::kSymMethod;
            d.name = t[i + 1].text;
            d.qualifier = recv;
            d.nameAt = t[i + 1].at;
            d.nameLen = t[i + 1].text.size();
        }
        return d;
    }
    // a.b(x) { is a call with a trailing closure, not a declaration
    for (size_t i = 0; i + 1 < open; i++)
        if (depth[i] == 0 && t[i].is(u".") && !kotlinFun) return d;
    for (size_t i = (open >= 4 ? open - 4 : 0); i < open; i++)
        if (t[i].is(u"operator")) {
            d.type = Decl::Function;
            d.kind = Lsp::kSymFunction;
            d.name = u"operator" + joined(t, i + 1, open);
            d.nameAt = t[i].at;
            d.nameLen = t[i].text.size();
            if (i >= 3 && t[i - 1].is(u":") && t[i - 2].is(u":") && t[i - 3].kind == 'i')
                d.qualifier = t[i - 3].text;
            return d;
        }
    size_t j = open - 1;
    if (t[j].is(u">")) {   // f<T>(
        int b = 0;
        for (;; j--) {
            if (t[j].is(u">")) b++;
            else if (t[j].is(u"<") && --b == 0) break;
            if (j == 0) return d;
        }
        if (j == 0) return d;
        j--;
    }
    if (t[j].kind != 'i' || controlWords().count(t[j].text)) return d;
    d.type = Decl::Function;
    d.kind = Lsp::kSymFunction;
    d.name = t[j].text;
    d.nameAt = t[j].at;
    d.nameLen = t[j].text.size();
    if (j >= 1 && t[j - 1].is(u"~")) {
        d.name = u"~" + d.name;
        d.nameAt = t[j - 1].at;
        d.nameLen += 1;
        j--;
    }
    if (j >= 3 && t[j - 1].is(u":") && t[j - 2].is(u":") && t[j - 3].kind == 'i')
        d.qualifier = t[j - 3].text;
    return d;
}

}  // namespace

std::vector<Symbol> clike(const std::u16string& text, Dialect dialect) {
    const Lines lines(text);
    std::vector<Symbol> out;
    struct Frame {
        bool code = false;
        int symbol = -1;   // into out, or -1
        int kind = 0;      // a container's kind
    };
    std::vector<Frame> stack;
    std::vector<Tok> header;
    const size_t n = text.size();
    bool lineStart = true;
    int line = 0;
    auto atDeclLevel = [&] { return stack.empty() || !stack.back().code; };
    auto newline = [&](size_t i) {
        // \r\n is one break
        if (text[i] == '\r' && i + 1 < n && text[i + 1] == '\n') return;
        line++;
        lineStart = true;
    };
    auto skipString = [&](size_t& i, char16_t q, bool multiline) {
        for (i++; i < n; i++) {
            if (text[i] == '\\') {
                if (i + 1 < n && (text[i + 1] == '\n' || text[i + 1] == '\r')) newline(i + 1);
                i++;
                continue;
            }
            if (text[i] == q) return;
            if (text[i] == '\n' || text[i] == '\r') {
                newline(i);
                if (!multiline) return;
            }
        }
    };
    auto declare = [&](size_t braceAt) {
        Frame f;
        f.code = true;
        if (!atDeclLevel()) {
            stack.push_back(f);
            return;
        }
        // The header may run over several lines; the declaration is the
        // shortest tail of whole lines that reads as one (Swift, Go and Kotlin
        // need no semicolon before it).
        Decl d;
        if (!header.empty()) {
            std::vector<size_t> lineStarts;   // indices where a header line begins
            for (size_t i = 0; i < header.size(); i++)
                if (i == 0 || header[i].line != header[i - 1].line) lineStarts.push_back(i);
            for (size_t k = lineStarts.size(); k-- > 0;) {
                std::vector<Tok> tail(header.begin() + (long)lineStarts[k], header.end());
                int b = 0;
                bool ok = true;
                for (const Tok& t : tail) {
                    if (t.is(u"(") || t.is(u"[")) b++;
                    else if (t.is(u")") || t.is(u"]")) {
                        if (--b < 0) { ok = false; break; }
                    }
                }
                if (!ok) continue;
                if (b != 0) break;   // still inside parentheses: a call's lambda
                d = classify(tail, dialect);
                if (d.type != Decl::None) break;
            }
        }
        header.clear();
        if (d.type == Decl::None) {
            stack.push_back(f);
            return;
        }
        if (d.type == Decl::Transparent) {
            f.code = false;
            stack.push_back(f);
            return;
        }
        int parent = -1, parentKind = 0, depth = 0;
        for (const Frame& s : stack)
            if (s.symbol >= 0) {
                parent = s.symbol;
                parentKind = s.kind;
                depth++;
            }
        Symbol s;
        s.name = Lsp::toUtf8(d.name);
        s.kind = d.kind;
        s.depth = depth;
        s.container = d.qualifier.empty() ? (parent >= 0 ? out[(size_t)parent].name : "")
                                          : Lsp::toUtf8(d.qualifier);
        if (d.type == Decl::Function && d.kind == Lsp::kSymFunction &&
            (classLike(parentKind) || !d.qualifier.empty())) {
            s.kind = Lsp::kSymMethod;
        }
        if (d.type == Decl::Function && s.kind == Lsp::kSymMethod &&
            (s.name == s.container || s.name == "constructor" || s.name == "init" ||
             s.name == "__construct"))
            s.kind = Lsp::kSymConstructor;
        s.range.start = lines.pos(d.startAt);
        s.range.end = lines.pos(braceAt + 1);
        s.selectionRange = Range{lines.pos(d.nameAt), lines.pos(d.nameAt + d.nameLen)};
        f.symbol = (int)out.size();
        f.kind = d.kind;
        f.code = d.type == Decl::Function;
        out.push_back(s);
        stack.push_back(f);
    };
    for (size_t i = 0; i < n; i++) {
        const char16_t c = text[i];
        if (c == '\n' || c == '\r') { newline(i); continue; }
        if (isSpace(c)) continue;
        const bool wasLineStart = lineStart;
        lineStart = false;
        // A preprocessor line (not Rust's #[attribute]).
        if (wasLineStart && c == '#' && i + 1 < n && (isAsciiLetter(text[i + 1]) || isSpace(text[i + 1]))) {
            for (; i < n; i++) {
                if (text[i] == '\\' && i + 1 < n && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
                    i++;
                    newline(i);
                    continue;
                }
                if (text[i] == '\n' || text[i] == '\r') { newline(i); break; }
            }
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i + 1 < n && text[i + 1] != '\n' && text[i + 1] != '\r') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            for (i += 2; i < n; i++) {
                if (text[i] == '\n' || text[i] == '\r') newline(i);
                else if (text[i] == '*' && i + 1 < n && text[i + 1] == '/') { i++; break; }
            }
            continue;
        }
        const size_t at = i;
        const int atLine = line;
        if (c == '"') {
            if (i + 2 < n && text[i + 1] == '"' && text[i + 2] == '"') {   // """text block"""
                for (i += 3; i < n; i++) {
                    if (text[i] == '\n' || text[i] == '\r') newline(i);
                    else if (text[i] == '"' && i + 2 < n && text[i + 1] == '"' && text[i + 2] == '"') {
                        i += 2;
                        break;
                    }
                }
            } else {
                skipString(i, '"', false);
            }
            if (atDeclLevel()) header.push_back(Tok{u"\"\"", at, atLine, 's'});
            continue;
        }
        if (c == '`') {
            skipString(i, '`', true);
            if (atDeclLevel()) header.push_back(Tok{u"``", at, atLine, 's'});
            continue;
        }
        if (c == '\'') {
            if (dialect == Dialect::Rust) {
                // 'x' and '\n' are characters; 'a is a lifetime.
                if (i + 2 < n && text[i + 1] != '\\' && text[i + 2] == '\'') { i += 2; continue; }
                if (i + 1 < n && text[i + 1] == '\\') {
                    size_t k = i + 2;
                    while (k < n && k < i + 12 && text[k] != '\'' && text[k] != '\n') k++;
                    if (k < n && text[k] == '\'') { i = k; continue; }
                }
                if (atDeclLevel()) header.push_back(Tok{u"'", at, atLine, 'p'});
                continue;
            }
            size_t k = i + 1;
            while (k < n && text[k] != '\'' && text[k] != '\n' && text[k] != '\r') {
                if (text[k] == '\\') k++;
                k++;
            }
            if (k < n && text[k] == '\'') {
                i = k;
                if (atDeclLevel()) header.push_back(Tok{u"''", at, atLine, 's'});
            } else if (atDeclLevel()) {
                header.push_back(Tok{u"'", at, atLine, 'p'});
            }
            continue;
        }
        if (c == '{') { declare(i); continue; }
        if (c == '}') {
            if (!stack.empty()) {
                const Frame f = stack.back();
                stack.pop_back();
                if (f.symbol >= 0) out[(size_t)f.symbol].range.end = lines.pos(i + 1);
            }
            header.clear();
            continue;
        }
        if (!atDeclLevel()) {
            // Inside a function: skip a word at a time.
            if (isIdent(c)) while (i + 1 < n && isIdent(text[i + 1])) i++;
            continue;
        }
        if (c == ';') { header.clear(); continue; }
        if (isIdentStart(c)) {
            size_t k = i;
            while (k + 1 < n && isIdent(text[k + 1])) k++;
            header.push_back(Tok{text.substr(i, k - i + 1), at, atLine, 'i'});
            i = k;
            continue;
        }
        if (isDigit(c)) {
            size_t k = i;
            while (k + 1 < n && (isIdent(text[k + 1]) || text[k + 1] == '.' || text[k + 1] == '\''))
                k++;
            header.push_back(Tok{text.substr(i, k - i + 1), at, atLine, 'n'});
            i = k;
            continue;
        }
        header.push_back(Tok{std::u16string(1, c), at, atLine, 'p'});
        // A header that has grown past any declaration is no header.
        if (header.size() > 400) header.erase(header.begin(), header.begin() + 200);
    }
    // Unclosed at the end: they run to it.
    for (const Frame& f : stack)
        if (f.symbol >= 0) out[(size_t)f.symbol].range.end = lines.pos(n);
    return out;
}

}  // namespace Outline
