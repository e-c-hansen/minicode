// SpellCheck.cpp — pure C++. See SpellCheck.h.
#include "SpellCheck.h"

#include <algorithm>
#include <cstdint>

#include "MarkdownParser.h"
#include "SyntaxHighlighter.h"

namespace Spell {

namespace {

bool isAsciiLetter(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isLower(char16_t c) { return c >= 'a' && c <= 'z'; }
bool isUpper(char16_t c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }

// A letter of some alphabet: ASCII letters, and from U+00C0 on whatever is
// not punctuation, a symbol, a surrogate (emoji and the like) or a
// full-width sign.
bool isLetter(char16_t c) {
    if (isAsciiLetter(c)) return true;
    if (c < 0xC0 || c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;   // punctuation, symbols, arrows, math
    if (c >= 0x3000 && c <= 0x303F) return false;   // CJK punctuation
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    if (c >= 0xFE30 && c <= 0xFE4F) return false;
    if (c >= 0xFF00 && c <= 0xFF0F) return false;
    return true;
}

bool isSpace(char16_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x2028 ||
           c == 0x2029 || c == 0x3000 || c == '\f' || c == '\v';
}

// Where a chunk of prose ends: blanks, and dashes and an ellipsis written
// without spaces around them ("this—that").
bool endsChunk(char16_t c) { return isSpace(c) || c == 0x2013 || c == 0x2014 || c == 0x2026; }

bool trimmedFront(char16_t c) {
    return c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' || c == '*' || c == '_' ||
           c == '~' || c == 0x2018 || c == 0x201C || c == 0xBF || c == 0xA1 || c == 0xAB;
}
bool trimmedBack(char16_t c) {
    return c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || c == '*' || c == '_' ||
           c == '~' || c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' ||
           c == 0x2019 || c == 0x201D || c == 0xBB;
}

// Whether [s, e) of text, trimmed already, is a word to check.
bool isWord(const std::u16string& t, size_t s, size_t e) {
    if (e - s < 2) return false;
    if (!isLetter(t[s]) || !isLetter(t[e - 1])) return false;
    int capitals = 0;
    for (size_t i = s; i < e; i++) {
        const char16_t c = t[i];
        if (isUpper(c)) {
            if (++capitals >= 2) return false;               // NSString, HTTP, macOS
            if (i > s && isLower(t[i - 1])) return false;    // camelCase
        }
        if (isLetter(c)) continue;
        if (c == '\'' || c == 0x2019 || c == '-') continue;  // don't, well-known
        return false;   // digits, _, /, ., @, :, brackets, symbols
    }
    return true;
}

void addSpan(std::vector<Span>& out, size_t s, size_t e) {
    if (e > s) out.push_back({s, e});
}

// ---- code

TokenStyle styleOf(const Token& t) { return t.style; }

// Whether the string pieces read as a sentence: a letter, a space, a
// letter, and nothing a format, a regex or markup would hold.
bool readsAsSentence(const std::u16string& t, const std::vector<Span>& pieces) {
    bool sentence = false;
    for (const Span& p : pieces) {
        for (size_t i = p.first; i < p.second; i++) {
            const char16_t c = t[i];
            if (c == '{' || c == '}' || c == '%' || c == '<' || c == '>' || c == '=' ||
                c == '\\' || c == '|')
                return false;
            if (!sentence && isLetter(c)) {
                size_t j = i + 1;
                while (j < p.second && isLetter(t[j])) j++;
                size_t k = j;
                while (k < p.second && (t[k] == ' ' || t[k] == '\t')) k++;
                if (k > j && k < p.second && isLetter(t[k])) sentence = true;
            }
        }
    }
    return sentence;
}

std::vector<Span> codeProse(const std::u16string& t, size_t from,
                            const std::vector<Token>& tokens) {
    std::vector<Span> out;
    const size_t n = t.size();
    for (size_t k = 0; k < tokens.size(); k++) {
        const Token& tok = tokens[k];
        const size_t s = std::max(from, std::min(tok.start, n));
        const size_t e = std::min(n, tok.start + tok.length);
        if (styleOf(tok) == TokenStyle::Comment) {
            addSpan(out, s, e);
            continue;
        }
        if (!isStringStyle(styleOf(tok))) continue;
        // One string: its pieces, escapes apart, side by side.
        std::vector<Span> pieces;
        size_t end = tok.start + tok.length;
        size_t m = k;
        for (; m < tokens.size() && isStringStyle(tokens[m].style) &&
               (m == k || tokens[m].start == end);
             m++) {
            end = tokens[m].start + tokens[m].length;
            if (tokens[m].style == TokenStyle::String)
                pieces.push_back({std::min(tokens[m].start, n), std::min(end, n)});
        }
        k = m - 1;
        if (pieces.empty()) continue;
        // Past the prefix and the opening quotes (f", r', """).
        size_t& first = pieces.front().first;
        for (size_t i = first; i < pieces.front().second && i < first + 6; i++) {
            const char16_t c = t[i];
            if (c == '"' || c == '\'' || c == '`') {
                size_t j = i;
                while (j < pieces.front().second && t[j] == c) j++;
                first = j;
                break;
            }
            if (!isAsciiLetter(c)) break;
        }
        if (!readsAsSentence(t, pieces)) continue;
        for (const Span& p : pieces) addSpan(out, std::max(p.first, from), p.second);
    }
    return out;
}

// ---- LaTeX

// How many of the brace groups after a command name things rather than
// hold text: kAll for every group that follows.
constexpr int kAll = 1000;
int namingGroups(const std::string& name) {
    static const char* const all[] = {
        "label", "ref", "eqref", "pageref", "autoref", "nameref", "cref", "Cref", "vref",
        "cite", "citep", "citet", "citeauthor", "citeyear", "citealt", "citealp", "parencite",
        "textcite", "autocite", "footcite", "nocite", "includegraphics", "input", "include",
        "includeonly", "usepackage", "RequirePackage", "documentclass", "LoadClass",
        "bibliography", "bibliographystyle", "addbibresource", "newcommand", "renewcommand",
        "providecommand", "newenvironment", "renewenvironment", "newtheorem", "setlength",
        "addtolength", "setcounter", "addtocounter", "stepcounter", "newcounter", "newlength",
        "settowidth", "hypersetup", "definecolor", "color", "pagecolor", "hspace", "vspace",
        "graphicspath", "usetikzlibrary", "pagestyle", "thispagestyle", "pagenumbering",
        "bibitem", "lstinputlisting", "inputminted", "begin", "end", "url", "ProvidesPackage",
        "DeclareMathOperator", "geometry", "setmainfont", "setsansfont", "setmonofont",
        "newfontfamily", "fontsize", "linespread", "rule", "multirow", "mintinline",
        "lstset", "tikzset", "pgfplotsset", "captionsetup", "titleformat", "newacronym",
        "newglossaryentry", "gls", "Gls", "glspl", "acrshort", "acrlong", "acrfull", "ac",
    };
    static const char* const first[] = {"href", "textcolor", "colorbox", "raisebox", "hyperlink",
                                        "hypertarget"};
    for (const char* a : all)
        if (name == a) return kAll;
    for (const char* a : first)
        if (name == a) return 1;
    if (name == "multicolumn") return 2;
    return 0;
}

// The end (past the closer) of the group opening at i, on its line; 0 when
// it does not close there.
size_t groupEnd(const std::u16string& t, size_t i, char16_t open, char16_t close) {
    int depth = 0;
    for (size_t j = i; j < t.size(); j++) {
        const char16_t c = t[j];
        if (c == '\n' || c == '\r') return 0;
        if (c == '\\') { j++; continue; }
        if (c == open) depth++;
        else if (c == close && --depth == 0) return j + 1;
    }
    return 0;
}

std::vector<Span> latexProse(const std::u16string& t, size_t from,
                             const std::vector<Token>& tokens) {
    const size_t n = t.size();
    std::vector<Span> excluded;
    for (const Token& tok : tokens) {
        const size_t s = std::min(tok.start, n), e = std::min(n, tok.start + tok.length);
        if (tok.style == TokenStyle::Comment || tok.style == TokenStyle::Plain || e <= s) continue;
        excluded.push_back({s, e});
        const bool command = (tok.style == TokenStyle::Keyword ||
                              tok.style == TokenStyle::Preprocessor ||
                              tok.style == TokenStyle::Function) &&
                             t[s] == '\\';
        if (!command) continue;
        std::string name;
        for (size_t i = s + 1; i < e && t[i] < 128 && t[i] != '*'; i++) name += (char)t[i];
        int braces = namingGroups(name);
        // The groups after it: options in [...] always name things, and the
        // brace groups of a naming command do.
        size_t j = e;
        for (;;) {
            size_t p = j;
            while (p < n && (t[p] == ' ' || t[p] == '\t' || t[p] == '*')) p++;
            if (p >= n) break;
            if (t[p] == '[') {
                const size_t q = groupEnd(t, p, '[', ']');
                if (!q) break;
                excluded.push_back({p, q});
                j = q;
            } else if (t[p] == '{' && braces > 0) {
                const size_t q = groupEnd(t, p, '{', '}');
                if (!q) break;
                excluded.push_back({p, q});
                braces--;
                j = q;
            } else {
                break;
            }
        }
    }
    std::sort(excluded.begin(), excluded.end());
    std::vector<Span> out;
    size_t at = from;
    for (const Span& x : excluded) {
        if (x.second <= at) continue;
        if (x.first > at) addSpan(out, at, x.first);
        at = std::max(at, x.second);
    }
    addSpan(out, at, n);
    return out;
}

// ---- Markdown

// The prose of a Markdown line's text [s, e): what is left after code
// spans, math, links' addresses and labels, autolinks and HTML tags.
void markdownInline(const std::u16string& t, size_t s, size_t e, std::vector<Span>& out) {
    size_t run = s, i = s;
    auto skip = [&](size_t a, size_t b) {
        addSpan(out, run, a);
        run = i = b;
    };
    while (i < e) {
        const char16_t c = t[i];
        if (c == '\\' && i + 1 < e) {
            if (t[i + 1] == '(') {   // \( math \)
                size_t j = i + 2;
                while (j + 1 < e && !(t[j] == '\\' && t[j + 1] == ')')) j++;
                if (j + 1 < e) { skip(i, j + 2); continue; }
            }
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t k = i;
            while (k < e && t[k] == '`') k++;
            const size_t ticks = k - i;
            size_t j = k, close = 0;
            while (j < e) {
                if (t[j] != '`') { j++; continue; }
                size_t r = j;
                while (r < e && t[r] == '`') r++;
                if (r - j == ticks) { close = r; break; }
                j = r;
            }
            if (close) skip(i, close);
            else skip(i, k);
            continue;
        }
        if (c == '$') {
            if (i + 1 < e && t[i + 1] == '$') {
                size_t j = i + 2;
                while (j + 1 < e && !(t[j] == '$' && t[j + 1] == '$')) j++;
                skip(i, j + 1 < e ? j + 2 : e);
                continue;
            }
            if (i + 1 < e && !isSpace(t[i + 1])) {
                size_t j = i + 1, close = 0;
                for (; j < e; j++) {
                    if (t[j] == '\\') { j++; continue; }
                    if (t[j] == '$' && !isSpace(t[j - 1]) && !(j + 1 < e && isDigit(t[j + 1]))) {
                        close = j + 1;
                        break;
                    }
                }
                if (close) { skip(i, close); continue; }
            }
            i++;
            continue;
        }
        if (c == ']' && i + 1 < e && (t[i + 1] == '(' || t[i + 1] == '[')) {
            const char16_t open = t[i + 1], closer = open == '(' ? ')' : ']';
            int depth = 0;
            size_t j = i + 1, close = 0;
            for (; j < e; j++) {
                if (t[j] == open) depth++;
                else if (t[j] == closer && --depth == 0) { close = j + 1; break; }
            }
            if (close) { skip(i, close); continue; }
            i++;
            continue;
        }
        if (c == '<' && i + 1 < e &&
            (isAsciiLetter(t[i + 1]) || t[i + 1] == '/' || t[i + 1] == '!' || t[i + 1] == '?')) {
            size_t j = i + 1;
            while (j < e && t[j] != '>') j++;
            if (j < e) { skip(i, j + 1); continue; }
        }
        i++;
    }
    addSpan(out, run, e);
}

// UTF-8 bytes of one UTF-16 unit run: 1, 2 or 3, and 4 for a pair (counted
// on its first unit, 0 on its second).
size_t utf8Bytes(const std::u16string& t, size_t i) {
    const char16_t c = t[i];
    if (c < 0x80) return 1;
    if (c < 0x800) return 2;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < t.size() && t[i + 1] >= 0xDC00 &&
        t[i + 1] <= 0xDFFF)
        return 4;
    if (c >= 0xDC00 && c <= 0xDFFF && i > 0 && t[i - 1] >= 0xD800 && t[i - 1] <= 0xDBFF)
        return 0;
    return 3;
}

void appendUtf8(std::string& out, const std::u16string& t, size_t i) {
    uint32_t c = t[i];
    const size_t bytes = utf8Bytes(t, i);
    if (bytes == 0) return;
    if (bytes == 4) c = 0x10000 + ((c - 0xD800) << 10) + (t[i + 1] - 0xDC00);
    if (bytes == 1) {
        out += (char)c;
    } else if (bytes == 2) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (bytes == 3) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

std::vector<Span> markdownProse(const std::u16string& t, size_t from) {
    // The text as UTF-8, with each line's start in both.
    std::string u8;
    u8.reserve(t.size());
    std::vector<size_t> starts16{0}, starts8{0};
    for (size_t i = 0; i < t.size(); i++) {
        appendUtf8(u8, t, i);
        if (t[i] == '\n') {
            starts16.push_back(i + 1);
            starts8.push_back(u8.size());
        }
    }
    const std::vector<MdLine> lines = MarkdownParser::lines(u8);
    std::vector<Span> out;
    for (const MdLine& line : lines) {
        switch (line.kind) {
            case MdLine::Heading: case MdLine::Quote: case MdLine::ListItem:
            case MdLine::Text: case MdLine::TableHead: case MdLine::TableRow:
                break;
            default:
                continue;
        }
        // The line of the source it is, and its text there in UTF-16.
        const size_t ln = (size_t)(std::upper_bound(starts8.begin(), starts8.end(),
                                                    line.lineStart) - starts8.begin()) - 1;
        const size_t lineEnd16 = ln + 1 < starts16.size() ? starts16[ln + 1] : t.size();
        if (lineEnd16 <= from) continue;
        auto to16 = [&](size_t byte) {
            size_t b = starts8[ln], i = starts16[ln];
            while (b < byte && i < lineEnd16) b += utf8Bytes(t, i++);
            return i;
        };
        const size_t s = to16(std::max(line.start, starts8[ln]));
        const size_t e = to16(std::max(line.end, line.start));
        std::vector<Span> spans;
        markdownInline(t, s, e, spans);
        for (const Span& sp : spans)
            if (sp.second > from) addSpan(out, std::max(sp.first, from), sp.second);
    }
    return out;
}

}  // namespace

Kind kindForExtension(const std::string& ext, bool hasGrammar) {
    static const char* const markdown[] = {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn"};
    static const char* const latex[] = {"tex", "ltx", "latex", "sty", "cls"};
    for (const char* m : markdown)
        if (ext == m) return Kind::Markdown;
    for (const char* l : latex)
        if (ext == l) return Kind::Latex;
    if (ext == "txt" || ext == "text" || ext.empty()) return Kind::Plain;
    if (ext == "commit_editmsg" || ext == "gitcommit") return Kind::Commit;
    return hasGrammar ? Kind::Code : Kind::None;
}

bool checks(Mode mode, Kind kind) {
    switch (mode) {
        case Mode::Off: return false;
        case Mode::ProseOnly:
            return kind == Kind::Markdown || kind == Kind::Latex || kind == Kind::Commit;
        case Mode::On: return kind != Kind::None;
    }
    return false;
}

std::vector<Span> words(const std::u16string& t, const std::vector<Span>& prose) {
    std::vector<Span> out;
    for (const Span& p : prose) {
        size_t i = p.first;
        const size_t e = std::min(p.second, t.size());
        while (i < e) {
            while (i < e && endsChunk(t[i])) i++;
            size_t j = i;
            while (j < e && !endsChunk(t[j])) j++;
            size_t s = i, f = j;
            while (s < f && trimmedFront(t[s])) s++;
            while (f > s && trimmedBack(t[f - 1])) f--;
            if (isWord(t, s, f)) out.push_back({s, f});
            i = j;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Span> prose(Kind kind, const std::u16string& text, size_t from,
                        const std::vector<Token>& tokens) {
    if (from >= text.size()) return {};
    switch (kind) {
        case Kind::None: return {};
        case Kind::Markdown: return markdownProse(text, from);
        case Kind::Latex: return latexProse(text, from, tokens);
        case Kind::Code: return codeProse(text, from, tokens);
        case Kind::Plain: case Kind::Commit: return {{from, text.size()}};
    }
    return {};
}

std::vector<Span> checkable(Kind kind, const std::u16string& text, size_t from,
                            const std::vector<Token>& tokens) {
    return words(text, prose(kind, text, from, tokens));
}

}  // namespace Spell
