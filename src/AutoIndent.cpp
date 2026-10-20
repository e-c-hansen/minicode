// AutoIndent.cpp — see AutoIndent.h.
#include "AutoIndent.h"
#include "LineComments.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <vector>

namespace AutoIndent {

namespace {

using U = std::u16string;
constexpr size_t npos = std::u16string::npos;

bool isWs(char16_t c) { return c == u' ' || c == u'\t'; }
bool isIdent(char16_t c) {
    return c == u'_' || (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') ||
           (c >= u'A' && c <= u'Z') || c >= 0x80;
}
bool isOpener(char16_t c) { return c == u'(' || c == u'[' || c == u'{'; }
bool isCloser(char16_t c) { return c == u')' || c == u']' || c == u'}'; }
char16_t openerOf(char16_t c) {
    return c == u')' ? u'(' : c == u']' ? u'[' : c == u'}' ? u'{' : 0;
}

// --------------------------------------------------------------- file names

struct Name {
    std::string base;   // lowercase file name, no folders
    std::string ext;    // lowercase, no dot; "" when there is none
};

Name nameOf(const std::string& filename) {
    Name n;
    for (char c : filename) n.base += (char)std::tolower((unsigned char)c);
    size_t slash = n.base.find_last_of('/');
    if (slash != std::string::npos) n.base = n.base.substr(slash + 1);
    size_t dot = n.base.find_last_of('.');
    if (dot != std::string::npos) n.ext = n.base.substr(dot + 1);
    return n;
}

bool in(const std::string& s, std::initializer_list<const char*> list) {
    for (const char* x : list)
        if (s == x) return true;
    return false;
}

const std::unordered_set<std::string>& clikeExts() {
    static const std::unordered_set<std::string> s = {
        "c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx", "h++", "ipp",
        "inl", "tpp", "m", "mm", "java", "go", "rs", "js", "mjs", "cjs", "ts",
        "mts", "cts", "jsx", "tsx", "swift", "kt", "kts", "scala", "sc", "cs",
        "dart", "php", "proto", "zig", "glsl", "vert", "frag", "hlsl", "metal",
        "json", "jsonc", "json5", "jsonl", "ndjson", "geojson", "gradle",
        "groovy", "v", "sol", "css", "scss", "less", "d", "ino", "cu", "cuh",
        "vala", "hx", "fx", "qml", "nut", "jenkinsfile", "pde", "as", "wgsl",
    };
    return s;
}

// How a language's code is read: comments, strings and brackets.
struct Lexer {
    Lang lang = Lang::Plain;
    U lineComment;                 // "" for none
    bool hashAtWordStart = false;  // '#' starts a comment only at a word's start
    bool blockComments = false;    // /* ... */
    bool doubleQuotes = true, singleQuotes = true;
    bool backtick = false;         // `...` may span lines (JS templates, Go raw)
    bool rawBacktick = false;      // no escapes inside `...` (Go)
    bool triple = false;           // """...""" spans lines (Kotlin, Swift, Java, ...)
    bool rustLifetimes = false;    // 'a is not a character literal
    bool codeEscapes = false;      // a backslash escapes the next character (shell, TeX)
    bool yaml = false;             // quotes open a string only at a token's start
};

Lexer lexerFor(const std::string& filename, Lang lang) {
    Name n = nameOf(filename);
    Lexer lx;
    lx.lang = lang;
    switch (lang) {
        case Lang::Python:
            lx.lineComment = u"#";
            break;
        case Lang::CLike:
            if (!in(n.ext, {"css", "json", "geojson", "jsonl", "ndjson"})) lx.lineComment = u"//";
            lx.blockComments = true;
            lx.backtick = in(n.ext, {"js", "mjs", "cjs", "ts", "mts", "cts", "jsx", "tsx", "go"});
            lx.rawBacktick = n.ext == "go";
            lx.triple = in(n.ext, {"kt", "kts", "swift", "java", "scala", "sc", "groovy",
                                   "gradle", "cs", "dart"});
            lx.rustLifetimes = n.ext == "rs";
            break;
        case Lang::Yaml:
            lx.lineComment = u"#";
            lx.hashAtWordStart = true;
            lx.yaml = true;
            break;
        case Lang::Generic: {
            std::string marker = LineComments::markerFor(filename);
            for (char c : marker) lx.lineComment += (char16_t)(unsigned char)c;
            lx.hashAtWordStart = marker == "#";
            lx.codeEscapes = in(n.ext, {"sh", "bash", "zsh", "fish", "tex", "sty", "cls",
                                        "zshrc", "bashrc", "zshenv", "zprofile", "profile",
                                        "bash_profile"});
            if (in(n.ext, {"lisp", "clj", "el", "scm", "hs", "elm", "tex", "sty", "cls", "vim"}))
                lx.singleQuotes = false;   // 'quote, x', `` ``...'' in TeX
            if (in(n.ext, {"tex", "sty", "cls", "vim"})) lx.doubleQuotes = false;
            if (n.ext == "vim") lx.lineComment.clear();
            break;
        }
        case Lang::Plain:
            break;
    }
    return lx;
}

// ------------------------------------------------------------- text helpers

size_t lineStartOf(const U& t, size_t pos) {
    pos = std::min(pos, t.size());
    while (pos > 0 && t[pos - 1] != u'\n') --pos;
    return pos;
}

// The end of the line holding `pos`, before its "\n" or "\r\n".
size_t contentEnd(const U& t, size_t pos) {
    size_t e = std::min(pos, t.size());
    while (e < t.size() && t[e] != u'\n') ++e;
    if (e > pos && t[e - 1] == u'\r') --e;
    return e;
}

// The position just after the line holding `pos` (its newline included).
size_t nextLine(const U& t, size_t pos) {
    size_t e = std::min(pos, t.size());
    while (e < t.size() && t[e] != u'\n') ++e;
    return e < t.size() ? e + 1 : e;
}

U leadingWs(const U& t, size_t ls) {
    size_t e = ls;
    while (e < t.size() && isWs(t[e])) ++e;
    return t.substr(ls, e - ls);
}

// Columns from `from` to `to`, a tab advancing to the next multiple of tw and
// a surrogate pair counting once.
int columns(const U& t, size_t from, size_t to, int tw) {
    int col = 0;
    for (size_t i = from; i < to && i < t.size(); ++i) {
        char16_t c = t[i];
        if (c == u'\t') col += tw - col % tw;
        else if (c >= 0xDC00 && c <= 0xDFFF) continue;   // second half of a pair
        else ++col;
    }
    return col;
}
int columns(const U& s, int tw) { return columns(s, 0, s.size(), tw); }

U spaces(int n) { return U((size_t)std::max(0, n), u' '); }

U makeIndent(int cols, const Unit& u) {
    if (cols <= 0) return U();
    if (!u.tabs) return spaces(cols);
    int tw = std::max(1, u.width);
    return U((size_t)(cols / tw), u'\t') + spaces(cols % tw);
}

U minusUnit(const U& ws, const Unit& u) {
    if (ws.empty()) return ws;
    if (ws.back() == u'\t') return ws.substr(0, ws.size() - 1);
    int tw = std::max(1, u.width);
    return makeIndent(columns(ws, tw) - tw, u);
}

U lineEnding(const U& t) {
    size_t nl = t.find(u'\n');
    if (nl != npos && nl > 0 && t[nl - 1] == u'\r') return u"\r\n";
    return u"\n";
}

U identAt(const U& t, size_t p) {
    size_t e = p;
    while (e < t.size() && isIdent(t[e])) ++e;
    return t.substr(p, e - p);
}

bool startsWith(const U& t, size_t p, const U& s) {
    return p + s.size() <= t.size() && t.compare(p, s.size(), s) == 0;
}

// ----------------------------------------------------------------- scanning

struct Bracket {
    char16_t ch;
    size_t pos;         // where it is
    size_t lineStart;   // the start of its line
};

enum class Where { Code, Comment, LineString, BlockString, BlockComment };

// What the code before the caret looks like.
struct Ctx {
    Where where = Where::Code;
    std::vector<Bracket> stack;     // brackets still open, innermost last
    size_t lineStart = 0;           // the caret's line
    size_t stmtStart = 0;           // Python: the first line of the caret's statement
    bool contLine = false;          // Python: the caret's line continues one ending in '\'
    // The last character of code (not space, comment or inside a string,
    // though a closing quote counts) before the caret: on the caret's
    // statement for Python, on its line for the rest.
    char16_t lastSig = 0;
    size_t lastSigPos = npos;
    // When the caret's line closes brackets opened on earlier lines, the
    // start of the line holding the outermost of them.
    size_t closedBase = npos;
};

bool lineCommentAt(const U& t, size_t i, size_t lineStart, const Lexer& lx) {
    if (lx.lineComment.empty() || !startsWith(t, i, lx.lineComment)) return false;
    if (lx.hashAtWordStart && i > lineStart && !isWs(t[i - 1])) return false;
    return true;
}

bool rustLifetime(const U& t, size_t i) {
    if (i + 1 >= t.size()) return false;
    char16_t n = t[i + 1];
    bool start = n == u'_' || (n >= u'a' && n <= u'z') || (n >= u'A' && n <= u'Z');
    return start && !(i + 2 < t.size() && t[i + 2] == u'\'');
}

// Reads the code from the top of the file up to `end`. `stmts`, if given,
// gets the start of every Python statement's first line.
Ctx scan(const U& t, size_t end, const Lexer& lx, std::vector<size_t>* stmts) {
    Ctx c;
    end = std::min(end, t.size());
    const bool py = lx.lang == Lang::Python;
    enum { CODE, STR, BLOCK } mode = CODE;
    char16_t q = 0;
    bool multi = false, tripleQ = false, escapes = true;
    if (stmts) stmts->push_back(0);

    auto sig = [&](char16_t ch, size_t pos) {
        c.lastSig = ch;
        c.lastSigPos = pos;
    };
    auto physical = [&](size_t next) {
        c.lineStart = next;
        c.closedBase = npos;
        if (!py) {
            c.lastSig = 0;
            c.lastSigPos = npos;
        }
    };
    auto codeNewline = [&](size_t next) {
        physical(next);
        if (!py) return;
        if (c.lastSig == u'\\') {   // a backslash continues the statement
            c.lastSig = 0;
            c.lastSigPos = npos;
            c.contLine = true;
            return;
        }
        c.contLine = false;
        if (!c.stack.empty()) return;
        c.stmtStart = next;
        c.lastSig = 0;
        c.lastSigPos = npos;
        if (stmts) stmts->push_back(next);
    };
    auto openString = [&](char16_t quote, size_t& i, bool triple, bool spans, bool esc) {
        mode = STR;
        q = quote;
        tripleQ = triple;
        multi = spans;
        escapes = esc;
        i += triple ? 3 : 1;
    };

    size_t i = 0;
    while (i < end) {
        char16_t ch = t[i];
        if (mode == STR) {
            if (ch == u'\\' && escapes) {
                if (i + 1 < t.size() && t[i + 1] == u'\n') {
                    physical(i + 2);
                    i += 2;
                } else if (i + 2 < t.size() && t[i + 1] == u'\r' && t[i + 2] == u'\n') {
                    physical(i + 3);
                    i += 3;
                } else {
                    i += 2;
                }
                continue;
            }
            if (tripleQ) {
                if (ch == q && i + 2 < end && t[i + 1] == q && t[i + 2] == q) {
                    mode = CODE;
                    sig(q, i + 2);
                    i += 3;
                    continue;
                }
                if (ch == u'\n') physical(i + 1);
                ++i;
                continue;
            }
            if (ch == q) {
                if (lx.yaml && q == u'\'' && i + 1 < end && t[i + 1] == u'\'') {
                    i += 2;   // '' is a quote inside a YAML single-quoted string
                    continue;
                }
                mode = CODE;
                sig(q, i);
                ++i;
                continue;
            }
            if (ch != u'\n') {
                ++i;
                continue;
            }
            if (multi) {
                physical(i + 1);
                ++i;
                continue;
            }
            mode = CODE;   // an unterminated string ends with its line
        } else if (mode == BLOCK) {
            if (ch == u'*' && i + 1 < end && t[i + 1] == u'/') {
                mode = CODE;
                i += 2;
                continue;
            }
            if (ch == u'\n') physical(i + 1);
            ++i;
            continue;
        }

        // Code.
        if (ch == u'\n') {
            codeNewline(i + 1);
            ++i;
            continue;
        }
        if (ch == u' ' || ch == u'\t' || ch == u'\r') {
            ++i;
            continue;
        }
        if (py ? ch == u'#' : lineCommentAt(t, i, c.lineStart, lx)) {
            size_t nl = t.find(u'\n', i);
            if (nl == npos) nl = t.size();
            if (nl >= end) {
                c.where = Where::Comment;
                return c;
            }
            i = nl;
            continue;
        }
        if (lx.blockComments && ch == u'/' && i + 1 < end && t[i + 1] == u'*') {
            mode = BLOCK;
            i += 2;
            continue;
        }
        if (py && (ch == u'"' || ch == u'\'')) {
            bool triple = i + 2 < end && t[i + 1] == ch && t[i + 2] == ch;
            openString(ch, i, triple, triple, true);
            continue;
        }
        if (!py && (ch == u'"' || ch == u'\'' || ch == u'`')) {
            bool yamlOk = true;
            if (lx.yaml && i > c.lineStart) {
                char16_t p = t[i - 1];
                yamlOk = isWs(p) || p == u':' || p == u'[' || p == u'{' || p == u',' ||
                         p == u'-' || p == u'?';
            }
            if (ch == u'"' && lx.doubleQuotes && yamlOk) {
                bool triple = lx.triple && i + 2 < end && t[i + 1] == u'"' && t[i + 2] == u'"';
                openString(ch, i, triple, triple, true);
                continue;
            }
            if (ch == u'\'' && lx.singleQuotes && yamlOk &&
                !(lx.rustLifetimes && rustLifetime(t, i))) {
                openString(ch, i, false, false, !lx.yaml);
                continue;
            }
            if (ch == u'`' && lx.backtick) {
                openString(ch, i, false, true, !lx.rawBacktick);
                continue;
            }
            if (ch == u'`' && lx.lang == Lang::Generic && lx.singleQuotes) {
                openString(ch, i, false, false, true);   // shell command substitution
                continue;
            }
        }
        if (isOpener(ch)) {
            c.stack.push_back({ch, i, c.lineStart});
            sig(ch, i);
            ++i;
            continue;
        }
        if (isCloser(ch)) {
            char16_t open = openerOf(ch);
            for (size_t k = c.stack.size(); k-- > 0;) {
                if (c.stack[k].ch != open) continue;
                if (c.stack[k].lineStart < c.lineStart) c.closedBase = c.stack[k].lineStart;
                c.stack.resize(k);
                break;
            }
            sig(ch, i);
            ++i;
            continue;
        }
        if (ch == u'\\' && !py && lx.codeEscapes) {
            sig(ch, i);
            // A backslash before the line's end continues it (shell); before
            // anything else it takes that character with it.
            if (i + 1 < end && t[i + 1] != u'\n' && t[i + 1] != u'\r') i += 2;
            else ++i;
            continue;
        }
        sig(ch, i);
        ++i;
    }
    if (mode == STR) c.where = multi ? Where::BlockString : Where::LineString;
    else if (mode == BLOCK) c.where = Where::BlockComment;
    return c;
}

// Python statements after which the block ends.
bool dedentsAfter(const U& t, size_t stmtStart) {
    size_t p = stmtStart;
    while (p < t.size() && isWs(t[p])) ++p;
    U w = identAt(t, p);
    return w == u"return" || w == u"pass" || w == u"break" || w == u"continue" ||
           w == u"raise";
}

// YAML: a line that opens a nested block, "key:" or a block scalar "key: |".
bool yamlOpens(const U& t, size_t ls, const Ctx& c) {
    if (c.lastSigPos == npos || c.lastSigPos < ls) return false;
    if (c.lastSig == u':') return true;
    size_t p = c.lastSigPos;
    int n = 0;
    while (p > ls && n < 2 && (t[p] == u'+' || t[p] == u'-' || (t[p] >= u'0' && t[p] <= u'9'))) {
        --p;
        ++n;
    }
    if (t[p] != u'|' && t[p] != u'>') return false;
    if (p == ls) return false;
    size_t qp = p;
    while (qp > ls && isWs(t[qp - 1])) --qp;
    if (qp == ls) return false;
    return t[qp - 1] == u':' || t[qp - 1] == u'-';
}

// Is the bracket at `pos` the last code on its line (a comment may follow)?
bool bracketEndsLine(const U& t, size_t pos, const Lexer& lx) {
    size_t le = contentEnd(t, pos);
    size_t p = pos + 1;
    while (p < le && isWs(t[p])) ++p;
    if (p >= le) return true;
    if (lx.lang == Lang::Python) return t[p] == u'#';
    if (!lx.lineComment.empty() && startsWith(t, p, lx.lineComment)) return true;
    return lx.blockComments && startsWith(t, p, u"/*");
}

Edit replaceWith(size_t start, size_t length, U replacement, size_t caret) {
    Edit e;
    e.changed = true;
    e.replaceStart = start;
    e.replaceLength = length;
    e.replacement = std::move(replacement);
    e.selStart = e.selEnd = caret;
    return e;
}

void order(const U& t, size_t& a, size_t& b) {
    if (a > b) std::swap(a, b);
    a = std::min(a, t.size());
    b = std::min(b, t.size());
}

// The lines [selStart, selEnd) touches: the first line's start and the last
// line's start. A selection ending at the very start of a line leaves it out.
void touchedLines(const U& t, size_t a, size_t b, size_t& first, size_t& last) {
    first = lineStartOf(t, a);
    size_t bb = b;
    if (b > a && bb > 0 && t[bb - 1] == u'\n') --bb;
    last = lineStartOf(t, std::max(bb, a));
}

bool blankLine(const U& t, size_t ls) {
    size_t e = contentEnd(t, ls);
    for (size_t p = ls; p < e; ++p)
        if (!isWs(t[p])) return false;
    return true;
}

}  // namespace

// ------------------------------------------------------------------ public

Lang languageFor(const std::string& filename) {
    Name n = nameOf(filename);
    if (in(n.ext, {"py", "pyw", "pyi"}) || in(n.base, {"sconstruct", "sconscript"}))
        return Lang::Python;
    if (in(n.ext, {"yml", "yaml"})) return Lang::Yaml;
    if (clikeExts().count(n.ext) || n.base == "jenkinsfile") return Lang::CLike;
    if (in(n.ext, {"md", "markdown", "mdown", "txt", "text", "tsv", "csv", "log", "rst"}))
        return Lang::Plain;
    if (!LineComments::markerFor(filename).empty()) return Lang::Generic;
    return Lang::Plain;
}

std::u16string Unit::text() const {
    return tabs ? U(u"\t") : spaces(std::max(1, width));
}

Unit detect(const std::u16string& t, const std::string& filename) {
    const Lang lang = languageFor(filename);
    const Name n = nameOf(filename);
    Unit u;
    u.width = lang == Lang::Yaml ? 2 : 4;
    const bool tabDefault = n.ext == "go" || n.ext == "mk" ||
                            in(n.base, {"makefile", "gnumakefile"});
    // Only steps after a line that opens a block count where the language
    // has blocks, so arguments lined up under "foo(" do not set the width.
    const bool anyStep = lang == Lang::Plain || lang == Lang::Generic;
    int tabLines = 0, spaceLines = 0;
    int steps[9] = {0};
    int prev = -1;   // the last non-blank line's indentation, when it is all spaces
    char16_t prevLast = 0;   // and its last character
    size_t i = 0;
    for (int lines = 0; i < t.size() && lines < 20000; ++lines) {
        size_t ls = i, j = i;
        int sp = 0;
        bool tab = false;
        while (j < t.size() && isWs(t[j])) {
            if (t[j] == u'\t') tab = true;
            else if (!tab) ++sp;
            ++j;
        }
        size_t e = j;
        while (e < t.size() && t[e] != u'\n') ++e;
        bool blank = j == e || (t[j] == u'\r' && j + 1 == e);
        if (!blank) {
            if (j > ls) (t[ls] == u'\t' ? tabLines : spaceLines)++;
            bool opens = anyStep || prevLast == u':' || isOpener(prevLast);
            if (!tab) {
                if (prev >= 0 && sp > prev && sp - prev <= 8 && opens) steps[sp - prev]++;
                prev = sp;
            } else {
                prev = -1;
            }
            size_t last = e;
            while (last > j && (isWs(t[last - 1]) || t[last - 1] == u'\r')) --last;
            prevLast = t[last - 1];
        }
        i = e + 1;
    }
    if (tabLines > spaceLines) {
        u.tabs = true;
        u.width = 4;
        u.fromFile = true;
        return u;
    }
    if (spaceLines > 0) {
        u.fromFile = true;
        int best = 0, count = 0;
        for (int d : {4, 2, 8, 3})
            if (steps[d] > count) {
                best = d;
                count = steps[d];
            }
        if (best) u.width = best;
        return u;
    }
    if (tabDefault) u.tabs = true;
    return u;
}

Edit newline(const std::u16string& t, size_t a, size_t b, const std::string& filename) {
    return newline(t, a, b, filename, detect(t, filename));
}

Edit newline(const std::u16string& t, size_t a, size_t b, const std::string& filename,
             const Unit& u) {
    order(t, a, b);
    const Lang lang = languageFor(filename);
    const U nl = lineEnding(t);
    const size_t ls = lineStartOf(t, a);
    const int tw = std::max(1, u.width);

    // Keep the line's indentation, up to the caret, and touch nothing else.
    auto keep = [&]() {
        size_t e = ls;
        while (e < a && isWs(t[e])) ++e;
        U rep = nl + t.substr(ls, e - ls);
        size_t caret = a + rep.size();
        return replaceWith(a, b - a, std::move(rep), caret);
    };
    if (lang == Lang::Plain) return keep();

    const Lexer lx = lexerFor(filename, lang);
    const Ctx c = scan(t, a, lx, nullptr);
    if (c.where == Where::LineString || c.where == Where::BlockString) return keep();
    if (c.where == Where::BlockComment) {
        // Continue a /* ... */ comment: " * " under the opening line's star.
        U ws = leadingWs(t, ls);
        size_t p = ls + ws.size();
        size_t m = b;
        size_t le = contentEnd(t, b);
        while (m < le && isWs(t[m])) ++m;
        if (startsWith(t, p, u"/*")) {
            if (startsWith(t, m, u"*/")) {   // /**|*/ opens the comment
                U head = nl + ws + u" * ";
                return replaceWith(a, m - a, head + nl + ws + u" ", a + head.size());
            }
            U rep = nl + ws + u" * ";
            return replaceWith(a, b - a, rep, a + rep.size());
        }
        if (p < t.size() && t[p] == u'*' && !startsWith(t, p, u"*/")) {
            U rep = nl + ws + u"* ";
            return replaceWith(a, b - a, rep, a + rep.size());
        }
        return keep();
    }

    // The moved text loses its leading spaces, and the line left behind its
    // trailing ones (all of them, on a line of nothing but spaces).
    const size_t le = contentEnd(t, b);
    size_t m = b;
    while (m < le && isWs(t[m])) ++m;
    size_t k = a;
    while (k > ls && isWs(t[k - 1])) --k;
    const bool movedEmpty = m >= le;
    const char16_t first = movedEmpty ? 0 : t[m];
    const Bracket* B = c.stack.empty() ? nullptr : &c.stack.back();

    auto base = [&]() {
        return leadingWs(t, c.closedBase != npos ? c.closedBase : ls);
    };

    U ind;
    if (B && isCloser(first) && openerOf(first) == B->ch) {
        U openWs = leadingWs(t, B->lineStart);
        if (c.lastSigPos == B->pos) {
            // Between a pair: the caret on an indented line of its own, the
            // closer on the next at the opening line's indentation.
            U head = nl + openWs + u.text();
            return replaceWith(k, m - k, head + nl + openWs, k + head.size());
        }
        ind = openWs;
    } else if (B) {
        U openWs = leadingWs(t, B->lineStart);
        if (lang == Lang::Python) {
            size_t limit = B->lineStart == ls ? a : contentEnd(t, B->pos);
            size_t p = B->pos + 1;
            while (p < limit && isWs(t[p])) ++p;
            if (p < limit && t[p] != u'#') {
                // Text follows the bracket: line up with it.
                int col = columns(t, B->lineStart, p, tw);
                ind = openWs + spaces(col - columns(openWs, tw));
            } else {
                ind = openWs + u.text();   // a hanging indent
            }
        } else if (c.lastSigPos == B->pos) {
            ind = openWs + u.text();
        } else {
            ind = base();
        }
    } else if (lang == Lang::Python) {
        U stmtWs = leadingWs(t, c.stmtStart);
        if (c.lastSig == u'\\') ind = c.contLine ? leadingWs(t, ls) : stmtWs + u.text();
        else if (c.lastSig == u':') ind = stmtWs + u.text();
        else if (movedEmpty && dedentsAfter(t, c.stmtStart)) ind = minusUnit(stmtWs, u);
        else ind = stmtWs;
    } else if (lang == Lang::Yaml) {
        U ws = leadingWs(t, ls);
        if (yamlOpens(t, ls, c)) {
            size_t p = ls + ws.size();
            if (p + 1 < t.size() && t[p] == u'-' && isWs(t[p + 1])) {
                // "- key:": the mapping's keys line up after the dash.
                size_t q = p + 1;
                while (q < t.size() && isWs(t[q])) ++q;
                ws += spaces(columns(t, p, q, tw));
            }
            ind = ws + u.text();
        } else {
            ind = ws;
        }
    } else {
        ind = base();
    }
    U rep = nl + ind;
    size_t caret = k + rep.size();
    return replaceWith(k, m - k, std::move(rep), caret);
}

Edit typed(const std::u16string& t, size_t caret, const std::string& filename) {
    return typed(t, caret, filename, detect(t, filename));
}

Edit typed(const std::u16string& t, size_t caret, const std::string& filename,
           const Unit& u) {
    if (caret == 0 || caret > t.size()) return {};
    const Lang lang = languageFor(filename);
    if (lang == Lang::Plain || lang == Lang::Yaml) return {};
    const Lexer lx = lexerFor(filename, lang);
    const char16_t ch = t[caret - 1];
    const int tw = std::max(1, u.width);

    if (isCloser(ch)) {
        // Only a closer alone on its line.
        size_t ls = lineStartOf(t, caret - 1);
        for (size_t p = ls; p < caret - 1; ++p)
            if (!isWs(t[p])) return {};
        size_t le = contentEnd(t, caret);
        for (size_t p = caret; p < le; ++p)
            if (!isWs(t[p])) return {};
        Ctx c = scan(t, caret - 1, lx, nullptr);
        if (c.where != Where::Code || c.stack.empty()) return {};
        const Bracket& B = c.stack.back();
        if (B.ch != openerOf(ch)) return {};
        // Arguments lined up after "foo(" keep their closer where it is.
        if (B.ch != u'{' && !bracketEndsLine(t, B.pos, lx)) return {};
        U target = leadingWs(t, B.lineStart);
        size_t wsLen = caret - 1 - ls;
        if (t.compare(ls, wsLen, target) == 0) return {};
        return replaceWith(ls, wsLen, target, ls + target.size() + 1);
    }

    if (lang == Lang::Python && ch == u':') {
        std::vector<size_t> stmts;
        Ctx c = scan(t, caret, lx, &stmts);
        if (c.where != Where::Code || !c.stack.empty() || c.lastSig != u':' ||
            c.lastSigPos != caret - 1)
            return {};
        const size_t ls = c.lineStart;
        if (c.stmtStart != ls) return {};
        size_t le = contentEnd(t, caret), p = caret;
        while (p < le && isWs(t[p])) ++p;
        if (p < le && t[p] != u'#') return {};
        U ws = leadingWs(t, ls);
        int cur = columns(ws, tw);
        if (cur == 0) return {};
        U word = identAt(t, ls + ws.size());
        std::vector<U> openers;
        if (word == u"else") openers = {u"if", u"elif", u"for", u"while", u"try", u"except"};
        else if (word == u"elif") openers = {u"if", u"elif"};
        else if (word == u"except") openers = {u"try", u"except"};
        else if (word == u"finally") openers = {u"try", u"except", u"else"};
        else if (word == u"case") openers = {u"case"};
        else return {};
        if (word == u"else" || word == u"finally") {
            for (size_t q = ls + ws.size() + word.size(); q < caret - 1; ++q)
                if (!isWs(t[q])) return {};
        } else {
            size_t after = ls + ws.size() + word.size();
            if (after < caret - 1 && !isWs(t[after]) && t[after] != u'(' && t[after] != u'*')
                return {};
        }
        for (auto it = stmts.rbegin(); it != stmts.rend(); ++it) {
            size_t s = *it;
            if (s >= ls) continue;
            U sws = leadingWs(t, s);
            size_t sp = s + sws.size();
            if (sp >= contentEnd(t, s) || t[sp] == u'#') continue;   // blank or a comment
            if (columns(sws, tw) >= cur) continue;
            U w = identAt(t, sp);
            if (w == u"async") {
                size_t q = sp + w.size();
                while (q < t.size() && isWs(t[q])) ++q;
                w = identAt(t, q);
            }
            if (std::find(openers.begin(), openers.end(), w) == openers.end()) return {};
            return replaceWith(ls, ws.size(), sws, caret - ws.size() + sws.size());
        }
        return {};
    }
    return {};
}

Edit tab(const std::u16string& t, size_t a, size_t b, const std::string& filename) {
    return tab(t, a, b, filename, detect(t, filename));
}

Edit tab(const std::u16string& t, size_t a, size_t b, const std::string& filename,
         const Unit& u) {
    order(t, a, b);
    size_t first, last;
    touchedLines(t, a, b, first, last);
    if (last > first) {
        // Indent every non-blank line the selection touches.
        const U unit = u.text();
        U out;
        size_t shiftA = 0, shiftB = 0;
        size_t s = first, end = contentEnd(t, last);
        while (true) {
            if (!blankLine(t, s)) {
                out += unit;
                if (s < a) shiftA += unit.size();
                if (s < b) shiftB += unit.size();
            }
            if (s == last) {
                out += t.substr(s, end - s);
                break;
            }
            size_t next = nextLine(t, s);
            out += t.substr(s, next - s);
            s = next;
        }
        Edit e = replaceWith(first, end - first, std::move(out), 0);
        e.selStart = a + shiftA;
        e.selEnd = b + shiftB;
        return e;
    }
    const Name n = nameOf(filename);
    if (u.tabs || in(n.ext, {"tsv", "tab"})) return {};
    if (languageFor(filename) == Lang::Plain && !u.fromFile) return {};
    const int tw = std::max(1, u.width);
    int col = columns(t, lineStartOf(t, a), a, tw);
    U rep = spaces(tw - col % tw);
    size_t caret = a + rep.size();
    return replaceWith(a, b - a, std::move(rep), caret);
}

Edit backtab(const std::u16string& t, size_t a, size_t b, const std::string& filename) {
    return backtab(t, a, b, filename, detect(t, filename));
}

Edit backtab(const std::u16string& t, size_t a, size_t b, const std::string&,
             const Unit& u) {
    order(t, a, b);
    size_t first, last;
    touchedLines(t, a, b, first, last);
    const int tw = std::max(1, u.width);
    U out;
    size_t removedTotal = 0;
    auto mapped = [](size_t x, size_t s, size_t r) {
        if (x >= s + r) return r;
        return x > s ? x - s : 0;
    };
    size_t shiftA = 0, shiftB = 0;
    size_t s = first, end = contentEnd(t, last);
    while (true) {
        size_t r = 0;
        if (s < t.size() && t[s] == u'\t') {
            r = 1;
        } else {
            size_t sp = 0;
            while (s + sp < t.size() && t[s + sp] == u' ') ++sp;
            if (sp > 0) r = (sp - 1) % (size_t)tw + 1;
        }
        shiftA += mapped(a, s, r);
        shiftB += mapped(b, s, r);
        removedTotal += r;
        size_t stop = s == last ? end : nextLine(t, s);
        out += t.substr(s + r, stop - s - r);
        if (s == last) break;
        s = stop;
    }
    if (removedTotal == 0) return {};
    Edit e = replaceWith(first, end - first, std::move(out), 0);
    e.selStart = a - shiftA;
    e.selEnd = b - shiftB;
    return e;
}

Edit backspace(const std::u16string& t, size_t a, size_t b, const std::string& filename) {
    return backspace(t, a, b, filename, detect(t, filename));
}

Edit backspace(const std::u16string& t, size_t a, size_t b, const std::string& filename,
               const Unit& u) {
    if (a != b || a > t.size() || u.tabs) return {};
    if (languageFor(filename) == Lang::Plain && !u.fromFile) return {};
    size_t ls = lineStartOf(t, a);
    if (a == ls) return {};
    for (size_t p = ls; p < a; ++p)
        if (t[p] != u' ') return {};
    size_t col = a - ls;
    size_t r = (col - 1) % (size_t)std::max(1, u.width) + 1;
    if (r <= 1) return {};
    return replaceWith(a - r, r, U(), a - r);
}

}  // namespace AutoIndent
