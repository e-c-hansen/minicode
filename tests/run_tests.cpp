// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "TermLinks.h"
#include "MarkdownParser.h"
#include "TerminalStream.h"
#include "TerminalScreen.h"
#include "Settings.h"
#include "LineComments.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include "Json.h"
#include "LspClient.h"
#include "FolderSearch.h"
#include "LegacyHighlighter.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>   // mkfifo, for the folder search test
#endif

// --------------------------------------------------------------- tiny harness
static int g_pass = 0, g_fail = 0;
static const char *g_group = "";

#define GROUP(name) g_group = name
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            std::printf("  FAIL [%s] %s:%d  %s\n", g_group, __FILE__,          \
                        __LINE__, #cond);                                      \
        }                                                                      \
    } while (0)

// --------------------------------------------------------- syntax highlighter
namespace {

std::string slice(const std::string &s, const Token &t) {
    return s.substr(t.start, t.length);
}

// Is there a token whose exact text is `word` with style `st`?
bool hasToken(const std::string &text, const std::string &ext,
              const std::string &word, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (slice(text, t) == word && t.style == st) return true;
    return false;
}

// Is there any token with style `st`?
bool hasStyle(const std::string &text, const std::string &ext, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (t.style == st) return true;
    return false;
}

void testSyntax() {
    GROUP("syntax:supports");
    CHECK(SyntaxHighlighter::supports("py"));
    CHECK(SyntaxHighlighter::supports("cpp"));
    CHECK(SyntaxHighlighter::supports("js"));
    CHECK(!SyntaxHighlighter::supports("xyz"));
    CHECK(!SyntaxHighlighter::supports(""));

    GROUP("syntax:python");
    std::string py = "def foo(x):\n    return 1  # note\n";
    CHECK(hasToken(py, "py", "def", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "return", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "1", TokenStyle::Number));
    CHECK(hasToken(py, "py", "# note", TokenStyle::Comment));
    CHECK(hasToken(py, "py", "foo", TokenStyle::Function));  // foo(
    CHECK(hasToken("s = \"hi\"", "py", "\"hi\"", TokenStyle::String));
    // A '#' comment must not be mistaken for anything else.
    CHECK(!hasStyle("# just a comment", "py", TokenStyle::Keyword));

    GROUP("syntax:cpp");
    std::string cpp = "int main() {\n  // c\n  return 0;\n}\n";
    CHECK(hasToken(cpp, "cpp", "int", TokenStyle::Type));
    CHECK(hasToken(cpp, "cpp", "return", TokenStyle::Keyword));
    CHECK(hasToken(cpp, "cpp", "0", TokenStyle::Number));
    CHECK(hasToken(cpp, "cpp", "// c", TokenStyle::Comment));
    CHECK(hasToken(cpp, "cpp", "main", TokenStyle::Function));
    CHECK(hasStyle("#include <cstdio>", "cpp", TokenStyle::Preprocessor));
    CHECK(hasToken("/* block */ x", "cpp", "/* block */", TokenStyle::Comment));

    GROUP("syntax:strings-escapes");
    // An escaped quote should not end the string early.
    CHECK(hasToken("\"a\\\"b\"", "cpp", "\"a\\\"b\"", TokenStyle::String));

    GROUP("syntax:plain");
    // Plain text / unknown extension yields no styled tokens.
    CHECK(SyntaxHighlighter::highlight("just words here", "txt").empty());

    GROUP("syntax:tex");
    for (const char *e : {"tex", "ltx", "latex", "sty", "cls", "bib"})
        CHECK(SyntaxHighlighter::supports(e));
    std::string tex = "\\documentclass{article}\n\\usepackage{amsmath}\n"
                      "\\section{Intro} Some \\emph{words}.\n";
    CHECK(hasToken(tex, "tex", "\\documentclass", TokenStyle::Preprocessor));
    CHECK(hasToken(tex, "tex", "\\usepackage", TokenStyle::Preprocessor));
    CHECK(hasToken(tex, "tex", "\\section", TokenStyle::Function));
    CHECK(hasToken(tex, "tex", "\\emph", TokenStyle::Keyword));
    CHECK(!hasStyle("Just prose, 42 of it.", "tex", TokenStyle::Number));
    // \% is a character, not a comment; \\% is a line break, then one.
    std::string pct = "50\\% off % real\n";
    CHECK(hasToken(pct, "tex", "\\%", TokenStyle::Keyword));
    CHECK(hasToken(pct, "tex", "% real", TokenStyle::Comment));
    CHECK(hasToken("a\\\\% c", "tex", "\\\\", TokenStyle::Keyword));
    CHECK(hasToken("a\\\\% c", "tex", "% c", TokenStyle::Comment));
    // Math of every kind, with commands inside still commands.
    CHECK(hasToken("see $x^2$ here", "tex", "$x^2$", TokenStyle::Number));
    CHECK(hasToken("$$y$$", "tex", "$$y$$", TokenStyle::Number));
    CHECK(hasToken("\\(a\\) b", "tex", "\\(a\\)", TokenStyle::Number));
    CHECK(hasToken("\\[ b \\]", "tex", "\\[ b \\]", TokenStyle::Number));
    CHECK(hasToken("$\\alpha + 1$", "tex", "\\alpha", TokenStyle::Keyword));
    CHECK(hasToken("$\\alpha + 1$", "tex", " + 1$", TokenStyle::Number));
    CHECK(hasToken("$a % c\nb$", "tex", "% c", TokenStyle::Comment));
    CHECK(hasToken("$a % c\nb$", "tex", "b$", TokenStyle::Number));
    CHECK(!hasStyle("\\$5 and \\$6", "tex", TokenStyle::Number));
    // Display math across lines is one token, cut only by commands.
    CHECK(hasToken("\\[\na\n\\]\nb", "tex", "\\[\na\n\\]", TokenStyle::Number));
    CHECK(hasToken("\\[\n\\alpha\n\\]", "tex", "\\[\n", TokenStyle::Number));
    CHECK(hasToken("\\[\n\\alpha\n\\]", "tex", "\n\\]", TokenStyle::Number));
    // An unclosed $ stops at the paragraph's end.
    std::string open = "$a\n\nb $c$ d";
    CHECK(hasToken(open, "tex", "$a\n", TokenStyle::Number));
    CHECK(hasToken(open, "tex", "$c$", TokenStyle::Number));
    // \begin / \end and their environment names.
    CHECK(hasToken("\\begin{itemize}", "tex", "\\begin", TokenStyle::Keyword));
    CHECK(hasToken("\\begin{itemize}", "tex", "itemize", TokenStyle::Type));
    std::string eq = "\\begin{equation}\nE = mc^2 % note\n\\end{equation}\nx";
    CHECK(hasToken(eq, "tex", "\nE = mc^2 ", TokenStyle::Number));
    CHECK(hasToken(eq, "tex", "% note", TokenStyle::Comment));
    CHECK(hasToken(eq, "tex", "\\end", TokenStyle::Keyword));
    CHECK(hasToken(eq, "tex", "equation", TokenStyle::Type));
    // Verbatim: nothing inside is TeX, and what follows is normal again.
    std::string verb = "\\begin{verbatim}\n$x % \\y\n\\end{verbatim}\nafter $z$\n";
    CHECK(hasToken(verb, "tex", "\n$x % \\y\n", TokenStyle::String));
    CHECK(hasToken(verb, "tex", "verbatim", TokenStyle::Type));
    CHECK(hasToken(verb, "tex", "$z$", TokenStyle::Number));
    CHECK(!hasStyle(verb, "tex", TokenStyle::Comment));
    CHECK(hasToken("\\begin{comment}\n\\foo $\n\\end{comment} $q$", "tex",
                   "\n\\foo $\n", TokenStyle::Comment));
    CHECK(hasToken("\\begin{comment}\n\\foo $\n\\end{comment} $q$", "tex",
                   "$q$", TokenStyle::Number));
    std::string vb = "\\verb|$%| then $m$ \\verb*+a+ \\verb@b@";
    CHECK(hasToken(vb, "tex", "\\verb", TokenStyle::Keyword));
    CHECK(hasToken(vb, "tex", "|$%|", TokenStyle::String));
    CHECK(hasToken(vb, "tex", "$m$", TokenStyle::Number));
    CHECK(hasToken(vb, "tex", "\\verb*", TokenStyle::Keyword));
    CHECK(hasToken(vb, "tex", "+a+", TokenStyle::String));
    CHECK(hasToken(vb, "tex", "@b@", TokenStyle::String));
    // An unclosed \verb ends with its line.
    CHECK(hasToken("\\verb|abc\n$d$", "tex", "|abc", TokenStyle::String));
    CHECK(hasToken("\\verb|abc\n$d$", "tex", "$d$", TokenStyle::Number));
    // '@' is a letter in command names (.sty); .bib entries.
    CHECK(hasToken("\\@ifnextchar[", "sty", "\\@ifnextchar", TokenStyle::Keyword));
    std::string bib = "@article{key,\n  title = {On $x$},\n}\n";
    CHECK(hasToken(bib, "bib", "@article", TokenStyle::Preprocessor));
    CHECK(hasToken(bib, "bib", "$x$", TokenStyle::Number));
    CHECK(!hasStyle(bib, "tex", TokenStyle::Preprocessor));
}

// ------------------------------------------------ incremental highlighting
// The line-at-a-time lexer must give exactly the old whole-file tokens, and
// the incremental highlighter must, after any edit, leave every line with
// exactly the tokens a fresh full lex gives. Checked by applying thousands of
// random edits to real files and comparing after each one.

std::string readFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Cut tokens at line boundaries, the way IncrementalHighlighter reports them:
// a piece ends just after a '\n' it covers.
template <class Ch>
std::vector<Token> splitAtLines(const std::vector<Token> &toks,
                                const std::basic_string<Ch> &text) {
    std::vector<Token> out;
    for (const Token &t : toks) {
        size_t s = t.start, e = t.start + t.length;
        while (s < e) {
            size_t nl = text.find(Ch('\n'), s);
            size_t cut = (nl == std::basic_string<Ch>::npos || nl + 1 > e) ? e : nl + 1;
            out.push_back({s, cut - s, t.style});
            s = cut;
        }
    }
    return out;
}

// One style per code unit, Plain where no token is.
std::vector<TokenStyle> styleMap(const std::vector<Token> &toks, size_t n) {
    std::vector<TokenStyle> m(n, TokenStyle::Plain);
    for (const Token &t : toks)
        for (size_t i = t.start; i < t.start + t.length && i < n; ++i) m[i] = t.style;
    return m;
}

// Minimal UTF-8 <-> UTF-16 for the test (input is valid UTF-8).
std::u16string toU16(const std::string &s) {
    std::u16string u;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u += (char16_t)(0xD800 + (cp >> 10));
            u += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u += (char16_t)cp;
        }
    }
    return u;
}
std::string toU8(const std::u16string &u, std::vector<size_t> *u16AtByte) {
    std::string s;
    if (u16AtByte) u16AtByte->clear();
    for (size_t i = 0; i < u.size();) {
        uint32_t cp = u[i];
        size_t units = 1;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            units = 2;
        }
        size_t before = s.size();
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        }
        if (u16AtByte) for (size_t k = before; k < s.size(); ++k) u16AtByte->push_back(i);
        i += units;
    }
    if (u16AtByte) u16AtByte->push_back(u.size());
    return s;
}

// Grammars the frozen lexer never had. For these the reference is a full lex
// by the new highlighter (every line in order from the top), so the random
// edits still check the incremental bookkeeping, and the UTF-16 run checks
// that UTF-16 gives the same tokens as UTF-8.
bool newGrammar(const std::string &ext) {
    return ext == "tex" || ext == "ltx" || ext == "latex" || ext == "sty" ||
           ext == "cls" || ext == "bib";
}
std::vector<Token> fullLex(const std::string &text, const std::string &ext) {
    return newGrammar(ext) ? SyntaxHighlighter::highlight(text, ext)
                           : legacy::highlight(text, ext);
}

// The reference tokens for a document: the frozen whole-file lexer (or a full
// lex, for a grammar it lacks), cut at lines, in the document's own units.
std::vector<Token> reference(const std::string &text, const std::string &ext) {
    return splitAtLines(fullLex(text, ext), text);
}
std::vector<Token> reference(const std::u16string &text, const std::string &ext) {
    std::vector<size_t> map;
    std::string u8 = toU8(text, &map);
    std::vector<Token> toks;
    for (const Token &t : fullLex(u8, ext)) {
        size_t a = map[t.start], b = map[t.start + t.length];
        if (b > a) toks.push_back({a, b - a, t.style});
    }
    return splitAtLines(toks, text);
}

// Fragments that open and close every multi-line construct, plus ordinary
// code and non-ASCII text.
const char *const kFragments[] = {
    "/*", "*/", "\"", "'", "`", "\"\"\"", "'''", "\\", "\\\n", "\n", "\n\n",
    "#", "//", "@", "# ", "#include <x>\n", "x(", "foo (", "Bar", "0x1F", "3.14",
    ".5", " ", "\t", "\r\n", "if ", "return ", "def ", "self", "é", "日本",
    "\xF0\x9F\x98\x80", "/* c */", "\"s\"", "a = \"b\\\"c\"", "*/\n", "/*\n",
    "\"\"\"doc\n", "x", "}", "{\n",
    // TeX: math, comments, verbatim and the environments that carry state.
    "$", "$$", "\\(", "\\)", "\\[", "\\]", "%", "\\%", "\\\\", "\\alpha",
    "\\section{A}", "\\begin{verbatim}", "\\end{verbatim}", "\\begin{equation}\n",
    "\\end{equation}", "\\begin{comment}", "\\end{comment}", "\\verb|x|",
    "\\verb", "|", "@article{", "\\begin{itemize}", "\n \n",
};

template <class Ch>
std::basic_string<Ch> fromUtf8(const std::string &s);
template <> std::string fromUtf8<char>(const std::string &s) { return s; }
template <> std::u16string fromUtf8<char16_t>(const std::string &s) { return toU16(s); }

// Keep a split never landing inside a UTF-8 sequence or surrogate pair, so
// the UTF-16 reference stays convertible. (Edits in UTF-8 may split
// sequences; the lexer treats stray bytes as punctuation either way.)
size_t snap(const std::u16string &t, size_t p) {
    if (p > 0 && p < t.size() && t[p] >= 0xDC00 && t[p] < 0xE000) --p;
    return p;
}
size_t snap(const std::string &, size_t p) { return p; }

struct FuzzStats { size_t edits = 0, linesRelexed = 0, lineTotal = 0; };

// The line holding offset `off`.
template <class Ch>
size_t lineOf(const IncrementalHighlighter<Ch> &h, size_t off) {
    size_t lo = 0, hi = h.lineCount();   // first line starting after off
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (h.lineStart(mid) <= off) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
}

// Apply `rounds` random edits to `text`, checking the incremental result
// against a full lex after each one. Returns false (and prints) on the first
// mismatch.
template <class Ch>
bool fuzzIncremental(std::basic_string<Ch> text, const std::string &ext,
                     unsigned seed, int rounds, FuzzStats &stats) {
    using Str = std::basic_string<Ch>;
    std::mt19937 rng(seed);
    auto rnd = [&](size_t n) { return n ? (size_t)(rng() % n) : 0; };

    IncrementalHighlighter<Ch> h(ext);
    std::vector<Token> all;
    h.reset(StringSource<Ch>(text), &all);
    if (all != reference(text, ext)) {
        std::printf("    reset differs from a full lex (%s)\n", ext.c_str());
        return false;
    }
    // The GUI's view: tokens per line, relative to the line start (they move
    // with the text), and a style per unit (attributes move with the text).
    std::vector<std::vector<Token>> perLine(h.lineCount());
    for (const Token &t : all) {
        size_t line = lineOf(h, t.start);
        perLine[line].push_back({t.start - h.lineStart(line), t.length, t.style});
    }
    std::vector<TokenStyle> styles = styleMap(all, text.size());

    const size_t nFrag = sizeof(kFragments) / sizeof(kFragments[0]);
    for (int round = 0; round < rounds; ++round) {
        // Build the edit: sometimes two separate edits reported as one range.
        size_t pos = snap(text, rnd(text.size() + 1));
        size_t maxDel = text.size() - pos;
        size_t oldLen = 0;
        switch (rnd(4)) {
        case 0: oldLen = 0; break;
        case 1: oldLen = std::min<size_t>(maxDel, 1 + rnd(3)); break;
        case 2: oldLen = std::min<size_t>(maxDel, rnd(40)); break;
        default: oldLen = std::min<size_t>(maxDel, rnd(400)); break;
        }
        oldLen = snap(text, pos + oldLen) - pos;
        Str ins;
        size_t kind = rnd(10);
        if (kind < 6) {
            for (size_t k = 0, m = 1 + rnd(3); k < m; ++k)
                ins += fromUtf8<Ch>(kFragments[rnd(nFrag)]);
        } else if (kind < 8 && !text.empty()) {          // paste a slice
            size_t a = snap(text, rnd(text.size()));
            size_t b = snap(text, std::min(text.size(), a + rnd(300)));
            if (b > a) ins = text.substr(a, b - a);
        }                                                 // else: pure delete
        size_t newLen = ins.size();
        Str before = text;
        text.replace(pos, oldLen, ins);

        size_t repPos = pos, repOld = oldLen, repNew = newLen;
        if (rnd(8) == 0 && pos + newLen < text.size()) {
            // A second edit further on, reported together with the first.
            size_t p2 = snap(text, pos + newLen + rnd(text.size() - pos - newLen));
            size_t d2 = snap(text, std::min(text.size(), p2 + rnd(5))) - p2;
            Str ins2 = fromUtf8<Ch>(kFragments[rnd(nFrag)]);
            text.replace(p2, d2, ins2);
            size_t endNew = p2 + ins2.size();
            repNew = endNew - pos;
            repOld = repNew + before.size() - text.size();
        }

        size_t oldLines = h.lineCount();
        std::vector<Token> got;
        auto r = h.edit(StringSource<Ch>(text), repPos, repOld, repNew, got);
        stats.edits++;
        stats.linesRelexed += r.endLine - r.firstLine;
        stats.lineTotal += h.lineCount();

        // Per-line model: replace the re-lexed lines, keep the rest.
        size_t delta = h.lineCount() - oldLines;   // wraps when lines were removed
        std::vector<std::vector<Token>> next;
        next.reserve(h.lineCount());
        for (size_t k = 0; k < r.firstLine; ++k) next.push_back(perLine[k]);
        for (size_t k = r.firstLine; k < r.endLine; ++k) next.emplace_back();
        for (const Token &t : got) {
            size_t line = lineOf(h, t.start);
            if (line < r.firstLine || line >= r.endLine) {
                std::printf("    token outside the re-lexed lines\n");
                return false;
            }
            next[line].push_back({t.start - h.lineStart(line), t.length, t.style});
        }
        for (size_t k = r.endLine; k < h.lineCount(); ++k)
            next.push_back(perLine[k - delta]);
        perLine.swap(next);

        // Asking again for the same lines, from the stored states, agrees.
        std::vector<Token> again;
        h.lineTokens(StringSource<Ch>(text), r.firstLine, r.endLine, again);
        if (again != got || lineOf(h, repPos) != h.lineOf(repPos)) {
            std::printf("    lineTokens/lineOf disagree with edit()\n");
            return false;
        }

        std::vector<Token> flat;
        for (size_t k = 0; k < perLine.size(); ++k)
            for (const Token &t : perLine[k])
                flat.push_back({t.start + h.lineStart(k), t.length, t.style});
        std::vector<Token> want = reference(text, ext);

        // Style-per-unit model, the way NSTextStorage keeps attributes.
        styles.erase(styles.begin() + repPos, styles.begin() + repPos + repOld);
        styles.insert(styles.begin() + repPos, repNew, TokenStyle::Plain);
        std::fill(styles.begin() + r.start, styles.begin() + r.end, TokenStyle::Plain);
        for (const Token &t : got)
            std::fill(styles.begin() + t.start, styles.begin() + t.start + t.length, t.style);

        IncrementalHighlighter<Ch> fresh(ext);
        fresh.reset(StringSource<Ch>(text), nullptr);
        bool statesOk = fresh.lineCount() == h.lineCount();
        for (size_t k = 0; statesOk && k < h.lineCount(); ++k)
            statesOk = fresh.endState(k) == h.endState(k) &&
                       fresh.lineStart(k) == h.lineStart(k);

        if (flat != want || styles != styleMap(want, text.size()) || !statesOk ||
            h.length() != text.size()) {
            std::printf("    %s: edit %d (pos %zu, -%zu +%zu) diverged: tokens %s, "
                        "styles %s, states %s\n", ext.c_str(), round, repPos, repOld,
                        repNew, flat == want ? "ok" : "DIFFER",
                        styles == styleMap(want, text.size()) ? "ok" : "DIFFER",
                        statesOk ? "ok" : "DIFFER");
            return false;
        }
    }
    return true;
}

void testIncrementalHighlight() {
    struct Sample { std::string name, text; };
    std::vector<Sample> samples;
    for (const char *p : {"demo/sample.cpp", "demo/hello.py", "demo/README.md",
                          "src/SyntaxHighlighter.cpp", "src/LatexDoc.cpp",
                          "scripts/release.sh", "tests/LegacyHighlighter.h",
                          "demo/paper/notes.tex", "tests/latex/torture.tex"}) {
        std::string t = readFile(p);
        GROUP("incremental:samples");
        CHECK(!t.empty());   // run from the repo root (make test does)
        if (!t.empty()) samples.push_back({p, t});
    }
    samples.push_back({"js", "// js\nconst a = `tpl\nline` + 'x';\n/* multi\n"
                             " line */ function f(x) { return x * 2; }\n"
                             "let s = \"esc \\\n continued\";\n"});
    samples.push_back({"py", "def f():\n    '''doc\n    more'''\n    s = \"\"\"a\n\"\"\"\n"
                             "@dec\nclass K: pass  # note\nx = 'a\\\nb'\n"});
    samples.push_back({"conf", "# settings\nwindow.background = #1E1E1E\n"
                               "key = \"quoted # not comment\"\n"});
    samples.push_back({"empty", ""});
    samples.push_back({"unicode", "s = \"h\xC3\xA9llo \xF0\x9F\x98\x80\"; /* \xE6\x97\xA5\n"
                                  "\xE6\x9C\xAC */ int caf\xC3\xA9(x);\n"});
    samples.push_back({"tex",
        "\\documentclass{article} % class\n\\usepackage{amsmath}\n"
        "\\begin{document}\n\\section{Caf\xC3\xA9} Price: 50\\%, $x^2 + \\alpha$\n"
        "and $a\nb$ over lines, \\(c\\) and $$\nd\n$$ then\n\\[\n  e = \\frac{1}{2}\n\\]\n"
        "\\begin{equation}\n  E = mc^2 % energy\n\n  lost\n\\end{equation}\n"
        "\\begin{verbatim}\n$ not math % not comment \\[\n\\end{verbatim}\n"
        "\\begin{lstlisting}[language=C]\nint x; // \xE6\x97\xA5\n\\end{lstlisting}\n"
        "\\verb|$%| and \\verb*+\\[+ \\verb|open\n"
        "\\begin{comment}\nhidden $\n\\end{comment}\n"
        "@article{k, title = {On $x$}}\n\\end{document}\n"});
    // The old-lexer comparison covers only the grammars it had; the random
    // edits also run TeX, checked against a full lex (see newGrammar).
    const char *exts[] = {"cpp", "py", "js", "sh", "txt"};
    const char *fuzzExts[] = {"cpp", "py", "js", "sh", "txt", "tex", "bib"};

    GROUP("incremental:matches-old-lexer");
    // The line lexer changed no token anywhere, joined back into whole tokens.
    bool same = true;
    for (const Sample &s : samples)
        for (const char *e : exts)
            if (SyntaxHighlighter::highlight(s.text, e) != legacy::highlight(s.text, e)) {
                std::printf("    %s as %s differs from the old lexer\n", s.name.c_str(), e);
                same = false;
            }
    CHECK(same);
    // Random soup of the tricky fragments, too.
    {
        std::mt19937 rng(7);
        bool soup = true;
        for (int k = 0; k < 400; ++k) {
            std::string t;
            for (int m = 0, len = 1 + (int)(rng() % 60); m < len; ++m)
                t += kFragments[rng() % (sizeof(kFragments) / sizeof(kFragments[0]))];
            for (const char *e : exts)
                soup = soup && SyntaxHighlighter::highlight(t, e) == legacy::highlight(t, e);
        }
        CHECK(soup);
    }

    GROUP("incremental:line-states");
    {
        IncrementalHighlighter<char> h("cpp");
        std::string t = "a /* b\nc\nd */ e\n\"x\\\ny\"\n";
        h.reset(StringSource<char>(t), nullptr);
        CHECK(h.lineCount() == 6);
        CHECK(h.endState(0).kind == LexState::BlockComment);
        CHECK(h.endState(1).kind == LexState::BlockComment);
        CHECK(h.endState(2).kind == LexState::Normal);
        CHECK(h.endState(3).kind == LexState::StringCont && h.endState(3).quote == u'"');
        CHECK(h.endState(4).kind == LexState::Normal);
        IncrementalHighlighter<char> p("py");
        std::string py = "x = '''a\nb''' + \"\"\"c\n";
        p.reset(StringSource<char>(py), nullptr);
        CHECK(p.endState(0).kind == LexState::TripleString && p.endState(0).quote == u'\'');
        CHECK(p.endState(1).kind == LexState::TripleString && p.endState(1).quote == u'"');

        IncrementalHighlighter<char> x("tex");
        std::string tx = "$a\nb$ \\[\nc\n\\] \\begin{verbatim}\n\n$\n\\end{verbatim} "
                         "\\begin{align}\nx\n\n$$\n$$ \\end{align}\n\\(\n";
        x.reset(StringSource<char>(tx), nullptr);
        CHECK(x.lineCount() == 13);
        CHECK(x.endState(0).kind == LexState::TexMath && x.endState(0).quote == u'$');
        CHECK(x.endState(1).kind == LexState::TexMath && x.endState(1).quote == u']');
        CHECK(x.endState(2).kind == LexState::TexMath && x.endState(2).quote == u']');
        CHECK(x.endState(3).kind == LexState::TexEnv);           // verbatim
        CHECK(x.endState(4) == x.endState(3));                    // blank: still verbatim
        CHECK(x.endState(5) == x.endState(3));                    // $ is text there
        CHECK(x.endState(6).kind == LexState::TexEnv && x.endState(6) != x.endState(3));
        CHECK(x.endState(7) == x.endState(6));                    // align
        CHECK(x.endState(8).kind == LexState::Normal);            // blank ends math
        CHECK(x.endState(9).kind == LexState::TexMath && x.endState(9).quote == u'D');
        CHECK(x.endState(10).kind == LexState::Normal);
        CHECK(x.endState(11).kind == LexState::TexMath && x.endState(11).quote == u')');
        CHECK(x.endState(12).kind == LexState::Normal);           // empty last line
    }

    GROUP("incremental:edit-scope");
    {
        // Typing a letter re-lexes one line; opening a comment re-lexes to the
        // end; closing it again stops as soon as the states agree.
        std::string t;
        for (int k = 0; k < 200; ++k) t += "int x = 1; // line\n";
        IncrementalHighlighter<char> h("cpp");
        h.reset(StringSource<char>(t), nullptr);
        std::vector<Token> out;
        t.insert(100, "y");
        auto r = h.edit(StringSource<char>(t), 100, 0, 1, out);
        CHECK(r.endLine - r.firstLine == 1);
        CHECK(r.start == h.lineStart(r.firstLine) && r.end == h.lineStart(r.firstLine + 1));
        out.clear();
        t.insert(40, "/*");
        r = h.edit(StringSource<char>(t), 40, 0, 2, out);
        CHECK(r.firstLine == 2 && r.endLine == h.lineCount());
        CHECK(!out.empty() && out.back().style == TokenStyle::Comment);
        out.clear();
        t.insert(80, "*/");
        r = h.edit(StringSource<char>(t), 80, 0, 2, out);
        CHECK(r.endLine == h.lineCount());   // everything below uncomments
        out.clear();
        t.erase(80, 2);
        r = h.edit(StringSource<char>(t), 80, 2, 0, out);
        CHECK(r.endLine == h.lineCount());
        out.clear();
        t.erase(40, 2);
        r = h.edit(StringSource<char>(t), 40, 2, 0, out);
        CHECK(r.endLine == h.lineCount());
        // A report that does not add up falls back to a full lex.
        out.clear();
        r = h.edit(StringSource<char>(t), 0, 5, 0, out);
        CHECK(r.firstLine == 0 && r.endLine == h.lineCount() && r.end == t.size());
        CHECK(out == reference(t, "cpp"));
    }
    {
        // In TeX an unclosed $ runs only to the paragraph's end, so typing
        // one re-lexes the paragraph, not the rest of the document.
        std::string t;
        for (int k = 0; k < 50; ++k) t += "Some prose with \\emph{a} word.\nMore.\n\n";
        IncrementalHighlighter<char> h("tex");
        h.reset(StringSource<char>(t), nullptr);
        std::vector<Token> out;
        t.insert(5, "$");
        auto r = h.edit(StringSource<char>(t), 5, 0, 1, out);
        CHECK(r.firstLine == 0 && r.endLine == 3);
        CHECK(h.endState(1).kind == LexState::TexMath && h.endState(2).kind == LexState::Normal);
        out.clear();
        t.insert(t.find("word"), "$");
        r = h.edit(StringSource<char>(t), t.find("$word"), 0, 1, out);
        CHECK(r.firstLine == 0 && r.endLine == 3);
        CHECK(h.endState(0).kind == LexState::Normal && h.endState(1).kind == LexState::Normal);
    }

    GROUP("incremental:random-edits");
    FuzzStats stats;
    unsigned seed = 1;
    for (const Sample &s : samples) {
        for (const char *e : fuzzExts) {
            bool ok = fuzzIncremental<char>(s.text, e, seed++, 120, stats);
            if (!ok) std::printf("    (UTF-8, sample %s)\n", s.name.c_str());
            CHECK(ok);
            ok = fuzzIncremental<char16_t>(toU16(s.text), e, seed++, 120, stats);
            if (!ok) std::printf("    (UTF-16, sample %s)\n", s.name.c_str());
            CHECK(ok);
        }
    }
    std::printf("  incremental: %zu random edits matched a full lex; "
                "%.1f lines re-lexed per edit (of %.0f)\n", stats.edits,
                (double)stats.linesRelexed / stats.edits,
                (double)stats.lineTotal / stats.edits);
}

// Timing on a big file: a full lex against one keystroke. Printed, not
// checked, since machines differ; the edit-scope checks above are the
// deterministic part.
void benchIncrementalHighlight() {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    std::string unit = readFile("demo/sample.cpp");
    if (unit.empty()) return;
    std::u16string text;
    {
        std::string big;
        size_t lines = 0;
        while (lines < 100000) {
            big += unit;
            // No block comments, so a "/*" typed at the top runs to the end.
            big += "// note\nstatic const char *s = \"text\";\n";
            lines += 11;
        }
        text = toU16(big);
    }
    size_t nLines = std::count(text.begin(), text.end(), u'\n');

    IncrementalHighlighter<char16_t> h("cpp");
    auto t0 = Clock::now();
    std::vector<Token> all;
    h.reset(StringSource<char16_t>(text), &all);
    double full = ms(Clock::now() - t0);

    // One keystroke at a time in the middle, then deleted again.
    const int kEdits = 2000;
    std::vector<Token> out;
    size_t pos = text.size() / 2;
    t0 = Clock::now();
    for (int k = 0; k < kEdits; ++k) {
        out.clear();
        if (k % 2 == 0) { text.insert(pos, 1, u'x'); h.edit(StringSource<char16_t>(text), pos, 0, 1, out); }
        else { text.erase(pos, 1); h.edit(StringSource<char16_t>(text), pos, 1, 0, out); }
    }
    double perKey = ms(Clock::now() - t0) / kEdits;

    // Worst case: open a block comment near the top, then close it.
    size_t top = text.find(u'\n') + 1;   // line 2 (line 1 is an #include)
    out.clear();
    t0 = Clock::now();
    text.insert(top, u"/*");
    auto r = h.edit(StringSource<char16_t>(text), top, 0, 2, out);
    double openAll = ms(Clock::now() - t0);
    size_t openLines = r.endLine - r.firstLine;
    out.clear();
    t0 = Clock::now();
    text.erase(top, 2);
    h.edit(StringSource<char16_t>(text), top, 2, 0, out);
    double closeAll = ms(Clock::now() - t0);

    std::printf("  bench: %zu lines of C++ (%zu tokens): full lex %.1f ms, "
                "one keystroke %.4f ms; typing /* at the top %.1f ms (%zu lines), "
                "deleting it %.1f ms\n",
                nLines, all.size(), full, perKey, openAll, openLines, closeAll);
}

// --------------------------------------------------------------- markdown
bool anyRun(const std::vector<MdRun> &runs,
            bool (*pred)(const MdRun &)) {
    for (const MdRun &r : runs) if (pred(r)) return true;
    return false;
}

// Join the table runs into display lines (the text a user actually sees).
std::vector<std::string> tableLines(const std::vector<MdRun> &runs) {
    std::vector<std::string> lines;
    std::string cur;
    for (const MdRun &r : runs) {
        if (!r.table) continue;
        for (char c : r.text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Display columns of every column divider (│ in body rows, ┼ in the rule).
// Counts code points, with CJK ideographs as two columns.
std::vector<size_t> dividerColumns(const std::string &line) {
    std::vector<size_t> cols;
    size_t col = 0;
    for (size_t i = 0; i < line.size();) {
        unsigned char c = (unsigned char)line[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        std::string ch = line.substr(i, len);
        if (ch == "\xE2\x94\x82" || ch == "\xE2\x94\xBC") cols.push_back(col);
        col += (len == 3 && c >= 0xE3) ? 2 : 1;   // U+3000.. are wide here
        i += len;
    }
    return cols;
}

// Every row puts its dividers in the same screen columns.
bool columnsAligned(const std::vector<std::string> &lines) {
    if (lines.empty()) return false;
    auto first = dividerColumns(lines[0]);
    for (const auto &l : lines)
        if (dividerColumns(l) != first) return false;
    return true;
}

void testMarkdown() {
    GROUP("md:headings");
    auto h = MarkdownParser::parse("# Title\n");
    CHECK(anyRun(h, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Title") != std::string::npos;
    }));
    auto h2 = MarkdownParser::parse("## Sub\n");
    CHECK(anyRun(h2, [](const MdRun &r) { return r.heading == 2; }));
    // '#' without a following space is not a heading.
    auto notH = MarkdownParser::parse("#nospace\n");
    CHECK(!anyRun(notH, [](const MdRun &r) { return r.heading > 0; }));

    GROUP("md:inline");
    CHECK(anyRun(MarkdownParser::parse("**bold**"),
                 [](const MdRun &r) { return r.bold && r.text == "bold"; }));
    CHECK(anyRun(MarkdownParser::parse("*ital*"),
                 [](const MdRun &r) { return r.italic && r.text == "ital"; }));
    CHECK(anyRun(MarkdownParser::parse("`code`"),
                 [](const MdRun &r) { return r.code && r.text == "code"; }));
    CHECK(anyRun(MarkdownParser::parse("[t](http://x)"), [](const MdRun &r) {
        return r.link && r.text == "t" && r.url == "http://x";
    }));

    GROUP("md:blocks");
    CHECK(anyRun(MarkdownParser::parse("```\nx=1\n```\n"), [](const MdRun &r) {
        return r.codeBlock && r.text.find("x=1") != std::string::npos;
    }));
    CHECK(anyRun(MarkdownParser::parse("- item\n"),
                 [](const MdRun &r) { return r.listDepth > 0; }));
    CHECK(anyRun(MarkdownParser::parse("> quote\n"),
                 [](const MdRun &r) { return r.quote; }));
    CHECK(anyRun(MarkdownParser::parse("---\n"),
                 [](const MdRun &r) { return r.rule; }));

    GROUP("md:tables");
    auto tbl = MarkdownParser::parse("| A | B |\n| --- | --- |\n| 1 | 2 |\n");
    CHECK(anyRun(tbl, [](const MdRun &r) { return r.table; }));
    CHECK(anyRun(tbl, [](const MdRun &r) {  // header row is bold
        return r.table && r.bold && r.text.find("A") != std::string::npos;
    }));
    CHECK(anyRun(tbl, [](const MdRun &r) {
        return r.table && r.text.find("1") != std::string::npos;
    }));
    // A pipe line without a separator row underneath is NOT a table.
    auto notTbl = MarkdownParser::parse("a | b is just text\n");
    CHECK(!anyRun(notTbl, [](const MdRun &r) { return r.table; }));

    GROUP("md:tables-layout");
    // Rows come out as: header, rule, body rows.
    auto lay = tableLines(MarkdownParser::parse(
        "| Name | Qty |\n|------|-----|\n| apple | 3 |\n| fig | 12 |\n"));
    CHECK(lay.size() == 4);
    CHECK(lay.size() == 4 && lay[0] == "Name  │ Qty");
    CHECK(lay.size() == 4 && lay[1] == "──────┼────");
    CHECK(lay.size() == 4 && lay[2] == "apple │ 3  ");
    CHECK(lay.size() == 4 && lay[3] == "fig   │ 12 ");
    CHECK(columnsAligned(lay));

    GROUP("md:tables-unicode");
    // Widths are measured in characters, not UTF-8 bytes: "café" and "✓ ok"
    // are longer in bytes than on screen, which used to push columns right.
    auto uni = tableLines(MarkdownParser::parse(
        "| Word | Mark |\n|---|---|\n| café | ✓ ok |\n| tea | x |\n"));
    CHECK(uni.size() == 4);
    CHECK(columnsAligned(uni));
    CHECK(uni.size() == 4 && uni[2] == "café │ ✓ ok");
    CHECK(uni.size() == 4 && uni[3] == "tea  │ x   ");
    // Wide (CJK) characters take two columns.
    auto cjk = tableLines(MarkdownParser::parse(
        "| k | v |\n|---|---|\n| 日本 | 1 |\n| ab | 2 |\n"));
    CHECK(cjk.size() == 4 && cjk[3] == "ab   │ 2");
    CHECK(columnsAligned(cjk));

    GROUP("md:tables-inline");
    // Inline markup in cells is rendered, not shown raw, and the hidden markup
    // characters don't count toward the column width.
    auto inl = MarkdownParser::parse(
        "| Kind | Example |\n|---|---|\n| **strong** | `x = 1` |\n"
        "| link | [docs](http://d) |\n| plain | abc |\n");
    auto inlLines = tableLines(inl);
    CHECK(columnsAligned(inlLines));
    CHECK(inlLines.size() == 5 && inlLines[2] == "strong │ x = 1  ");
    CHECK(inlLines.size() == 5 && inlLines[3] == "link   │ docs   ");
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.bold && r.text == "strong";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.code && r.text == "x = 1";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.link && r.text == "docs" && r.url == "http://d";
    }));
    CHECK(!anyRun(inl, [](const MdRun &r) {
        return r.text.find("**") != std::string::npos ||
               r.text.find('`') != std::string::npos;
    }));

    GROUP("md:tables-align");
    auto al = tableLines(MarkdownParser::parse(
        "| L | C | R |\n|:--|:-:|--:|\n| a | b | c |\n| long | wide | 12345 |\n"));
    CHECK(al.size() == 4 && al[0] == "L    │  C   │     R");
    CHECK(al.size() == 4 && al[2] == "a    │  b   │     c");
    CHECK(columnsAligned(al));

    GROUP("md:tables-shape");
    // No leading/trailing pipes is still a table.
    CHECK(tableLines(MarkdownParser::parse("A | B\n--- | ---\n1 | 2\n")).size() == 3);
    // Short rows are padded out to the header's column count, long rows are cut.
    auto ragged = tableLines(MarkdownParser::parse(
        "| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 |\n"));
    CHECK(ragged.size() == 4 && ragged[2] == "1 │   │  ");
    CHECK(ragged.size() == 4 && ragged[3] == "1 │ 2 │ 3");
    // An escaped pipe is cell content, not a column break.
    auto esc = tableLines(MarkdownParser::parse(
        "| op | means |\n|---|---|\n| a \\| b | or |\n"));
    CHECK(esc.size() == 3 && esc[2] == "a | b │ or   ");
    // Header and separator column counts must match.
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // A separator cell must be dashes (optionally colon-wrapped).
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| -x- | --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // The table ends at a blank line or a line without pipes, and the text
    // around it stays ordinary paragraph text on its own lines.
    auto around = MarkdownParser::parse(
        "Before\n| a | b |\n|---|---|\n| 1 | 2 |\nAfter\n");
    CHECK(tableLines(around).size() == 3);
    CHECK(anyRun(around, [](const MdRun &r) {
        return !r.table && r.text == "After";
    }));
    CHECK(!anyRun(around, [](const MdRun &r) {
        return r.table && r.text.find("fter") != std::string::npos;
    }));

    GROUP("md:block-separation");
    // A heading directly after a paragraph (no blank line) must still be a
    // heading, preceded by a newline rather than glued onto the paragraph.
    auto glued = MarkdownParser::parse("paragraph text\n# Heading\n");
    CHECK(anyRun(glued, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Heading") != std::string::npos;
    }));
    bool sawNewlineBeforeHeading = false;
    {
        bool prevEndsNewline = true;  // start of doc counts as a line start
        for (const MdRun &r : glued) {
            if (r.heading == 1) { sawNewlineBeforeHeading = prevEndsNewline; break; }
            prevEndsNewline = !r.text.empty() && r.text.back() == '\n';
        }
    }
    CHECK(sawNewlineBeforeHeading);
}

}  // namespace

// ---------------------------------------------------------- terminal stream
namespace {

// Plays events back the way the panel does: ended lines are committed, the
// live line is replaced by each new Line event.
struct Panel {
    std::vector<std::vector<TermRun>> lines;   // ended lines
    std::vector<TermRun> live;
    std::string events;                        // trace: L l A C D P
    int lastStatus = -1;
    std::string lastDir;

    void apply(const std::vector<TermEvent> &evs) {
        for (const TermEvent &e : evs) {
            switch (e.kind) {
            case TermEvent::Line:
                events += e.ended ? 'L' : 'l';
                if (e.ended) { lines.push_back(e.runs); live.clear(); }
                else live = e.runs;
                break;
            case TermEvent::PromptStart:  events += 'A'; break;
            case TermEvent::CommandStart: events += 'C'; break;
            case TermEvent::CommandEnd:
                events += 'D'; lastStatus = e.status; break;
            case TermEvent::Directory:
                events += 'P'; lastDir = e.text; break;
            }
        }
    }
    static std::string plain(const std::vector<TermRun> &runs) {
        std::string s;
        for (const TermRun &r : runs) s += r.text;
        return s;
    }
    // Everything on screen as plain text, lines joined with \n.
    std::string text() const {
        std::string s;
        for (const auto &l : lines) s += plain(l) + "\n";
        return s + plain(live);
    }
};

Panel run(const std::string &input) {
    TerminalStream ts;
    Panel p;
    p.apply(ts.feed(input));
    return p;
}

// Feed `input` one byte at a time, the worst case for split sequences.
Panel runBytewise(const std::string &input) {
    TerminalStream ts;
    Panel p;
    for (char c : input) p.apply(ts.feed(&c, 1));
    return p;
}

// The style of the run containing `needle` anywhere in the panel.
const TermStyle *styleOf(const Panel &p, const std::string &needle) {
    auto search = [&](const std::vector<TermRun> &runs) -> const TermStyle * {
        for (const TermRun &r : runs)
            if (r.text.find(needle) != std::string::npos) return &r.style;
        return nullptr;
    };
    for (const auto &l : p.lines)
        if (const TermStyle *s = search(l)) return s;
    return search(p.live);
}

bool isIndexed(const TermColor &c, int index) {
    return c.kind == TermColor::Indexed && c.index == index;
}

void testTerminalStream() {
    GROUP("term:text");
    {
        Panel p = run("hello\r\nworld\r\n");
        CHECK(p.text() == "hello\nworld\n");    // \r\n ends a line
        CHECK(p.events == "LL");
        Panel partial = run("prompt> ");       // no newline: a live line
        CHECK(partial.events == "l" && partial.text() == "prompt> ");
        CHECK(run("a\r\n\r\nb").text() == "a\n\nb");   // empty lines survive
    }

    GROUP("term:escapes-dropped");
    {
        // Cursor visibility, other-line movement, charset select: no text.
        CHECK(run("\x1b[?25lok\x1b[2A\x1b[10;5H\x1b(B!\x1b[?25h").text() == "ok!");
        // An OSC we don't use (window title) and a DCS string vanish too.
        CHECK(run("\x1b]0;my title\x07" "a\x1bPq#0;2\x1b\\b").text() == "ab");
    }

    GROUP("term:sgr-basic");
    {
        Panel p = run("\x1b[31mred\x1b[0m plain \x1b[1;4;32mbold\x1b[22m under\x1b[m\n");
        CHECK(p.text() == "red plain bold under\n");
        const TermStyle *red = styleOf(p, "red");
        CHECK(red && isIndexed(red->fg, 1) && !red->bold);
        const TermStyle *plain = styleOf(p, "plain");
        CHECK(plain && *plain == TermStyle());
        const TermStyle *bold = styleOf(p, "bold");
        CHECK(bold && bold->bold && bold->underline && isIndexed(bold->fg, 2));
        const TermStyle *under = styleOf(p, "under");   // 22 ends bold only
        CHECK(under && !under->bold && under->underline && isIndexed(under->fg, 2));
        CHECK(p.lines.size() == 1 && p.lines[0].size() == 4);  // one run per style
    }

    GROUP("term:sgr-colors");
    {
        // Bright, 256-color (both separators), truecolor, backgrounds, resets.
        Panel p = run("\x1b[91ma\x1b[38;5;213mb\x1b[38:5:81mc"
                      "\x1b[38;2;10;20;30md\x1b[38:2::1:2:3me"
                      "\x1b[44;103mf\x1b[39;49mg\x1b[2;3;7;9mh\x1b[23;27;29mi");
        CHECK(p.text() == "abcdefghi");
        CHECK(isIndexed(styleOf(p, "a")->fg, 9));
        CHECK(isIndexed(styleOf(p, "b")->fg, 213));
        CHECK(isIndexed(styleOf(p, "c")->fg, 81));
        const TermColor &d = styleOf(p, "d")->fg;
        CHECK(d.kind == TermColor::RGB && d.r == 10 && d.g == 20 && d.b == 30);
        const TermColor &e = styleOf(p, "e")->fg;
        CHECK(e.kind == TermColor::RGB && e.r == 1 && e.g == 2 && e.b == 3);
        const TermStyle *f = styleOf(p, "f");
        CHECK(isIndexed(f->bg, 11) && e.kind == TermColor::RGB);
        const TermStyle *g = styleOf(p, "g");
        CHECK(g->fg.kind == TermColor::Default && g->bg.kind == TermColor::Default);
        const TermStyle *h = styleOf(p, "h");
        CHECK(h->dim && h->italic && h->inverse && h->strike);
        const TermStyle *i = styleOf(p, "i");
        CHECK(i->dim && !i->italic && !i->inverse && !i->strike);
        // Style carries across lines, as in a real terminal.
        Panel carry = run("\x1b[35mone\ntwo\x1b[0m");
        CHECK(isIndexed(styleOf(carry, "two")->fg, 5));
        // A malformed extended color is ignored rather than misread.
        Panel bad = run("\x1b[38;5mx");
        CHECK(styleOf(bad, "x")->fg.kind == TermColor::Default);
    }

    GROUP("term:palette");
    {
        TermColor c; c.kind = TermColor::Indexed;
        c.index = 1;   CHECK(c.rgb() == 0xCD3131);
        c.index = 16;  CHECK(c.rgb() == 0x000000);   // cube corner
        c.index = 196; CHECK(c.rgb() == 0xFF0000);   // cube pure red
        c.index = 213; CHECK(c.rgb() == 0xFF87FF);
        c.index = 232; CHECK(c.rgb() == 0x080808);   // gray ramp ends
        c.index = 255; CHECK(c.rgb() == 0xEEEEEE);
        TermColor rgb; rgb.kind = TermColor::RGB; rgb.r = 1; rgb.g = 2; rgb.b = 3;
        CHECK(rgb.rgb() == 0x010203);
    }

    GROUP("term:line-editing");
    {
        // \r then text overwrites in place; leftovers stay unless erased.
        CHECK(run("10%\r20%\n").text() == "20%\n");
        CHECK(run("abcdef\rXY\n").text() == "XYcdef\n");
        CHECK(run("downloading 99%\r\x1b[Kdone\n").text() == "done\n");
        // Erase to start of line, whole line, and N characters.
        CHECK(run("abcdef\x1b[3D\x1b[1K\n").text() == "    ef\n");
        CHECK(run("abcdef\x1b[2Kxy\n").text() == "      xy\n");
        CHECK(run("abcdef\r\x1b[2C\x1b[2X\n").text() == "ab  ef\n");
        // Backspace moves left; the next character overwrites.
        CHECK(run("ab\bc\n").text() == "ac\n");
        CHECK(run("|\b/\b-\n").text() == "-\n");
        // Cursor to column (1-based), forward past the end pads with spaces.
        CHECK(run("hello\x1b[1GJ\n").text() == "Jello\n");
        CHECK(run("a\x1b[3Cb\n").text() == "a   b\n");
        // Tabs move to the next multiple of 8, filling only past the end.
        CHECK(run("ab\tc\n").text() == "ab      c\n");
        CHECK(run("abcdefghij\r\tX\n").text() == "abcdefghXj\n");
        // A spinner redraw keeps its color on the rewritten character.
        Panel spin = run("\x1b[36m|\x1b[0m\r\x1b[36m/\x1b[0m");
        CHECK(spin.text() == "/" && isIndexed(styleOf(spin, "/")->fg, 6));
        // Combining marks join the previous character's cell.
        CHECK(run("e\xCC\x81x\b!\n").text() == "e\xCC\x81!\n");
    }

    GROUP("term:live-line-updates");
    {
        // Across reads, the live line is re-sent whole and replaced.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("50%"));
        p.apply(ts.feed("\r75%"));
        CHECK(p.events == "ll" && p.text() == "75%");
        p.apply(ts.feed("\r\n"));
        CHECK(p.events == "llL" && p.text() == "75%\n");
        // Moving the cursor alone changes nothing on screen: no event.
        CHECK(ts.feed("\r").empty());
        // The host breaking the line starts a fresh one.
        p.apply(ts.feed("typed"));
        ts.breakLine();
        Panel q;
        q.apply(ts.feed("next"));
        CHECK(q.text() == "next");
        // A huge unbroken line is ended automatically.
        Panel big = run(std::string(TerminalStream::kMaxLineCells + 10, 'x'));
        CHECK(big.lines.size() == 1 &&
              Panel::plain(big.lines[0]).size() == TerminalStream::kMaxLineCells);
        CHECK(Panel::plain(big.live).size() == 10);
    }

    GROUP("term:split-across-reads");
    {
        std::string input = "\x1b[38;5;213mpink\x1b[0m caf\xC3\xA9 \xE2\x9C\x93\r\n"
                            "50%\r\x1b[Kdone\r\n"
                            "\x1b]133;D;7\x07\x1b]7;file://h/tmp\x1b\\";
        Panel whole = run(input);
        Panel bytes = runBytewise(input);
        CHECK(whole.text() == "pink caf\xC3\xA9 \xE2\x9C\x93\ndone\n");
        CHECK(bytes.text() == whole.text());
        CHECK(isIndexed(styleOf(bytes, "pink")->fg, 213));
        CHECK(bytes.lastStatus == 7 && bytes.lastDir == "/tmp");
        // Half a UTF-8 character is held back, not shown as garbage.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("x\xE2\x9C"));
        CHECK(p.text() == "x");
        p.apply(ts.feed("\x93"));
        CHECK(p.text() == "x\xE2\x9C\x93");
    }

    GROUP("term:invalid-utf8");
    {
        CHECK(run("a\xFF" "b\xC0\xAF" "c\n").text() ==
              "a\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD" "c\n");
        // A sequence cut short by ESC or ASCII becomes one replacement.
        CHECK(run("\xE2\x9C" "a\xE2\x1b[31mb\n").text() ==
              "\xEF\xBF\xBD" "a\xEF\xBF\xBD" "b\n");
        CHECK(run("\xED\xA0\x80!\n").text() == "\xEF\xBF\xBD!\n");  // surrogate
    }

    GROUP("term:shell-integration");
    {
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;A\x07"));
        CHECK(p.events == "A");
        p.apply(ts.feed("\x1b]133;C\x07out\r\npartial\x1b]133;D;130\x07"
                        "\x1b]7;file://Host.local/Users/me/my%20dir%25\x07\x1b]133;A\x07"));
        CHECK(p.events == "ACLlDPA");    // the live line is sent before D
        CHECK(p.text() == "out\npartial");
        CHECK(p.lastStatus == 130);
        CHECK(p.lastDir == "/Users/me/my dir%");
        // D without a status, and ST (ESC \) instead of BEL as terminator.
        auto noStatus = ts.feed("\x1b]133;D\x1b\\");
        CHECK(noStatus.size() == 1 && noStatus[0].kind == TermEvent::CommandEnd &&
              noStatus[0].status == 0);
        CHECK(ts.feed("\x1b]133;B\x07").empty());   // recognized, ignored
        CHECK(ts.feed("\x1b]7;file://nohostpath\x07").empty());
    }

    GROUP("term:robustness");
    {
        // An OSC that never terminates is abandoned; output resumes after its
        // eventual terminator instead of being swallowed forever.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;" + std::string(20000, 'x')));
        CHECK(p.text().empty());
        p.apply(ts.feed("\x07visible"));
        CHECK(p.text() == "visible");
        // ESC inside an OSC starts a new sequence.
        CHECK(run("\x1b]0;title\x1b]133;A\x07").events == "A");
        // An overlong CSI is ignored whole, not half-applied.
        std::string longCsi = "\x1b[" + std::string(200, '1') + "mz";
        Panel lc = run(longCsi);
        CHECK(lc.text() == "z" && *styleOf(lc, "z") == TermStyle());
        // Other control characters are dropped.
        CHECK(run("a\x07\x01" "b").text() == "ab");
    }
}

}  // namespace

// ------------------------------------------------------- terminal screen
namespace {

// Real output, recorded with `script -q` from programs on a 40x10 terminal
// with TERM=xterm-256color.
//
// vim -u NONE -N -i NONE f.txt (f.txt = "line one\nline two\n"), typed:
// "iHello <Esc>:wq<Enter>".
const std::string kVimEdit =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"f.txt\" 2L, 18B\033["
    "2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033[0%m\033"
    "[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hline one\r\n"
    "line two\033[2;9H\033[K\033[3;1H\033[94m~                        "
    "               \033[4;1H~                                       \033"
    "[5;1H~                                       \033[6;1H~          "
    "                             \033[7;1H~                          "
    "             \033[8;1H~                                       \033"
    "[9;1H~                                       \033[1;1H\033[\?25h\033"
    "[\?4m\033[\?25l\033[m\033[10;1H\033[1m-- INSERT --\033[m\033[10;1"
    "3H\033[K\033[10;1H\033[K\033[1;6H\rHello line one\033[10;1H\033[1"
    "m-- INSERT --\033[1;7H\033[m\033[10;30H^[\033[1;6H\033[\?25h\033["
    "\?25l\033[10;30H  \033[1;7H\033[10;1H\033[K\033[1;6H\033[\?25h\033"
    "[\?25l\033[10;1H:wq\r\033[\?2004l\033[>4;m\"f.txt\" 2L, 24B writt"
    "en\r\033[23;2t\033[23;1t\r\r\n"
    "\033[\?1004l\033[\?2004l\033[\?1l\033>\033[\?1049l\033[\?25h\033["
    ">4;m";

// vim on a 100-line file ("row 1".."row 100"), typed: Ctrl+E three times,
// "dd", "Onew<Esc>", ":q!<Enter>". Scrolls with a scroll region and redraws
// lines by overwriting only the characters that differ.
const std::string kVimScroll =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"long2.txt\" 100L, 69"
    "2B\033[2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033"
    "[0%m\033[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hrow 1\r\n"
    "row 2\033[2;6H\033[K\033[3;1Hrow 3\033[3;6H\033[K\033[4;1Hrow 4\r"
    "\n"
    "row 5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[1;1H\033[\?25h\033[\?4m\033[\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 10\033[10;1H\033[K\033[1;1H\033[\?25h\033["
    "\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 11\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "^E\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 12\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "dd\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 13\033[1;1H\033[\?25h\033[\?25l\033[10;1H\033"
    "[1m-- INSERT --\033[m\033[10;1H\033[K\033[1;3H\rne\033[1;5H\033[K"
    "\033[2;5H5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[6;6H\033[K\033[7;6H0\033[8;6H1\033[9;6H2\r\n"
    "\033[1m-- INSERT --\033[1;4H\033[m\033[10;30H^[\033[1;3H\033[\?25"
    "h\033[\?25l\033[10;30H  \033[1;4H\033[10;1H\033[K\033[1;3H\033[\?"
    "25h\033[\?25l\033[10;1H:q!\r\033[\?2004l\033[>4;m\033[23;2t\033[2"
    "3;1t\033[10;1H\033[K\033[10;1H\033[\?1004l\033[\?2004l\033[\?1l\033"
    ">\033[\?1049l\033[\?25h\033[>4;m";

// less on the same 100-line file, typed: Space, q.
const std::string kLess =
    "\033[\?1049h\033[10;1H\033[\?1h\033=\rrow 1\r\n"
    "row 2\r\nrow 3\r\nrow 4\r\nrow 5\r\nrow 6\r\nrow 7\r\nrow 8\r\nrow 9\r\n"
    "\033[7mlong.txt\033[27m\033[K\r\033[Krow 10\r\n"
    "row 11\r\nrow 12\r\nrow 13\r\nrow 14\r\nrow 15\r\nrow 16\r\nrow 17\r\n"
    "row 18\r\n"
    ":\033[K\r\033[K\033[\?1l\033>\033[\?1049l";

// Feed `data` up to (not including) the first `marker`.
void feedUntil(TerminalScreen &s, const std::string &data, const std::string &marker) {
    size_t at = data.find(marker);
    s.feed(data.substr(0, at));
}

// Rows [from, to) as text, one string per row.
std::vector<std::string> rowsOf(const TerminalScreen &s, int from, int to) {
    std::vector<std::string> out;
    for (int r = from; r < to; r++) out.push_back(s.rowText(r));
    return out;
}

std::vector<std::string> numberedRows(const std::string &prefix, int first, int last) {
    std::vector<std::string> out;
    for (int i = first; i <= last; i++) out.push_back(prefix + std::to_string(i));
    return out;
}

TerminalScreen screenAfter(const std::string &bytes, int cols = 10, int rows = 4) {
    TerminalScreen s(cols, rows);
    s.feed(bytes);
    return s;
}

void testTerminalScreen() {
    GROUP("screen:vim-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimEdit, "\033[10;1H:wq");
        CHECK(s.altScreen());
        CHECK(s.appCursorKeys() && s.appKeypad() && s.bracketedPaste());
        CHECK(s.rowText(0) == "Hello line one");
        CHECK(s.rowText(1) == "line two");
        bool tildes = true;
        for (int r = 2; r <= 8; r++) tildes = tildes && s.rowText(r) == "~";
        CHECK(tildes);
        CHECK(s.rowText(9) == "");             // "-- INSERT --" was erased
        CHECK(isIndexed(s.cell(2, 0).style.fg, 12));   // ~ in bright blue
        CHECK(s.cell(0, 0).style == TermStyle());
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 5);
        // vim asked where the cursor was (after an ambiguous-width probe).
        CHECK(s.takeReplies() == "\033[2;2R\033[3;1R");
        CHECK(s.takeReplies().empty());
        // "\e[0%m" and "\e[>4;2m" are not SGR: nothing turned bold or odd.
        CHECK(!s.cell(0, 0).style.bold && !s.cell(0, 0).style.underline);
        s.feed(kVimEdit.substr(kVimEdit.find("\033[10;1H:wq")));
        CHECK(!s.altScreen());
        CHECK(!s.appCursorKeys() && !s.appKeypad() && !s.bracketedPaste());
        CHECK(s.cursorVisible());
        CHECK(s.text().empty());               // the shell's screen, untouched
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
    }

    GROUP("screen:vim-scroll-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimScroll, "\033[10;30Hdd");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 4, 12));   // three Ctrl+E
        CHECK(s.scrollTop() == 0 && s.scrollBottom() == 9);
        feedUntil(s, kVimScroll.substr(kVimScroll.find("\033[10;30Hdd")),
                  "\033[1m-- INSERT --");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 5, 13));   // dd
        TerminalScreen whole(40, 10);
        feedUntil(whole, kVimScroll, "\033[10;1H:q!");
        std::vector<std::string> want = {"new"};
        for (const std::string &r : numberedRows("row ", 5, 12)) want.push_back(r);
        CHECK(rowsOf(whole, 0, 9) == want);                      // Onew<Esc>
        CHECK(whole.cursorRow() == 0 && whole.cursorCol() == 2);
        // Byte at a time gives the same screen.
        TerminalScreen bytes(40, 10);
        std::string upTo = kVimScroll.substr(0, kVimScroll.find("\033[10;1H:q!"));
        for (char c : upTo) bytes.feed(&c, 1);
        CHECK(bytes.text() == whole.text());
        whole.feed(kVimScroll.substr(kVimScroll.find("\033[10;1H:q!")));
        CHECK(!whole.altScreen() && whole.text().empty());
    }

    GROUP("screen:less-capture");
    {
        TerminalScreen s(40, 10);
        s.feed("$ ls\r\nhello\r\n$ ");                 // on the main screen first
        feedUntil(s, kLess, "\033[\?1049l");
        CHECK(s.altScreen());
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 10, 18));
        s.feed("\033[\?1049l");
        CHECK(!s.altScreen());
        CHECK(s.text() == "$ ls\nhello\n$");         // main screen restored
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        // The log model keeps less's pages out of the scrollback.
        TerminalStream ts;
        std::string logged;
        for (const TermEvent &e : ts.feed("before\r\n" + kLess + "after\r\n"))
            if (e.kind == TermEvent::Line)
                for (const TermRun &r : e.runs) logged += r.text + (e.ended ? "|" : "");
        CHECK(logged.find("row") == std::string::npos);
        CHECK(logged.find("before") != std::string::npos &&
              logged.find("after") != std::string::npos);
        CHECK(!ts.altScreen());
    }

    GROUP("screen:cursor-addressing");
    {
        TerminalScreen s(10, 5);
        s.feed("\033[3;4HX");                  // CUP is 1-based
        CHECK(s.rowText(2) == "   X" && s.cursorRow() == 2 && s.cursorCol() == 4);
        s.feed("\033[2AY");                    // CUU
        CHECK(s.rowText(0) == "    Y");
        s.feed("\033[9BZ");                    // CUD clamps to the last row
        CHECK(s.rowText(4) == "     Z");
        s.feed("\033[3D!");                    // CUB
        CHECK(s.rowText(4) == "   ! Z");
        s.feed("\033[20C#");                   // CUF clamps to the last column
        CHECK(s.cell(4, 9).ch == "#");
        s.feed("\033[1G<\033[2d^");            // CHA, VPA
        CHECK(s.rowText(4)[0] == '<' && s.rowText(1) == " ^");
        s.feed("\033[H\033[2E.\033[F,");       // CNL, CPL move to column 0
        CHECK(s.rowText(2) == ".  X" && s.rowText(1) == ",^");
        s.feed("\033[;5H*");                   // empty parameter = default
        CHECK(s.rowText(0) == "    *");
        s.feed("\033[99;99H");
        CHECK(s.cursorRow() == 4 && s.cursorCol() == 9);
    }

    GROUP("screen:erase");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[2;5H");
        TerminalScreen el0 = s; el0.feed("\033[K");
        CHECK(el0.rowText(1) == "klmn");
        TerminalScreen el1 = s; el1.feed("\033[1K");
        CHECK(el1.rowText(1) == "     pqrst");
        TerminalScreen el2 = s; el2.feed("\033[2K");
        CHECK(el2.rowText(1) == "" && el2.rowText(0) == "abcdefghij");
        TerminalScreen ed0 = s; ed0.feed("\033[J");
        CHECK(ed0.text() == "abcdefghij\nklmn");
        TerminalScreen ed1 = s; ed1.feed("\033[1J");
        CHECK(ed1.text() == "\n     pqrst\nuvwxyz");
        TerminalScreen ed2 = s; ed2.feed("\033[2J");
        CHECK(ed2.text().empty() && ed2.cursorRow() == 1 && ed2.cursorCol() == 4);
        TerminalScreen ech = s; ech.feed("\033[3X");
        CHECK(ech.rowText(1) == "klmn   rst" && ech.cursorCol() == 4);
        // Erase paints the current background (xterm's bce).
        TerminalScreen bce = s; bce.feed("\033[44m\033[K");
        CHECK(isIndexed(bce.cell(1, 7).style.bg, 4) && bce.cell(1, 7).ch == " ");
        CHECK(bce.cell(1, 3).style.bg.kind == TermColor::Default);
    }

    GROUP("screen:insert-delete");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[1;3H");
        TerminalScreen ich = s; ich.feed("\033[2@");
        CHECK(ich.rowText(0) == "ab  cdefgh");
        TerminalScreen dch = s; dch.feed("\033[3P");
        CHECK(dch.rowText(0) == "abfghij");
        TerminalScreen il = s; il.feed("\033[2;5H\033[L");
        CHECK(il.text() == "abcdefghij\n\nklmnopqrst\nuvwxyz");
        CHECK(il.cursorCol() == 0);
        TerminalScreen dl = s; dl.feed("\033[M");
        CHECK(dl.text() == "klmnopqrst\nuvwxyz");
        TerminalScreen dl9 = s; dl9.feed("\033[2;1H\033[9M");
        CHECK(dl9.text() == "abcdefghij");
        // Insert mode (IRM) pushes the rest of the line right.
        TerminalScreen irm = s; irm.feed("\033[4hXY\033[4lZ");
        CHECK(irm.rowText(0) == "abXYZdefgh");
    }

    GROUP("screen:scroll-region");
    {
        // Rows 2-4 scroll; rows 1 and 5 stay put.
        auto s = screenAfter("top\r\na\r\nb\r\nc\r\nbottom\033[2;4r", 10, 5);
        CHECK(s.scrollTop() == 1 && s.scrollBottom() == 3);
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);   // DECSTBM homes
        TerminalScreen lf = s; lf.feed("\033[4;1H\nd");
        CHECK(lf.text() == "top\nb\nc\nd\nbottom");
        TerminalScreen ind = s; ind.feed("\033[4;1H\033D");
        CHECK(ind.text() == "top\nb\nc\n\nbottom");
        TerminalScreen ri = s; ri.feed("\033[2;1H\033M");
        CHECK(ri.text() == "top\n\na\nb\nbottom");
        TerminalScreen su = s; su.feed("\033[2S");
        CHECK(su.text() == "top\nc\n\n\nbottom");
        TerminalScreen sd = s; sd.feed("\033[T");
        CHECK(sd.text() == "top\n\na\nb\nbottom");
        // IL/DL act inside the region only.
        TerminalScreen il = s; il.feed("\033[3;1H\033[L");
        CHECK(il.text() == "top\na\n\nb\nbottom");
        TerminalScreen outside = s; outside.feed("\033[5;1H\033[L");
        CHECK(outside.text() == "top\na\nb\nc\nbottom");
        // CUU/CUD stop at the margins when starting inside the region.
        TerminalScreen cuu = s; cuu.feed("\033[3;1H\033[9A");
        CHECK(cuu.cursorRow() == 1);
        TerminalScreen cud = s; cud.feed("\033[3;1H\033[9B");
        CHECK(cud.cursorRow() == 3);
        // An invalid region is ignored; no parameters resets it.
        TerminalScreen bad = s; bad.feed("\033[4;2r");
        CHECK(bad.scrollTop() == 1 && bad.scrollBottom() == 3);
        TerminalScreen reset = s; reset.feed("\033[r");
        CHECK(reset.scrollTop() == 0 && reset.scrollBottom() == 4);
        // Origin mode addresses rows from the top margin.
        TerminalScreen om = s; om.feed("\033[\?6h\033[1;1HX\033[9;1H");
        CHECK(om.rowText(1) == "X" && om.cursorRow() == 3);
    }

    GROUP("screen:alternate");
    {
        auto s = screenAfter("main\r\ntext\033[31m", 10, 4);
        TerminalScreen a = s;
        a.feed("\033[\?1049h");
        CHECK(a.altScreen() && a.text().empty());
        a.feed("\033[32m\033[3;3Halt");
        a.feed("\033[\?1049l");
        CHECK(!a.altScreen() && a.text() == "main\ntext");
        CHECK(a.cursorRow() == 1 && a.cursorCol() == 4);   // cursor restored
        a.feed("x");
        CHECK(isIndexed(a.cell(1, 4).style.fg, 1));          // and its style
        // 1049 always starts with a clear alternate screen.
        a.feed("\033[\?1049h");
        CHECK(a.text().empty());
        // 47 switches without saving the cursor or clearing.
        TerminalScreen b = s;
        b.feed("\033[\?47hAB\033[\?47l");
        CHECK(b.text() == "main\ntext" && b.cursorCol() == 6);
        b.feed("\033[\?47h");
        CHECK(b.text() == "\n    AB");                // kept from last time
        // 1047 clears the alternate screen when leaving it.
        TerminalScreen c = s;
        c.feed("\033[\?1047hAB\033[\?1047l\033[\?1047h");
        CHECK(c.text().empty());
    }

    GROUP("screen:save-restore");
    {
        TerminalScreen s(10, 4);
        s.feed("\033[2;3H\033[1;33m\0337\033[H\033[0mx\0338y");
        CHECK(s.rowText(1) == "  y" && s.cell(1, 2).style.bold &&
              isIndexed(s.cell(1, 2).style.fg, 3));
        CHECK(s.cell(0, 0).style == TermStyle());
        s.feed("\033[4;4H\033[s\033[H\033[uz");        // SCOSC / SCORC
        CHECK(s.rowText(3) == "   z");
        // Restoring with nothing saved goes home.
        TerminalScreen fresh(10, 4);
        fresh.feed("\033[3;3H\0338");
        CHECK(fresh.cursorRow() == 0 && fresh.cursorCol() == 0);
    }

    GROUP("screen:autowrap");
    {
        TerminalScreen s(5, 3);
        s.feed("abcde");
        // The cursor waits on the last column: the wrap is pending.
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 4 && s.rowText(1) == "");
        TerminalScreen cr = s; cr.feed("\rX");
        CHECK(cr.rowText(0) == "Xbcde" && cr.rowText(1) == "");
        TerminalScreen more = s; more.feed("f");
        CHECK(more.rowText(0) == "abcde" && more.rowText(1) == "f");
        TerminalScreen lf = s; lf.feed("\r\nz");        // no blank line between
        CHECK(lf.text() == "abcde\nz");
        TerminalScreen el = s; el.feed("\033[K!");      // erase clears the pending wrap
        CHECK(el.rowText(0) == "abcd!" && el.rowText(1) == "");
        // Wrapping at the bottom scrolls.
        TerminalScreen bottom(5, 2);
        bottom.feed("12345678901234");
        CHECK(bottom.text() == "67890\n1234");
        // With autowrap off, the last column is overwritten.
        TerminalScreen off(5, 2);
        off.feed("\033[\?7labcdefg");
        CHECK(off.text() == "abcdg" && !off.autowrap());
    }

    GROUP("screen:tabs");
    {
        TerminalScreen s(20, 2);
        s.feed("a\tb\tc\td");
        CHECK(s.rowText(0) == "a       b       c  d");   // last stop clamps
        TerminalScreen h(20, 2);
        h.feed("\033[3g\033[4G\033H\033[1G\tx");        // clear all, set at col 4
        CHECK(h.rowText(0) == "   x");
        TerminalScreen cbt(20, 2);
        cbt.feed("\033[18G\033[Zy\033[2Zz");
        CHECK(cbt.rowText(0) == "        z       y");
        TerminalScreen cht(20, 2);
        cht.feed("\033[2Iq");
        CHECK(cht.cursorCol() == 17);
    }

    GROUP("screen:wide-chars");
    {
        TerminalScreen s(6, 2);
        s.feed("\xE4\xB8\xAD\xE6\x96\x87!");                  // 中文!
        CHECK(s.cell(0, 0).width == 2 && s.cell(0, 1).width == 0);
        CHECK(s.cell(0, 2).ch == "\xE6\x96\x87" && s.cell(0, 4).ch == "!");
        CHECK(s.cursorCol() == 5 && s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!");
        // A wide character that does not fit wraps whole.
        s.feed("\xE5\xAD\x97");                               // 字
        CHECK(s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!" &&
              s.rowText(1) == "\xE5\xAD\x97");
        // Overwriting half of a wide character blanks the other half.
        TerminalScreen o(6, 2);
        o.feed("\xE4\xB8\xAD\xE6\x96\x87\033[1;2Hx");
        CHECK(o.rowText(0) == " x\xE6\x96\x87");
        // Emoji are wide, combining marks join the previous cell.
        TerminalScreen e(6, 2);
        e.feed("\xF0\x9F\x98\x80" "e\xCC\x81.");
        CHECK(e.cell(0, 0).width == 2 && e.cell(0, 2).ch == "e\xCC\x81" &&
              e.cell(0, 3).ch == ".");
        CHECK(termCharWidth(0x4E2D) == 2 && termCharWidth('a') == 1 &&
              termCharWidth(0x0301) == 0 && termCharWidth(0x2713) == 1);
    }

    GROUP("screen:line-drawing");
    {
        TerminalScreen s(10, 2);
        s.feed("\033(0lqk\033(Bq\016x\017x");
        CHECK(s.rowText(0) == "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90qxx");
        TerminalScreen g1(10, 2);
        g1.feed("\033)0a\016q\017q");
        CHECK(g1.rowText(0) == "a\xE2\x94\x80q");
        // REP repeats the last character.
        TerminalScreen rep(10, 2);
        rep.feed("-\033[4b|");
        CHECK(rep.rowText(0) == "-----|");
    }

    GROUP("screen:modes-and-replies");
    {
        TerminalScreen s(10, 4);
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.bracketedPaste());
        s.feed("\033[\?25l\033[\?1h\033=\033[\?2004h");
        CHECK(!s.cursorVisible() && s.appCursorKeys() && s.appKeypad() &&
              s.bracketedPaste());
        s.feed("\033[\?1;2004l\033>\033[\?25h");        // several at once
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.appKeypad() &&
              !s.bracketedPaste());
        s.feed("\033[3;7H\033[6n\033[5n\033[c\033[>c\033[\?6n");
        CHECK(s.takeReplies() == "\033[3;7R\033[0n\033[\?1;2c");
        s.feed("\033]0;my title\007");
        CHECK(s.title() == "my title");
        // Private and intermediate sequences never reach SGR.
        s.feed("\033[>4;2m\033[\?4m\033[0%m\033[2 qA");
        CHECK(s.cell(2, 6).style == TermStyle() && s.cell(2, 6).ch == "A");
        // Full reset.
        s.feed("\033[31m\033[\?1049h\033c");
        CHECK(!s.altScreen() && s.text().empty() && s.cursorRow() == 0);
    }

    GROUP("screen:resize");
    {
        TerminalScreen s(10, 4);
        s.feed("one\r\ntwo\r\nthree\r\nfour");
        s.resize(10, 2);                     // the cursor's line stays on screen
        CHECK(s.text() == "three\nfour" && s.cursorRow() == 1 && s.cursorCol() == 4);
        s.resize(3, 3);                      // columns are cut, rows added below
        CHECK(s.text() == "thr\nfou" && s.cursorCol() == 2 && s.rows() == 3);
        s.resize(6, 3);
        CHECK(s.cols() == 6 && s.rowText(0) == "thr");
        CHECK(s.scrollBottom() == 2);
        // A wide character split by the new edge becomes a blank.
        TerminalScreen w(4, 1);
        w.feed("a\xE4\xB8\xAD");
        w.resize(2, 1);
        CHECK(w.rowText(0) == "a");
        // The alternate screen resizes too.
        TerminalScreen alt(10, 4);
        alt.feed("\033[\?1049h\033[4;1Hlast");
        alt.resize(8, 2);
        CHECK(alt.altScreen() && alt.rowText(1) == "last");
    }

    GROUP("screen:selection-text");
    {
        auto s = screenAfter("abc\r\ndefgh\r\nij", 10, 3);
        CHECK(s.textBetween(0, 1, 1, 2) == "bc\ndef");
        CHECK(s.textBetween(1, 2, 0, 1) == "bc\ndef");     // either direction
        CHECK(s.textBetween(2, 0, 2, 9) == "ij");
        CHECK(s.textBetween(0, 0, 2, 9) == "abc\ndefgh\nij");
    }

    GROUP("screen:keys");
    {
        using K = TermKey;
        CHECK(TerminalScreen::encodeKey(K::Up, 0, false) == "\033[A");
        CHECK(TerminalScreen::encodeKey(K::Up, 0, true) == "\033OA");     // DECCKM
        CHECK(TerminalScreen::encodeKey(K::Left, 0, true) == "\033OD");
        CHECK(TerminalScreen::encodeKey(K::Home, 0, false) == "\033[H");
        CHECK(TerminalScreen::encodeKey(K::End, 0, true) == "\033OF");
        CHECK(TerminalScreen::encodeKey(K::Right, TermModCtrl, true) == "\033[1;5C");
        CHECK(TerminalScreen::encodeKey(K::Down, TermModShift | TermModAlt, false) ==
              "\033[1;4B");
        CHECK(TerminalScreen::encodeKey(K::PageUp, 0, false) == "\033[5~");
        CHECK(TerminalScreen::encodeKey(K::PageDown, TermModShift, false) == "\033[6;2~");
        CHECK(TerminalScreen::encodeKey(K::Delete, 0, false) == "\033[3~");
        CHECK(TerminalScreen::encodeKey(K::F1, 0, false) == "\033OP");
        CHECK(TerminalScreen::encodeKey(K::F4, TermModCtrl, false) == "\033[1;5S");
        CHECK(TerminalScreen::encodeKey(K::F5, 0, false) == "\033[15~");
        CHECK(TerminalScreen::encodeKey(K::F12, 0, false) == "\033[24~");
        CHECK(TerminalScreen::encodeKey(K::Enter, 0, false) == "\r");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, true) == "\033OM");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, false) == "\r");
        CHECK(TerminalScreen::encodeKey(K::Backspace, 0, false) == "\x7f");
        CHECK(TerminalScreen::encodeKey(K::Backspace, TermModAlt, false) == "\033\x7f");
        CHECK(TerminalScreen::encodeKey(K::Tab, 0, false) == "\t");
        CHECK(TerminalScreen::encodeKey(K::Tab, TermModShift, false) == "\033[Z");
        CHECK(TerminalScreen::encodeKey(K::Escape, 0, false) == "\033");

        CHECK(TerminalScreen::encodeChar('c', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('C', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('[', TermModCtrl) == "\033");
        CHECK(TerminalScreen::encodeChar(' ', TermModCtrl) == std::string(1, '\0'));
        CHECK(TerminalScreen::encodeChar('/', TermModCtrl) == "\x1f");
        CHECK(TerminalScreen::encodeChar('b', TermModAlt) == "\033b");  // Meta
        CHECK(TerminalScreen::encodeChar('x', TermModAlt | TermModCtrl) == "\033\x18");
        CHECK(TerminalScreen::encodeChar(0xE9, 0) == "\xC3\xA9");
        CHECK(TerminalScreen::encodePaste("a\nb\r\nc", false) == "a\rb\rc");
        CHECK(TerminalScreen::encodePaste("x", true) == "\033[200~x\033[201~");
        CHECK(TerminalScreen::encodePaste("x\033[201~y", true) == "\033[200~xy\033[201~");
    }
}

}  // namespace

// --------------------------------------------------------------- settings
namespace {

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

bool colorIs(const Rgba &c, uint32_t rgb, double alpha = 1.0) {
    return c.rgb() == rgb && near(c.a, alpha);
}

Settings parseSettings(const std::string &text,
                       std::vector<SettingsError> *errors = nullptr) {
    return Settings::parse(text, errors);
}

// Every line of `text` that reads "# key = value" with the "# " removed, so
// the documented keys in the default file can be checked against the parser.
std::string uncommentKeys(const std::string &text) {
    std::string out, line;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i < text.size() && text[i] != '\n') { line += text[i]; continue; }
        // "# group.field = value", not prose that happens to contain " = ".
        size_t eq = line.find(" = ");
        std::string key = eq == std::string::npos ? "" : line.substr(2, eq - 2);
        bool isKey = line.rfind("# ", 0) == 0 && key.find('.') != std::string::npos;
        for (char ch : key)
            if (!(ch == '.' || (ch >= 'a' && ch <= 'z'))) isKey = false;
        if (isKey) out += line.substr(2) + "\n";
        line.clear();
    }
    return out;
}

void testSettings() {
    GROUP("settings:colors");
    Rgba c;
    CHECK(Settings::parseColor("#1E1E1E", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#1e1e1e", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#fff", c) && colorIs(c, 0xFFFFFF));
    CHECK(Settings::parseColor("#0A0", c) && colorIs(c, 0x00AA00));
    CHECK(Settings::parseColor("#00000080", c) && colorIs(c, 0x000000, 128 / 255.0));
    CHECK(Settings::parseColor("#FFFFFF00", c) && near(c.a, 0));
    CHECK(!Settings::parseColor("1E1E1E", c));      // no #
    CHECK(!Settings::parseColor("#12", c));
    CHECK(!Settings::parseColor("#12345", c));
    CHECK(!Settings::parseColor("#1234567", c));
    CHECK(!Settings::parseColor("#GGGGGG", c));
    CHECK(!Settings::parseColor("#", c));
    CHECK(!Settings::parseColor("", c));
    CHECK(!Settings::parseColor("red", c));

    GROUP("settings:opacity");
    double o = -1;
    CHECK(Settings::parseOpacity("0.5", o) && near(o, 0.5));
    CHECK(Settings::parseOpacity(".25", o) && near(o, 0.25));
    CHECK(Settings::parseOpacity("1", o) && near(o, 1));
    CHECK(Settings::parseOpacity("0", o) && near(o, 0));
    CHECK(Settings::parseOpacity("80%", o) && near(o, 0.8));
    CHECK(Settings::parseOpacity("100%", o) && near(o, 1));
    CHECK(Settings::parseOpacity("12.5%", o) && near(o, 0.125));
    CHECK(!Settings::parseOpacity("1.5", o));
    CHECK(!Settings::parseOpacity("80", o));        // a bare 80 is not 80%
    CHECK(!Settings::parseOpacity("-0.1", o));
    CHECK(!Settings::parseOpacity("101%", o));
    CHECK(!Settings::parseOpacity("%", o));
    CHECK(!Settings::parseOpacity("half", o));
    CHECK(!Settings::parseOpacity("0.5.0", o));
    CHECK(!Settings::parseOpacity("", o));

    GROUP("settings:defaults");
    Settings d = parseSettings("");
    CHECK(colorIs(d.background(Surface::Editor), 0x1E1E1E));
    CHECK(colorIs(d.background(Surface::Sidebar), 0x252526));
    CHECK(colorIs(d.background(Surface::Terminal), 0x181818));
    CHECK(colorIs(d.background(Surface::Statusbar), 0x007ACC));
    CHECK(colorIs(d.background(Surface::Browser), 0x2A2A2A));
    CHECK(colorIs(d.text(Surface::Editor), 0xD4D4D4));
    CHECK(colorIs(d.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(colorIs(d.text(Surface::Statusbar), 0xFFFFFF));
    CHECK(colorIs(d.syntax(TokenStyle::Keyword), 0x569CD6));
    CHECK(colorIs(d.syntax(TokenStyle::Comment), 0x6A9955));
    CHECK(colorIs(d.syntax(TokenStyle::Plain), 0xD4D4D4));
    CHECK(colorIs(d.markdown(MarkdownColor::Link), 0x4EA1F7));
    CHECK(colorIs(d.markdownCodeBackground(), 0x2A2A2A));
    CHECK(colorIs(d.terminalInputBackground(), 0x232323));
    CHECK(colorIs(d.terminalInputText(), 0xEDEDED));
    CHECK(near(d.opacity(Surface::Editor), 1));
    CHECK(!d.blur());
    CHECK(d.material() == "under-window");
    CHECK(d.windowIsOpaque());
    CHECK(!d.customTitlebar());
    CHECK(!d.textIsSet(Surface::Browser));
    CHECK(std::string(Settings::surfaceName(Surface::Statusbar)) == "statusbar");

    GROUP("settings:panel-opacity");
    Settings p = parseSettings("editor.opacity = 0.6\nsidebar.opacity = 40%\n");
    CHECK(colorIs(p.background(Surface::Editor), 0x1E1E1E, 0.6));
    CHECK(colorIs(p.background(Surface::Sidebar), 0x252526, 0.4));
    CHECK(colorIs(p.background(Surface::Terminal), 0x181818, 1));  // untouched
    CHECK(colorIs(p.text(Surface::Editor), 0xD4D4D4, 1));          // text stays solid
    CHECK(near(p.markdownCodeBackground().a, 0.6));                // follows editor
    CHECK(near(p.terminalInputBackground().a, 1));
    CHECK(!p.windowIsOpaque());
    CHECK(p.customTitlebar());   // a see-through window draws its own title bar
    CHECK(near(p.background(Surface::Titlebar).a, 1));

    GROUP("settings:window-opacity");
    Settings w = parseSettings("window.opacity = 0.5\nterminal.opacity = 0.9\n");
    CHECK(near(w.opacity(Surface::Editor), 0.5));
    CHECK(near(w.opacity(Surface::Titlebar), 0.5));
    CHECK(near(w.opacity(Surface::Statusbar), 0.5));
    CHECK(near(w.opacity(Surface::Terminal), 0.9));      // its own wins
    CHECK(near(w.terminalInputBackground().a, 0.9));
    // Order doesn't matter: the panel's own opacity wins either way.
    Settings w2 = parseSettings("terminal.opacity = 0.9\nwindow.opacity = 0.5\n");
    CHECK(near(w2.opacity(Surface::Terminal), 0.9));
    // A panel can be fully opaque inside a see-through window.
    Settings w3 = parseSettings("window.opacity = 0.3\neditor.opacity = 1\n");
    CHECK(near(w3.background(Surface::Editor).a, 1));
    CHECK(near(w3.background(Surface::Sidebar).a, 0.3));

    GROUP("settings:background-alpha");
    // A color's own alpha multiplies with the opacity.
    Settings ba = parseSettings("editor.background = #10203080\n"
                                "editor.opacity = 0.5\n");
    CHECK(ba.background(Surface::Editor).rgb() == 0x102030);
    CHECK(near(ba.background(Surface::Editor).a, (128 / 255.0) * 0.5));
    Settings bb = parseSettings("statusbar.background = #333\n");
    CHECK(colorIs(bb.background(Surface::Statusbar), 0x333333));
    CHECK(bb.windowIsOpaque());
    Settings bc = parseSettings("sidebar.background = #25252600\n");
    CHECK(!bc.windowIsOpaque());

    GROUP("settings:text");
    Settings t = parseSettings("window.text = #EEEEEE\neditor.text = #00FF00\n"
                               "sidebar.text = #FF000080\n");
    CHECK(colorIs(t.text(Surface::Editor), 0x00FF00));
    CHECK(colorIs(t.text(Surface::Sidebar), 0xFF0000, 128 / 255.0));
    CHECK(colorIs(t.text(Surface::Terminal), 0xEEEEEE));   // window.text fallback
    CHECK(colorIs(t.text(Surface::Statusbar), 0xEEEEEE));
    CHECK(colorIs(t.syntax(TokenStyle::Plain), 0x00FF00)); // plain = editor.text
    CHECK(colorIs(t.terminalInputText(), 0xEEEEEE));
    CHECK(t.textIsSet(Surface::Browser));
    CHECK(t.windowIsOpaque());          // text alone never makes it see-through
    CHECK(!t.customTitlebar());
    Settings t2 = parseSettings("browser.text = #111111\n");
    CHECK(t2.textIsSet(Surface::Browser));
    CHECK(!t2.textIsSet(Surface::Editor));

    GROUP("settings:syntax-markdown");
    Settings sy = parseSettings("syntax.keyword = #FF0000\nsyntax.function = #00F\n"
                                "markdown.heading = #ABCDEF\nmarkdown.quote = #123\n");
    CHECK(colorIs(sy.syntax(TokenStyle::Keyword), 0xFF0000));
    CHECK(colorIs(sy.syntax(TokenStyle::Function), 0x0000FF));
    CHECK(colorIs(sy.syntax(TokenStyle::String), 0xCE9178));   // default kept
    CHECK(colorIs(sy.markdown(MarkdownColor::Heading), 0xABCDEF));
    CHECK(colorIs(sy.markdown(MarkdownColor::Quote), 0x112233));
    CHECK(colorIs(sy.markdown(MarkdownColor::Code), 0xCE9178));
    std::vector<SettingsError> se;
    parseSettings("syntax.plain = #FFFFFF\n", &se);   // plain is editor.text
    CHECK(se.size() == 1);

    GROUP("settings:window");
    Settings bl = parseSettings("window.blur = true\nwindow.material = HUD\n");
    CHECK(bl.blur());
    CHECK(bl.material() == "hud");
    CHECK(!bl.windowIsOpaque());
    CHECK(bl.customTitlebar());
    for (const char *v : {"on", "yes", "1", "TRUE"})
        CHECK(parseSettings(std::string("window.blur = ") + v).blur());
    for (const char *v : {"off", "no", "0", "false"})
        CHECK(!parseSettings(std::string("window.blur = true\nwindow.blur = ") + v).blur());
    Settings tb = parseSettings("titlebar.background = #000000\n");
    CHECK(tb.customTitlebar());          // any titlebar key takes it over
    CHECK(tb.windowIsOpaque());
    CHECK(colorIs(tb.background(Surface::Titlebar), 0x000000));
    Settings tb2 = parseSettings("titlebar.opacity = 0\n");
    CHECK(tb2.customTitlebar());
    CHECK(!tb2.windowIsOpaque());        // the title bar counts once it's ours

    GROUP("settings:syntax-of-file");
    std::vector<SettingsError> e1;
    Settings f = parseSettings(
        "# a comment\n"
        "   \n"
        "  # indented comment\n"
        "EDITOR.Opacity=0.5\n"                      // case, no spaces
        "sidebar.opacity =\t25%   # trailing comment\n"
        "statusbar.background = #FF0000#not-a-comment-space\n"
        "terminal.text = #00FF00\r\n"               // CRLF
        "editor.opacity = 0.7", &e1);               // no final newline
    CHECK(near(f.opacity(Surface::Editor), 0.7));   // last one wins
    CHECK(near(f.opacity(Surface::Sidebar), 0.25));
    CHECK(colorIs(f.text(Surface::Terminal), 0x00FF00));
    CHECK(e1.size() == 1);   // "#FF0000#not..." is not a color
    CHECK(e1.size() == 1 && e1[0].line == 6);
    CHECK(colorIs(f.background(Surface::Statusbar), 0x007ACC));  // default kept

    GROUP("settings:errors");
    std::vector<SettingsError> e2;
    Settings bad = parseSettings(
        "editor.opacity = 0.5\n"         // 1 ok
        "editor.opacity = 2\n"           // 2 out of range
        "editr.opacity = 0.5\n"          // 3 unknown surface
        "editor.color = #FFFFFF\n"       // 4 unknown field
        "just some words\n"              // 5 no '='
        "= #FFFFFF\n"                    // 6 no key
        "editor.text =\n"                // 7 no value
        "editor.text = #FFF extra\n"     // 8 junk after value
        "sidebar.text = white\n"         // 9 not a color
        "window.blur = maybe\n"          // 10 not a bool
        "window.material = glass\n"      // 11 unknown material
        "editor.opacity = 80\n"          // 12 bare percentage
        "editr.opacity = abc\n",         // 13 unknown beats bad value
        &e2);
    CHECK(e2.size() == 12);
    std::vector<int> lines;
    for (const SettingsError &e : e2) lines.push_back(e.line);
    CHECK((lines == std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
    CHECK(near(bad.opacity(Surface::Editor), 0.5));      // bad lines ignored
    CHECK(colorIs(bad.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(!bad.blur());
    CHECK(bad.material() == "under-window");
    auto has = [&](size_t i, const char *needle) {
        return i < e2.size() && e2[i].message.find(needle) != std::string::npos;
    };
    CHECK(has(0, "not an opacity"));
    CHECK(has(1, "unknown setting 'editr.opacity'"));
    CHECK(has(2, "unknown setting 'editor.color'"));
    CHECK(has(3, "expected key = value"));
    CHECK(has(7, "not a color"));
    CHECK(has(8, "not true or false"));
    CHECK(has(9, "unknown material"));
    CHECK(has(10, "80%"));                               // hints at the fix
    CHECK(has(11, "unknown setting"));
    parseSettings("editor.opacity = 2\n");              // null errors is fine

    GROUP("settings:default-file");
    std::vector<SettingsError> e3;
    Settings df = parseSettings(Settings::defaultFileText(), &e3);
    CHECK(e3.empty());
    CHECK(df.windowIsOpaque() && !df.customTitlebar() && !df.blur());
    // Every documented key, uncommented, is one the parser accepts, and its
    // documented value is the real default.
    std::string keys = uncommentKeys(Settings::defaultFileText());
    // 4 window, 18 panel, 7 syntax, 4 markdown, 6 lsp
    CHECK(std::count(keys.begin(), keys.end(), '\n') == 39);
    std::vector<SettingsError> e4;
    Settings un = parseSettings(keys, &e4);
    CHECK(e4.empty());
    for (const SettingsError &e : e4)
        std::printf("    line %d: %s\n", e.line, e.message.c_str());
    for (int i = 0; i < kSurfaceCount; i++) {
        Surface s = (Surface)i;
        CHECK(un.background(s) == d.background(s));
        CHECK(un.text(s) == d.text(s));
    }
    for (int i = 1; i < 8; i++)
        CHECK(un.syntax((TokenStyle)i) == d.syntax((TokenStyle)i));
    for (int i = 0; i < kMarkdownColorCount; i++)
        CHECK(un.markdown((MarkdownColor)i) == d.markdown((MarkdownColor)i));
    CHECK(un.material() == d.material() && un.blur() == d.blur());
    // The documented servers are the first ones the built-in search tries.
    CHECK(un.lspEnabled() == d.lspEnabled());
    for (const std::string &server : Settings::lspServers()) {
        std::vector<std::string> cmds = Lsp::defaultCommands(server);
        CHECK(!cmds.empty() && un.lspCommand(server) == cmds[0]);
        CHECK(d.lspCommand(server).empty());
    }

    GROUP("settings:lsp");
    std::vector<SettingsError> e5;
    Settings ls = parseSettings(
        "lsp.enabled = false\n"                             // 1
        "lsp.python = pylsp -v --log-file /tmp/x  # log\n"  // 2 words + comment
        "lsp.cpp = \"/opt/my tools/clangd\" --background-index\n"   // 3
        "lsp.go = off\n"                                    // 4
        "lsp.cobol = cobol-ls\n"                            // 5 unknown
        "lsp.rust =\n"                                      // 6 no value
        "lsp.enabled = sometimes\n",                        // 7 not a bool
        &e5);
    CHECK(!ls.lspEnabled());
    CHECK(ls.lspCommand("python") == "pylsp -v --log-file /tmp/x");
    CHECK(ls.lspCommand("cpp") == "\"/opt/my tools/clangd\" --background-index");
    CHECK(ls.lspOff("go") && !ls.lspOff("python") && !ls.lspOff("rust"));
    CHECK(ls.lspCommand("rust").empty());
    CHECK(e5.size() == 3);
    CHECK(e5.size() == 3 && e5[0].line == 5 && e5[1].line == 6 && e5[2].line == 7);
    CHECK(!e5.empty() && e5[0].message.find("lsp.cobol") != std::string::npos);
    CHECK(parseSettings("").lspEnabled());
}

}  // namespace

// ------------------------------------------------------------ line comments
namespace {

std::string narrow(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? (char)c : '?';
    return out;
}

// Toggle with the selection written into the text as [ and ] (or a single |
// for a caret), and return the result written the same way.
std::string toggled(const std::string &marked, const std::string &marker) {
    std::u16string text;
    size_t a = std::string::npos, b = std::string::npos;
    for (char c : marked) {
        if (c == '|') { a = b = text.size(); continue; }
        if (c == '[') { a = text.size(); continue; }
        if (c == ']') { b = text.size(); continue; }
        text += (char16_t)(unsigned char)c;
    }
    LineComments::Result r = LineComments::toggle(text, a, b, marker);
    // The single-replacement form must describe the same edit.
    std::u16string viaReplace = text;
    if (r.changed)
        viaReplace.replace(r.replaceStart, r.replaceLength, r.replacement);
    if (viaReplace != r.text) return "replacement mismatch";
    std::string out = narrow(r.text);
    if (r.selStart == r.selEnd) return out.insert(r.selStart, "|");
    out.insert(r.selEnd, "]");
    return out.insert(r.selStart, "[");
}

void testLineComments() {
    using LineComments::markerFor;
    GROUP("comments:markers");
    CHECK(markerFor("a.py") == "#");
    CHECK(markerFor("settings.conf") == "#");
    CHECK(markerFor("/Users/x/.config/minicode/settings.conf") == "#");
    CHECK(markerFor("main.CPP") == "//");
    CHECK(markerFor("App.mm") == "//");
    CHECK(markerFor("index.tsx") == "//");
    CHECK(markerFor("query.sql") == "--");
    CHECK(markerFor("init.lua") == "--");
    CHECK(markerFor("setup.ini") == ";");
    CHECK(markerFor("Makefile") == "#");
    CHECK(markerFor("src/Dockerfile") == "#");
    CHECK(markerFor(".zshrc") == "#");
    CHECK(markerFor("paper.tex") == "%");
    CHECK(markerFor("data.json") == "");      // JSON has no comments
    CHECK(markerFor("README.md") == "");
    CHECK(markerFor("LICENSE") == "");
    CHECK(markerFor("") == "");

    GROUP("comments:single-line");
    CHECK(toggled("x = 1|", "#") == "# x = 1|");
    CHECK(toggled("x| = 1", "#") == "# x| = 1");
    CHECK(toggled("|x = 1", "#") == "# |x = 1");       // caret follows the text
    CHECK(toggled("# x = 1|", "#") == "x = 1|");
    CHECK(toggled("#x = 1|", "#") == "x = 1|");        // no space after marker
    CHECK(toggled("#  x|", "#") == " x|");             // only one space removed
    CHECK(toggled("    return 0;|", "//") == "    // return 0;|");
    CHECK(toggled("    // return 0;|", "//") == "    return 0;|");
    CHECK(toggled("  // |a", "//") == "  |a");         // caret inside removed marker
    CHECK(toggled("\tint a;|", "//") == "\t// int a;|");
    CHECK(toggled("a\nb|\nc", "#") == "a\n# b|\nc");   // only the caret's line
    CHECK(toggled("|", "#") == "# |");                 // empty line
    CHECK(toggled("a\n|", "#") == "a\n# |");
    CHECK(toggled("   |", "#") == "   # |");
    CHECK(toggled("SELECT 1|", "--") == "-- SELECT 1|");

    GROUP("comments:multi-line");
    CHECK(toggled("[a\nb\nc]", "#") == "[# a\n# b\n# c]");
    CHECK(toggled("[# a\n# b\n# c]", "#") == "[a\nb\nc]");
    // Mixed: anything uncommented means comment them all.
    CHECK(toggled("[# a\nb]", "#") == "[# # a\n# b]");
    // Inserted at the smallest indentation, keeping the block aligned.
    CHECK(toggled("[  if x:\n    y\n  z]", "#") == "[  # if x:\n  #   y\n  # z]");
    CHECK(toggled("[  # if x:\n  #   y\n  # z]", "#") == "[  if x:\n    y\n  z]");
    // Blank lines are skipped, in both directions.
    CHECK(toggled("[a\n\nb]", "//") == "[// a\n\n// b]");
    CHECK(toggled("[// a\n   \n// b]", "//") == "[a\n   \nb]");
    // A selection that ends at the start of a line leaves that line alone.
    CHECK(toggled("[a\nb\n]c", "#") == "[# a\n# b\n]c");
    // Selection starting mid-line keeps its place in the text.
    CHECK(toggled("ab[c\nd]e", "#") == "# ab[c\n# d]e");
    CHECK(toggled("# ab[c\n# d]e", "#") == "ab[c\nd]e");
    // Surrounding lines are untouched.
    CHECK(toggled("keep\n[a\nb]\nkeep", "//") == "keep\n[// a\n// b]\nkeep");
    // Backwards selection is fine.
    std::u16string t = u"a\nb";
    LineComments::Result r = LineComments::toggle(t, 3, 0, "#");
    CHECK(narrow(r.text) == "# a\n# b");

    GROUP("comments:non-ascii");
    // UTF-16 offsets: an emoji is two units, and the caret stays after it.
    std::u16string emoji = u"x = \U0001F600";
    LineComments::Result e = LineComments::toggle(emoji, emoji.size(), emoji.size(), "#");
    CHECK(e.text == u"# x = \U0001F600");
    CHECK(e.selStart == e.text.size());

    GROUP("comments:no-marker");
    LineComments::Result n = LineComments::toggle(u"abc", 1, 1, "");
    CHECK(!n.changed && n.text == u"abc" && n.selStart == 1);
    // Out-of-range selections are clamped rather than crashing.
    LineComments::Result o = LineComments::toggle(u"abc", 99, 99, "#");
    CHECK(o.changed && o.text == u"# abc");
}

void testSettingsColorEditing() {
    GROUP("settings:find-color");
    ColorSpan span;
    CHECK(Settings::findColor(u"editor.text = #D4D4D4", span) &&
          span.start == 14 && span.length == 7 && colorIs(span.color, 0xD4D4D4));
    CHECK(Settings::findColor(u"# editor.text = #D4D4D4", span) && span.start == 16);
    CHECK(Settings::findColor(u"sidebar.background=#12345680  # note", span) &&
          span.start == 19 && span.length == 9 && near(span.color.a, 128 / 255.0));
    CHECK(Settings::findColor(u"x = #abc\r", span) && span.length == 4);
    CHECK(!Settings::findColor(u"editor.opacity = 0.5", span));
    CHECK(!Settings::findColor(u"# Colors are #RRGGBB, or #RRGGBBAA", span));
    CHECK(!Settings::findColor(u"# Each line is key = value, and", span));
    CHECK(!Settings::findColor(u"x = #GGG", span));
    CHECK(!Settings::findColor(u"", span));

    GROUP("settings:format-color");
    CHECK(Settings::formatColor(Rgba::hex(0x0a0b0c)) == "#0A0B0C");
    CHECK(Settings::formatColor(Rgba::hex(0xFFFFFF, 0.5)) == "#FFFFFF80");
    CHECK(Settings::formatColor(Rgba::hex(0x000000, 0)) == "#00000000");
    CHECK(Settings::formatColor(Rgba::hex(0x123456, 0.999)) == "#123456");
    Rgba back;
    CHECK(Settings::parseColor(Settings::formatColor(Rgba::hex(0x336699, 0.25)), back) &&
          back.rgb() == 0x336699 && back.a > 0.24 && back.a < 0.26);

    GROUP("settings:contrast");
    Settings dark = Settings::parse("");
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0xD4D4D4)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0x1E1E1E)).rgb() == 0xFFFFFF);
    CHECK(dark.contrastText(Rgba::hex(0x007ACC)).rgb() == 0xFFFFFF);
    // A see-through white swatch over the dark editor mostly shows the editor.
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF, 0.1)).rgb() == 0xFFFFFF);
    Settings light = Settings::parse("editor.background = #FFFFFF\n");
    CHECK(light.contrastText(Rgba::hex(0x000000, 0.1)).rgb() == 0x000000);

    GROUP("settings:set-color");
    Rgba red = Rgba::hex(0xFF0000);
    CHECK(Settings::setColor(u"editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    // A commented-out setting is switched on.
    CHECK(Settings::setColor(u"# editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"#editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"  # editor.text = #D4D4D4", red) == u"  editor.text = #FF0000");
    // Trailing comments and alpha survive.
    CHECK(Settings::setColor(u"sidebar.text = #FFF  # mine", Rgba::hex(0x00FF00, 0.5)) ==
          u"sidebar.text = #00FF0080  # mine");
    // Lines without a color, or prose, are left alone.
    CHECK(Settings::setColor(u"editor.opacity = 0.5", red) == u"editor.opacity = 0.5");
    CHECK(Settings::setColor(u"# Colors are #RRGGBB", red) == u"# Colors are #RRGGBB");
    CHECK(Settings::setColor(u"# a note, x y = #FFFFFF", red) == u"# a note, x y = #FF0000");
    // The result parses as the new setting.
    std::u16string line = Settings::setColor(u"# terminal.text = #D4D4D4", red);
    CHECK(colorIs(Settings::parse(narrow(line)).text(Surface::Terminal), 0xFF0000));
}

// ------------------------------------------------------------------- latex

// The span covering `text`, or nullptr.
const LatexSpan *spanWithText(const LatexDoc &doc, const std::string &text) {
    for (const LatexSpan &sp : doc.spans)
        if (sp.display == text) return &sp;
    return nullptr;
}

bool hasSpanText(const LatexDoc &doc, const std::string &text) {
    return spanWithText(doc, text) != nullptr;
}

const char *kSampleTex =
    "\\documentclass{article}\n"                       // 1
    "\\usepackage{amsmath}\n"                          // 2
    "\\title{My Paper}\n"                              // 3
    "\\author{A. Writer}\n"                            // 4
    "\\begin{document}\n"                              // 5
    "\\maketitle\n"                                    // 6
    "\\section{Introduction}\\label{sec:intro}\n"       // 7
    "Some prose with \\textbf{bold} inside.\n"          // 8
    "\n"                                               // 9
    "Another paragraph, $x^2 + 1$ included.\n"          // 10
    "\\begin{itemize}\n"                               // 11
    "  \\item First thing\n"                           // 12
    "  \\item Second thing\n"                          // 13
    "\\end{itemize}\n"                                 // 14
    "\\end{document}\n";                               // 15

void testLatexDoc() {
    std::string src = kSampleTex;
    LatexDoc doc = LatexDoc::parse(src);

    GROUP("latex:fields");
    const LatexSpan *title = spanWithText(doc, "My Paper");
    CHECK(title != nullptr);
    CHECK(title && title->kind == LatexSpanKind::Field);
    CHECK(title && title->command == "title");
    CHECK(title && title->line == 3);
    CHECK(title && src.substr(title->start, title->end - title->start) == "My Paper");
    const LatexSpan *sec = spanWithText(doc, "Introduction");
    CHECK(sec && sec->command == "section" && sec->line == 7);
    CHECK(hasSpanText(doc, "A. Writer"));

    GROUP("latex:skips-machinery");
    // Package options, labels and class names are not prose, so are not offered.
    CHECK(!hasSpanText(doc, "amsmath"));
    CHECK(!hasSpanText(doc, "sec:intro"));
    CHECK(!hasSpanText(doc, "article"));

    GROUP("latex:text-runs");
    // A styling command splits the run; its content is editable on its own.
    CHECK(hasSpanText(doc, "Some prose with"));
    CHECK(hasSpanText(doc, "bold"));
    CHECK(hasSpanText(doc, "inside."));
    // A blank line ends a paragraph.
    const LatexSpan *para = spanWithText(doc, "Another paragraph,");
    CHECK(para != nullptr);
    CHECK(para && para->line == 10);
    // Nothing before \begin{document} is treated as body text.
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Text) CHECK(sp.line >= 6);

    GROUP("latex:math");
    const LatexSpan *math = nullptr;
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Math) math = &sp;
    CHECK(math != nullptr);
    CHECK(math && src.substr(math->start, math->end - math->start) == "x^2 + 1");
    LatexDoc envMath = LatexDoc::parse(
        "\\begin{document}\\begin{align}\na &= b\n\\end{align}\\end{document}");
    CHECK(envMath.spans.size() == 1);
    CHECK(envMath.spans[0].kind == LatexSpanKind::Math);
    CHECK(envMath.spans[0].display.find("a") != std::string::npos);

    GROUP("latex:lists");
    CHECK(doc.lists.size() == 1);
    const LatexList &list = doc.lists[0];
    CHECK(list.environment == "itemize");
    CHECK(list.itemBodyStart.size() == 2);
    CHECK(list.itemBodyEnd.size() == 2);
    CHECK(list.indent == "  ");
    const LatexSpan *first = spanWithText(doc, "First thing");
    CHECK(first != nullptr);
    CHECK(first && first->listIndex == 0 && first->itemIndex == 0);
    const LatexSpan *second = spanWithText(doc, "Second thing");
    CHECK(second && second->itemIndex == 1);

    GROUP("latex:lines");
    CHECK(LatexDoc::lineAt(src, 0) == 1);
    CHECK(doc.spansOnLine(12).size() == 1);
    CHECK(doc.spansOnLine(999).empty());

    GROUP("latex:display-text");
    CHECK(LatexDoc::displayText("a  b\n  c") == "a b c");
    CHECK(LatexDoc::displayText("50\\% off \\& more") == "50% off & more");
    CHECK(LatexDoc::displayText("a~b") == "a b");
    CHECK(LatexDoc::displayText("The \\emph{fun} part") == "The fun part");
    CHECK(LatexDoc::displayText("x % a comment\ny") == "x y");

    GROUP("latex:replace");
    LatexDoc::Edit e = LatexDoc::replaceSpan(src, *title, "A Better Title");
    CHECK(e.source.find("\\title{A Better Title}") != std::string::npos);
    CHECK(e.source.find("My Paper") == std::string::npos);
    CHECK(e.source.substr(e.start, e.end - e.start) == "A Better Title");
    // The rest of the file is untouched, byte for byte.
    CHECK(e.source.substr(0, title->start) == src.substr(0, title->start));
    CHECK(e.source.substr(e.end) == src.substr(title->end));
    // Re-parsing the result finds the new text where the old one was.
    LatexDoc after = LatexDoc::parse(e.source);
    CHECK(hasSpanText(after, "A Better Title"));

    GROUP("latex:add-item");
    LatexDoc::Edit add = LatexDoc::addItem(src, list, 1, "Third thing");
    CHECK(add.source.find("  \\item Second thing\n  \\item Third thing\n"
                          "\\end{itemize}") != std::string::npos);
    CHECK(add.source.substr(add.start, add.end - add.start) == "Third thing");
    LatexDoc addedDoc = LatexDoc::parse(add.source);
    CHECK(addedDoc.lists.size() == 1 && addedDoc.lists[0].itemBodyStart.size() == 3);
    CHECK(hasSpanText(addedDoc, "Third thing"));
    // Inserting in the middle keeps the order.
    LatexDoc::Edit mid = LatexDoc::addItem(src, list, 0, "Middle");
    CHECK(mid.source.find("First thing\n  \\item Middle\n  \\item Second") !=
          std::string::npos);
    // A list written on one line still gets a well-formed new item.
    std::string oneLine =
        "\\begin{document}\\begin{itemize}\\item one\\end{itemize}\\end{document}";
    LatexDoc oneDoc = LatexDoc::parse(oneLine);
    CHECK(oneDoc.lists.size() == 1);
    LatexDoc::Edit oneAdd = LatexDoc::addItem(oneLine, oneDoc.lists[0], 0, "two");
    CHECK(LatexDoc::parse(oneAdd.source).lists[0].itemBodyStart.size() == 2);
    CHECK(hasSpanText(LatexDoc::parse(oneAdd.source), "two"));

    GROUP("latex:click");
    // SyncTeX names the line a paragraph closed on, one past the prose here.
    const LatexSpan *hit = doc.spanForClick({9}, "prose");
    CHECK(hit && hit->display == "Some prose with");
    // The word decides between two items whose lines both look plausible.
    CHECK(doc.spanForClick({13, 14}, "First")->display == "First thing");
    CHECK(doc.spanForClick({13, 14}, "Second")->display == "Second thing");
    // A heading is found on its own line.
    CHECK(doc.spanForClick({7}, "Introduction")->display == "Introduction");
    // Without a usable word, the nearest span still wins.
    CHECK(doc.spanForClick({12}, "")->display == "First thing");
    CHECK(doc.spanForClick({12}, "nowhere") == nullptr);   // refuses to guess
    CHECK(doc.spanForClick({}, "First") == nullptr);
    // The PDF reads math back without its markup, so matching ignores it.
    CHECK(doc.spanForClick({10}, "x2")->kind == LatexSpanKind::Math);
    CHECK(doc.spanForClick({10}, "paragraph,")->display == "Another paragraph,");
    CHECK(doc.spanForClick({900}, "First") == nullptr);

    GROUP("latex:real-documents");
    // Real documents wrap their text in all sorts of commands, including the
    // author's own macros. Text the reader cannot see is text the preview
    // cannot edit, so unknown commands are looked through, not skipped.
    LatexDoc wild = LatexDoc::parse(
        "\\begin{document}\n"
        "\\normalfont{Post-training of Gemini}\n"
        "\\raisebox{0.5em}{\\textit{Senior Engineer} @ Google}\n"
        "\\makebox[8em][l]{\\includegraphics[scale=0.1]{GDM.png}}\n"
        "\\twemoji{globe} \\href{https://echansen.org}{echansen.org}\n"
        "\\todo{Bullet 2 -- scale and adoption}\n"
        "\\vspace*{-\\baselineskip}\n"
        "\\setlength{\\parskip}{1em}\n"
        "\\multicolumn{2}{c}{Total}\n"
        "\\end{document}\n");
    CHECK(hasSpanText(wild, "Post-training of Gemini"));
    CHECK(hasSpanText(wild, "Senior Engineer"));
    CHECK(hasSpanText(wild, "@ Google"));
    CHECK(hasSpanText(wild, "Bullet 2 -- scale and adoption"));   // a user macro
    CHECK(hasSpanText(wild, "Total"));
    // ... while arguments that are machinery stay out of reach.
    CHECK(!hasSpanText(wild, "GDM.png"));
    CHECK(!hasSpanText(wild, "https://echansen.org"));
    CHECK(hasSpanText(wild, "echansen.org"));     // the link text, not its URL
    CHECK(!hasSpanText(wild, "0.5em"));
    CHECK(!hasSpanText(wild, "1em"));
    CHECK(!hasSpanText(wild, "8em"));
    CHECK(!hasSpanText(wild, "l"));
    CHECK(!hasSpanText(wild, "c"));
    CHECK(!hasSpanText(wild, "2"));
    // A date is text even though it has no letters in it.
    LatexDoc dates = LatexDoc::parse(
        "\\begin{document}\\raisebox{0.5em}{\\textit{2014 - 2019}}"
        "\\normalfont{11/2025 - ...}\\end{document}");
    CHECK(hasSpanText(dates, "2014 - 2019"));
    CHECK(hasSpanText(dates, "11/2025 - ..."));
    CHECK(!hasSpanText(dates, "0.5em"));
    LatexDoc refs = LatexDoc::parse(
        "\\begin{document}\\label{sec:intro}\\cite{knuth1984}"
        "\\includegraphics{fig.pdf}\\input{chapter}Real text\\end{document}");
    CHECK(refs.spans.size() == 1 && refs.spans[0].display == "Real text");

    GROUP("latex:click-refuses");
    // A click on something the reader does not know must not open the nearest
    // text instead: editing the wrong line is worse than editing nothing.
    CHECK(doc.spanForClick({12, 13}, "Photograph") == nullptr);
    CHECK(doc.spanForClick({7}, "Introduction") != nullptr);
    // With nothing to match on (a logo, a glyph the PDF cannot name), the
    // nearest span is still offered.
    CHECK(doc.spanForClick({12}, "") != nullptr);
    // A click that reports a whole line finds the span inside it.
    CHECK(doc.spanForClick({12}, "First thing and more")->display == "First thing");

    GROUP("latex:verbatim");
    LatexDoc verb = LatexDoc::parse(
        "\\begin{document}\\begin{verbatim}\n\\section{no}\n"
        "\\end{verbatim}\nafter\\end{document}");
    CHECK(!hasSpanText(verb, "no"));
    CHECK(hasSpanText(verb, "after"));

    GROUP("latex:tabular");
    LatexDoc tab = LatexDoc::parse(
        "\\begin{document}\\begin{tabular}{|l|r|}\nApples & 3 \\\\\n"
        "\\end{tabular}\\end{document}");
    CHECK(!hasSpanText(tab, "|l|r|"));
    CHECK(hasSpanText(tab, "Apples"));   // each cell is editable on its own
    CHECK(hasSpanText(tab, "3"));

    GROUP("latex:description");
    LatexDoc desc = LatexDoc::parse(
        "\\begin{document}\\begin{description}\n\\item[Term] meaning\n"
        "\\end{description}\\end{document}");
    CHECK(hasSpanText(desc, "Term"));
    CHECK(hasSpanText(desc, "meaning"));
    CHECK(desc.lists.size() == 1 && desc.lists[0].itemBodyStart.size() == 1);

    GROUP("latex:robustness");
    // Unterminated input must not hang or read past the end.
    CHECK(LatexDoc::parse("\\section{unclosed").spans.size() == 1);
    CHECK(LatexDoc::parse("\\begin{itemize}\\item x").lists.size() == 1);
    CHECK(LatexDoc::parse("$x").spans.size() <= 1);
    CHECK(LatexDoc::parse("").spans.empty());
    CHECK(LatexDoc::parse("\\").spans.empty());
}

// ------------------------------------------------------------------ synctex

// Two boxes on page 1: source line 7 near the top, line 12 lower down.
// Values are scaled points (1pt = 65536sp); y is the baseline of the box.
const char *kSampleSyncTex =
    "SyncTeX Version:1\n"
    "Input:1:/tmp/doc.tex\n"
    "Input:2:/tmp/other.tex\n"
    "Output:pdf\n"
    "Magnification:1000\n"
    "Unit:1\n"
    "X Offset:0\n"
    "Y Offset:0\n"
    "Content:\n"
    "!753\n"
    "{1\n"
    "[1,7:4718592,6553600:19660800,655360,0\n"   // x=72 y=100 w=300 h=10
    "h1,7:4718592,6553600:19660800,655360,0\n"
    "[1,12:4718592,19660800:13107200,655360,0\n" // x=72 y=300 w=200 h=10
    "]\n"
    "]\n"
    "}1\n"
    "Postamble:\n";

void testSyncTex() {
    SyncTexIndex idx = SyncTexIndex::parse(kSampleSyncTex);

    GROUP("synctex:parse");
    CHECK(idx.valid());
    CHECK(idx.recordCount() == 3);
    CHECK(idx.pathForTag(1) == "/tmp/doc.tex");
    CHECK(idx.tagForPath("/tmp/doc.tex") == 1);
    CHECK(idx.tagForPath("doc.tex") == 1);          // matched by file name
    CHECK(idx.tagForPath("/elsewhere/other.tex") == 2);
    CHECK(idx.tagForPath("missing.tex") == 0);

    GROUP("synctex:hit");
    // A point inside the first box resolves to its line.
    std::vector<SyncTexHit> hits = idx.hitsAtPoint(1, 100, 95);
    CHECK(!hits.empty());
    CHECK(!hits.empty() && hits[0].line == 7);
    CHECK(!hits.empty() && hits[0].distance == 0.0);
    CHECK(!hits.empty() && hits[0].tag == 1);
    // The box is reported in points, measured from the top-left of the page.
    CHECK(!hits.empty() && std::abs(hits[0].x - 72.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].width - 300.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].y - 90.0) < 0.01);

    // Inside the second box.
    hits = idx.hitsAtPoint(1, 100, 295);
    CHECK(!hits.empty() && hits[0].line == 12);

    // Between them: nothing contains the point, so the nearest box wins, and
    // the other line is still offered as a candidate.
    hits = idx.hitsAtPoint(1, 100, 260);
    CHECK(hits.size() == 2);
    CHECK(!hits.empty() && hits[0].line == 12);
    CHECK(hits.size() > 1 && hits[1].line == 7);

    // One hit per source line, and never from another page.
    CHECK(idx.hitsAtPoint(1, 100, 95).size() == 2);
    CHECK(idx.hitsAtPoint(2, 100, 95).empty());

    GROUP("synctex:robustness");
    CHECK(!SyncTexIndex::parse("").valid());
    CHECK(!SyncTexIndex::parse("SyncTeX Version:1\nContent:\n{1\nnonsense\n").valid());
    CHECK(SyncTexIndex::parse(kSampleSyncTex).hitsAtPoint(1, 0, 0, 1).size() == 1);
}

// ------------------------------------------------------ latex click-to-source
// Each group is a failure the click sweep (tests/latex/sweep.sh) found in a
// real or torture document: a double-click that refused, or opened the wrong
// text.

// The span holding exactly `text` as its source bytes, or null.
const LatexSpan *spanWithSource(const LatexDoc &doc, const std::string &src,
                                const std::string &text) {
    for (const LatexSpan &sp : doc.spans)
        if (src.substr(sp.start, sp.end - sp.start) == text) return &sp;
    return nullptr;
}

std::string sourceOf(const std::string &src, const LatexSpan *sp) {
    return sp ? src.substr(sp->start, sp->end - sp->start) : std::string("(none)");
}

void testLatexClicks() {
    GROUP("latex:link-inside-wrapper");
    // A wrapper whose text holds a link used to be taken for a URL argument
    // and skipped whole, so nothing in an italic note or a bullet that cites a
    // paper could be clicked.
    std::string wrapped =
        "\\begin{document}\n"
        "\\it{Note: my notes live at \\href{https://notes.example}{notes.example}}.\n"
        "\\normalfont{Built a birdhouse (\\href{https://x.org/paper}{paper}) "
        "painted it blue.}\n"
        "\\normalfont{\\href{https://scholar.example/u}{Google Scholar}}\n"
        "\\end{document}\n";
    LatexDoc w = LatexDoc::parse(wrapped);
    CHECK(hasSpanText(w, "Note: my notes live at"));
    CHECK(hasSpanText(w, "notes.example"));
    CHECK(hasSpanText(w, "Built a birdhouse ("));
    CHECK(hasSpanText(w, "paper"));
    CHECK(hasSpanText(w, "Google Scholar"));
    CHECK(!hasSpanText(w, "https://notes.example"));   // the URLs stay machinery
    CHECK(!hasSpanText(w, "https://x.org/paper"));
    CHECK(w.spanForClick({2}, "Note") != nullptr);
    CHECK(sourceOf(wrapped, w.spanForClick({3}, "painted")) == ") painted it blue.");

    GROUP("latex:match-key");
    // The PDF gives ligatures as one character and accents as letters; the
    // source spells them in ASCII and TeX escapes. Both meet in the key.
    CHECK(LatexDoc::matchKey("e\xEF\xAC\x83" "cient") == "efficient");   // U+FB03
    CHECK(LatexDoc::matchKey("\xEF\xAC\x81nd") == "find");                // U+FB01
    CHECK(LatexDoc::matchKey("R\xC3\xA9sum\xC3\xA9") == "resume");
    CHECK(LatexDoc::matchKey("Stra\xC3\x9F" "e") == "strasse");
    CHECK(LatexDoc::matchKey("se\xC3\xB1or, co\xC3\xB6perate") == "senorcooperate");
    CHECK(LatexDoc::matchKey("\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 x") == "quotedx");
    CHECK(LatexDoc::matchKey("\xCE\xB1\xCE\xB2") == "\xCE\xB1\xCE\xB2");  // Greek kept
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("r\\'esum\\'e")) == "resume");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("r\\'{e}sum\\'{e}")) == "resume");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("na\\\"ive")) == "naive");
    CHECK(LatexDoc::displayText("Stra\\ss e") == "Strasse");
    CHECK(LatexDoc::displayText("Fran\\c{c}ais") == "Francais");
    CHECK(LatexDoc::displayText("hy\\-phen") == "hyphen");
    CHECK(LatexDoc::displayText("the \\LaTeX{} way") == "the LaTeX way");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("$\\alpha + \\beta$")) ==
          "\xCE\xB1\xCE\xB2");

    GROUP("latex:accents-stay-in-the-run");
    // \ss, \c{c} and \'{e} are letters of a word, not markup that ends a run.
    std::string accents =
        "\\begin{document}\nCaf\\'{e} in der Stra\\ss e, Fran\\c{c}ais.\n"
        "\\end{document}\n";
    LatexDoc acc = LatexDoc::parse(accents);
    CHECK(acc.spans.size() == 1);
    CHECK(sourceOf(accents, acc.spanForClick({2}, "Stra\xC3\x9F" "e")) ==
          "Caf\\'{e} in der Stra\\ss e, Fran\\c{c}ais.");
    CHECK(acc.spanForClick({2}, "Fran\xC3\xA7" "ais") != nullptr);
    CHECK(acc.spanForClick({2}, "Caf\xC3\xA9") != nullptr);
    std::string ligs = "\\begin{document}\nAn efficient office.\n\\end{document}\n";
    CHECK(LatexDoc::parse(ligs).spanForClick({2}, "e\xEF\xAC\x83" "cient") != nullptr);
    CHECK(LatexDoc::parse(ligs).spanForClick({2}, "o\xEF\xAC\x83" "ce") != nullptr);

    GROUP("latex:row-break");
    // \\ ends a run like & does, so a cell or a resume line edits alone.
    std::string rows =
        "\\begin{document}\n\\textit{Principal Engineer} \\hfill 2014 -- 2019\\\\\n"
        "Next line\n\\end{document}\n";
    LatexDoc rw = LatexDoc::parse(rows);
    CHECK(spanWithSource(rw, rows, "2014 -- 2019") != nullptr);
    CHECK(spanWithSource(rw, rows, "Next line") != nullptr);

    GROUP("latex:url");
    // \url prints its argument, so the URL is the text to edit.
    std::string urls = "\\begin{document}\nSee \\url{https://example.org} now.\n"
                       "\\end{document}\n";
    LatexDoc ur = LatexDoc::parse(urls);
    const LatexSpan *url = spanWithSource(ur, urls, "https://example.org");
    CHECK(url && url->kind == LatexSpanKind::Field && url->command == "url");
    CHECK(ur.spanForClick({2}, "https://example.org") == url);

    GROUP("latex:maketitle");
    // \title sits in the preamble; SyncTeX places its text at \maketitle, or
    // the line after it.
    std::string titled =
        "\\documentclass{article}\n\\title{Torture Document}\n"
        "\\author{Quentin Placeholder}\n\\begin{document}\n\\maketitle\n\n"
        "Body text.\n\\end{document}\n";
    LatexDoc tt = LatexDoc::parse(titled);
    CHECK(tt.titleLines.size() == 1 && tt.titleLines[0] == 5);
    const LatexSpan *ttl = tt.spanForClick({6}, "Document");
    CHECK(ttl && ttl->command == "title");
    const LatexSpan *ath = tt.spanForClick({5}, "Placeholder");
    CHECK(ath && ath->command == "author");
    CHECK(tt.spanForClick({6}, "Unrelated") == nullptr);

    GROUP("latex:multi-line-span");
    // A span is found from any line it covers, not only its first.
    std::string longPara =
        "\\begin{document}\nOne\ntwo\nthree\nfour\nfive\nsix seven\n\n"
        "\\end{document}\n";
    LatexDoc lp = LatexDoc::parse(longPara);
    CHECK(lp.spans.size() == 1 && lp.spans[0].line == 2 && lp.spans[0].endLine == 7);
    CHECK(lp.spanForClick({7}, "One") != nullptr);

    GROUP("latex:word-split-by-font");
    // foo\emph{bar}baz is one word on the page and three spans in the
    // source; a join covers all three as one balanced range.
    std::string split = "\\begin{document}\nSplit: foo\\emph{bar}baz here.\n"
                        "\\end{document}\n";
    LatexDoc sp = LatexDoc::parse(split);
    CHECK(sp.joins.size() == 1);
    const LatexSpan *joined = sp.spanForClick({2}, "foobarbaz");
    CHECK(sourceOf(split, joined) == "Split: foo\\emph{bar}baz here.");
    // Clicking the italic part alone still opens just that part.
    CHECK(sourceOf(split, sp.spanForClick({2}, "bar")) == "bar");
    // A spaced font change is two words, so nothing is joined.
    CHECK(LatexDoc::parse("\\begin{document}\nfoo \\emph{bar} baz\n\\end{document}")
              .joins.empty());
    // A join whose range would unbalance the braces is not offered.
    CHECK(LatexDoc::parse("\\begin{document}\n\\textbf{\\emph{x}}y\n\\end{document}")
              .joins.empty());
    // Replacing a join is still one exact splice.
    LatexDoc::Edit je = LatexDoc::replaceSpan(split, *joined, "new");
    CHECK(je.source == "\\begin{document}\nnew\n\\end{document}\n");

    GROUP("latex:footnote-mark");
    // The PDF glues the footnote mark onto the word: "footnote1", "1The".
    std::string fn = "\\begin{document}\nA footnote.\\footnote{The note.}\n"
                     "\\end{document}\n";
    LatexDoc fd = LatexDoc::parse(fn);
    CHECK(sourceOf(fn, fd.spanForClick({2}, "footnote.1")) == "A footnote.");
    CHECK(sourceOf(fn, fd.spanForClick({2}, "1The")) == "The note.");

    GROUP("latex:short-words");
    // A short word must be a whole word of the span: "at" is not in "Math",
    // "is" is not in "Visit". Pieces split by punctuation still count.
    std::string shorts = "\\begin{document}\n\\section{Math}\nVisit\n\n\n\n\n"
                         "Go end-to-end at 5~pm.\n\\end{document}\n";
    LatexDoc sh = LatexDoc::parse(shorts);
    CHECK(sh.spanForClick({2}, "at") == nullptr);           // not in "Math"
    CHECK(sh.spanForClick({2, 3}, "is") == nullptr);        // not in "Visit"
    CHECK(sh.spanForClick({8}, "to") != nullptr);
    CHECK(sh.spanForClick({8}, "5 pm") != nullptr);
    CHECK(sh.spanForClick({8}, "at") != nullptr);
    // Math still matches inside, since the PDF reads x^2 back as "x2".
    CHECK(LatexDoc::parse("\\begin{document}\n$x^2$\n\\end{document}")
              .spanForClick({2}, "x2") != nullptr);

    GROUP("latex:context");
    // The same word in several nearby spans: the words around the click on
    // the page decide which one it was.
    std::string ctx =
        "\\begin{document}\n"
        "A paragraph with \\textit{italic walrus} words, \\emph{emphasized pelican}\n"
        "words, and \\underline{underlined badger} words and more.\n"
        "\\end{document}\n";
    LatexDoc cx = LatexDoc::parse(ctx);
    // Without context the nearest wins; with it, the right one.
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "emphasized pelican",
                                        ", and underlined")) == "words, and");
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "italic walrus",
                                        ", emphasized")) == "words,");
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "underlined badger",
                                        "and more")) == "words and more.");

    GROUP("latex:single-characters");
    // A single letter or digit is refused, whatever the page shows around it.
    // It could be text TeX made up (a page, section or list number), which
    // has no place in the source, and the word beside it opens the same span.
    std::string pages =
        "\\documentclass{article}\n"                                 // 1
        "\\begin{document}\n"                                        // 2
        "\\section{Lists}\n"                                         // 3
        "See Fig.~3 here, a note I wrote in 2 minutes.\n"             // 4
        "\\begin{enumerate}\n"                                       // 5
        "  \\item First numbered hedgehog.\n"                        // 6
        "  \\item Second numbered porcupine.\n"                      // 7
        "\\end{enumerate}\n"                                         // 8
        "\\begin{enumerate}[(a)]\n"                                  // 9
        "  \\item Lettered item.\n"                                  // 10
        "\\end{enumerate}\n"                                         // 11
        "$x + y = z$ closes the page.\n"                              // 12
        "\\end{document}\n";
    LatexDoc pd = LatexDoc::parse(pages);
    // A page number at the foot of the page, below the last paragraph. It
    // used to fall through to the nearest span and open that paragraph.
    CHECK(pd.spanForClick({12, 11}, "2", "z closes the page.", "") == nullptr);
    CHECK(pd.spanForClick({12}, "2") == nullptr);
    // A section number, in front of its heading. It used to open the heading.
    CHECK(pd.spanForClick({3, 4}, "1", "", "Lists See Fig. 3 here") == nullptr);
    CHECK(pd.spanForClick({3}, "1") == nullptr);
    // A list number and a list label, in front of their items. They used to
    // open the item; that is the price of never opening the wrong text.
    CHECK(pd.spanForClick({6}, "1", "2 minutes.", ". First numbered") == nullptr);
    CHECK(pd.spanForClick({7}, "2.", "hedgehog.", "Second numbered") == nullptr);
    CHECK(pd.spanForClick({10}, "(a)", "porcupine.", "Lettered item.") == nullptr);
    // Real single characters in the source are refused too, even where the
    // text around them agrees: a digit, one-letter words, a math variable.
    CHECK(pd.spanForClick({4}, "3", "See Fig.", "here, a note") == nullptr);
    CHECK(pd.spanForClick({4}, "a", "Fig. 3 here,", "note I wrote") == nullptr);
    CHECK(pd.spanForClick({4}, "I", "here, a note", "wrote in 2") == nullptr);
    CHECK(pd.spanForClick({12}, "y", "x +", "= z closes") == nullptr);
    // One character is one character in any script: a Greek letter, an
    // accented letter.
    std::string greek = "\\begin{document}\n$\\alpha + \\beta$ and "
                        "\\'e t\\'e\n\\end{document}\n";
    LatexDoc gd = LatexDoc::parse(greek);
    CHECK(gd.spanForClick({2}, "\xCE\xB1") == nullptr);                 // α
    CHECK(gd.spanForClick({2}, "\xCE\xB1", "", "+ \xCE\xB2 and") == nullptr);
    CHECK(gd.spanForClick({2}, "\xC3\xA9", "and", "t\xC3\xA9") == nullptr);   // é
    CHECK(gd.spanForClick({2}, "t\xC3\xA9") != nullptr);                // "té"
    // A line reported whole (for a glyph that is not a word) is the same:
    // one character is refused, and a line of words is still found.
    CHECK(pd.spanForClick({6}, "1.") == nullptr);
    CHECK(sourceOf(pages, pd.spanForClick({6}, "1. First numbered hedgehog.")) ==
          "First numbered hedgehog.");
    // The words beside them still open the right text.
    CHECK(sourceOf(pages, pd.spanForClick({6}, "First", "2 minutes. 1.",
                                          "numbered hedgehog.")) ==
          "First numbered hedgehog.");
    CHECK(sourceOf(pages, pd.spanForClick({4}, "note", "Fig. 3 here, a",
                                          "I wrote in 2")) ==
          "See Fig.~3 here, a note I wrote in 2 minutes.");
    CHECK(sourceOf(pages, pd.spanForClick({3, 4}, "Lists", "1", "See Fig.")) == "Lists");
    // Two characters are a word again ("at", "is" are checked above), and a
    // glyph with no letters or digits at all still offers the nearest span.
    CHECK(sourceOf(pages, pd.spanForClick({4}, "in", "a note I wrote", "2 minutes.")) ==
          "See Fig.~3 here, a note I wrote in 2 minutes.");
    CHECK(pd.spanForClick({6}, "\xE2\x80\xA2") != nullptr);   // a bullet
}

// A line box that SyncTeX files under line 23 (where its paragraph ended),
// with the glue between its words filed under the lines they were read on.
const char *kLineSyncTex =
    "SyncTeX Version:1\n"
    "Input:1:/tmp/doc.tex\n"
    "Unit:1\n"
    "X Offset:0\n"
    "Y Offset:0\n"
    "Content:\n"
    "{1\n"
    "[1,30:0,6553600:39321600,6553600,0\n"
    "(1,23:4718592,6553600:26214400,655360,131072\n"   // x=72 w=400, baseline 100
    "g1,20:9830400,6553600\n"                           // glue at x=150, line 20
    "g1,21:16384000,6553600\n"                          // glue at x=250, line 21
    "g1,24:30932992,6553600\n"                          // glue at the box's end
    ")\n"
    "]\n"
    "}1\n";

void testSyncTexText() {
    GROUP("synctex:text-lines");
    SyncTexIndex idx = SyncTexIndex::parse(kLineSyncTex);
    // The plain box lookup can only say "line 23".
    CHECK(idx.hitsAtPoint(1, 200, 95)[0].line == 23);
    // The glue beside the word knows better: right of it first, then left.
    std::vector<SyncTexHit> h = idx.textHitsAtPoint(1, 200, 95);
    CHECK(h.size() >= 3);
    CHECK(h.size() >= 3 && h[0].line == 21 && h[1].line == 20 && h[2].line == 23);
    CHECK(!h.empty() && std::abs(h[0].x - 72.0) < 0.01);   // anchored on the line
    CHECK(idx.textHitsAtPoint(1, 100, 95)[0].line == 20);
    // Glue on the box's end edge closes the box rather than following the
    // word, so the glue before the word comes first.
    std::vector<SyncTexHit> e = idx.textHitsAtPoint(1, 300, 95);
    CHECK(e.size() >= 2 && e[0].line == 21 && e[1].line == 24);
    // Off every line box, it is the plain lookup.
    CHECK(idx.textHitsAtPoint(1, 200, 300)[0].line ==
          idx.hitsAtPoint(1, 200, 300)[0].line);
    CHECK(idx.textHitsAtPoint(2, 200, 95).empty());
}

}  // namespace

// -------------------------------------------------------------------- JSON
namespace {

Json parsed(const std::string &text) {
    Json j;
    Json::parse(text, j);
    return j;
}

void testJson() {
    GROUP("json:parse");
    Json j;
    CHECK(Json::parse(" {\"a\": [1, 2.5, -3e2, true, false, null], \"b\": \"x\"} ", j));
    CHECK(j.isObject() && j.size() == 2);
    CHECK(j["a"].size() == 6);
    CHECK(j["a"][0].asInt() == 1);
    CHECK(j["a"][1].asNumber() == 2.5);
    CHECK(j["a"][2].asNumber() == -300);
    CHECK(j["a"][3].asBool() && !j["a"][4].asBool(true) && j["a"][5].isNull());
    CHECK(j["b"].asString() == "x");
    // Missing keys and out-of-range indexes read as null, never throw.
    CHECK(j["nope"].isNull() && j["nope"]["deeper"][3].isNull());
    CHECK(j["a"][99].isNull() && j["b"][0].isNull());
    CHECK(j["b"].asInt(7) == 7 && j["a"].asString().empty());

    GROUP("json:strings");
    CHECK(parsed("\"a\\\"b\\\\c\\/d\\n\\t\"").asString() == "a\"b\\c/d\n\t");
    CHECK(parsed("\"\\u00e9\"").asString() == "\xC3\xA9");                 // é
    CHECK(parsed("\"\\u20AC\"").asString() == "\xE2\x82\xAC");             // €
    CHECK(parsed("\"\\ud83d\\ude00\"").asString() == "\xF0\x9F\x98\x80");  // 😀 pair
    CHECK(parsed("\"\\ud83d\"").asString() == "\xEF\xBF\xBD");  // lone high -> U+FFFD
    CHECK(parsed("\"caf\xC3\xA9\"").asString() == "caf\xC3\xA9");  // raw UTF-8 kept

    GROUP("json:errors");
    std::string err;
    CHECK(!Json::parse("", j, &err) && !err.empty());
    CHECK(!Json::parse("{", j));
    CHECK(!Json::parse("[1,]", j));
    CHECK(!Json::parse("{\"a\" 1}", j));
    CHECK(!Json::parse("01", j));
    CHECK(!Json::parse("tru", j));
    CHECK(!Json::parse("\"unterminated", j));
    CHECK(!Json::parse("\"tab\there\"", j));   // raw control character
    CHECK(!Json::parse("1 2", j));
    CHECK(!Json::parse(std::string(300, '['), j));   // too deep, not a crash
    Json keep = Json(5);
    CHECK(!Json::parse("nope", keep) && keep.asInt() == 5);   // untouched on failure

    GROUP("json:dump");
    Json o = Json::object({{"id", 3}, {"name", "a\"b\n"}, {"ok", true}});
    o.set("list", Json::array().push(1).push(Json()).push(1.5));
    CHECK(o.dump() == "{\"id\":3,\"name\":\"a\\\"b\\n\",\"ok\":true,\"list\":[1,null,1.5]}");
    CHECK(Json(std::string("\x01")).dump() == "\"\\u0001\"");
    CHECK(Json(-42).dump() == "-42");
    CHECK(Json::object().dump() == "{}" && Json::array().dump() == "[]");
    o.set("id", 4);   // replaces in place, keeps order
    CHECK(o.dump().rfind("{\"id\":4,", 0) == 0);
    // Round trip.
    Json back;
    CHECK(Json::parse(o.dump(), back) && back == o);
    CHECK(parsed("{\"a\":1,\"b\":2}") == parsed("{\"b\":2,\"a\":1}"));
    CHECK(parsed("[1,2]") != parsed("[2,1]"));
}

// --------------------------------------------------------------------- LSP
void testLspFraming() {
    GROUP("lsp:framing");
    std::string one = "{\"a\":1}", two = "{\"b\":\"\xC3\xA9\"}";   // é is 2 bytes
    CHECK(Lsp::Framer::frame(one) == "Content-Length: 7\r\n\r\n{\"a\":1}");
    CHECK(Lsp::Framer::frame(two).find("Content-Length: 10\r\n") == 0);

    // Two messages in one read.
    Lsp::Framer f;
    f.feed(Lsp::Framer::frame(one) + Lsp::Framer::frame(two));
    std::string body;
    CHECK(f.next(body) && body == one);
    CHECK(f.next(body) && body == two);
    CHECK(!f.next(body));

    // One message dribbled in a byte at a time, header and body both split.
    std::string framed = Lsp::Framer::frame(two);
    Lsp::Framer g;
    int got = 0;
    for (char c : framed) {
        g.feed(&c, 1);
        while (g.next(body)) { got++; CHECK(body == two); }
    }
    CHECK(got == 1 && g.buffered() == 0);

    // Other headers, any case, and a message with an unusable header skipped.
    Lsp::Framer h;
    h.feed("content-type: application/vscode-jsonrpc; charset=utf-8\r\n"
           "CONTENT-LENGTH:7\r\n\r\n{\"a\":1}"
           "X-Nothing: 1\r\n\r\n"
           "Content-Length: 2\r\n\r\n[]");
    CHECK(h.next(body) && body == one);
    CHECK(h.next(body) && body == "[]");
    CHECK(!h.next(body));

    // Many messages: the consumed prefix is dropped, not kept forever.
    Lsp::Framer m;
    bool all = true;
    for (int i = 0; i < 1000; i++) {
        m.feed(Lsp::Framer::frame(one));
        if (!m.next(body) || body != one) all = false;
    }
    CHECK(all && m.buffered() == 0);
}

void testLspPositions() {
    GROUP("lsp:positions");
    using Lsp::Position;
    std::u16string t = Lsp::toUtf16("ab\ncd\r\nef\rgh");
    CHECK(Lsp::offsetForPosition(t, {0, 0}) == 0);
    CHECK(Lsp::offsetForPosition(t, {0, 2}) == 2);
    CHECK(Lsp::offsetForPosition(t, {0, 9}) == 2);    // clamps to the line end
    CHECK(Lsp::offsetForPosition(t, {1, 1}) == 4);
    CHECK(Lsp::offsetForPosition(t, {2, 0}) == 7);    // after \r\n
    CHECK(Lsp::offsetForPosition(t, {3, 2}) == 12);   // after a lone \r
    CHECK(Lsp::offsetForPosition(t, {9, 0}) == t.size());
    CHECK(Lsp::positionForOffset(t, 0) == (Position{0, 0}));
    CHECK(Lsp::positionForOffset(t, 4) == (Position{1, 1}));
    CHECK(Lsp::positionForOffset(t, 7) == (Position{2, 0}));
    CHECK(Lsp::positionForOffset(t, 11) == (Position{3, 1}));
    CHECK(Lsp::positionForOffset(t, 999) == (Position{3, 2}));
    // Round trip at every offset except inside \r\n.
    bool roundTrip = true;
    for (size_t i = 0; i <= t.size(); i++) {
        if (i == 6) continue;   // between \r and \n
        if (Lsp::offsetForPosition(t, Lsp::positionForOffset(t, i)) != i) roundTrip = false;
    }
    CHECK(roundTrip);

    // UTF-16 code units: é is one unit, 😀 is two (a surrogate pair), exactly
    // as NSString counts them. The server sees UTF-8, positions stay UTF-16.
    std::u16string u = Lsp::toUtf16("caf\xC3\xA9 \xF0\x9F\x98\x80 x\nnext");
    CHECK(u.size() == 14);
    CHECK(Lsp::positionForOffset(u, 8) == (Position{0, 8}));    // the x
    CHECK(u[8] == u'x');
    CHECK(Lsp::offsetForPosition(u, {1, 0}) == 10);
    CHECK(Lsp::toUtf8(u) == "caf\xC3\xA9 \xF0\x9F\x98\x80 x\nnext");
    CHECK(Lsp::toUtf16("\xFF").size() == 1 && Lsp::toUtf16("\xFF")[0] == 0xFFFD);

    GROUP("lsp:uri");
    CHECK(Lsp::uriFromPath("/Users/me/a b/c#.cpp") ==
          "file:///Users/me/a%20b/c%23.cpp");
    CHECK(Lsp::pathFromUri("file:///Users/me/a%20b/c%23.cpp") == "/Users/me/a b/c#.cpp");
    CHECK(Lsp::pathFromUri(Lsp::uriFromPath("/tmp/caf\xC3\xA9/x+y.h")) ==
          "/tmp/caf\xC3\xA9/x+y.h");
    CHECK(Lsp::uriFromPath("/tmp/caf\xC3\xA9") == "file:///tmp/caf%C3%A9");
    CHECK(Lsp::pathFromUri("file://localhost/etc/hosts") == "/etc/hosts");
    CHECK(Lsp::pathFromUri("FILE:///x%2") == "/x%2");   // bad escape kept literally
    CHECK(Lsp::pathFromUri("https://example.com/x").empty());
    CHECK(Lsp::pathFromUri("file:///a/b.cpp#L3") == "/a/b.cpp");
}

void testLspServers() {
    GROUP("lsp:servers");
    Lsp::Language l;
    CHECK(Lsp::languageForExtension("cpp", l) && l.server == "cpp" && l.languageId == "cpp");
    CHECK(Lsp::languageForExtension("C", l) && l.server == "cpp" && l.languageId == "c");
    CHECK(Lsp::languageForExtension("mm", l) && l.languageId == "objective-cpp");
    CHECK(Lsp::languageForExtension("h", l) && l.server == "cpp");
    CHECK(Lsp::languageForExtension("py", l) && l.server == "python");
    CHECK(Lsp::languageForExtension("go", l) && l.server == "go");
    CHECK(Lsp::languageForExtension("rs", l) && l.server == "rust");
    CHECK(Lsp::languageForExtension("tsx", l) && l.server == "typescript" &&
          l.languageId == "typescriptreact");
    CHECK(Lsp::languageForExtension("js", l) && l.languageId == "javascript");
    CHECK(!Lsp::languageForExtension("md", l));
    CHECK(!Lsp::languageForExtension("tex", l));
    CHECK(!Lsp::languageForExtension("png", l));
    CHECK(!Lsp::languageForExtension("", l));
    CHECK((Lsp::defaultCommands("python") ==
           std::vector<std::string>{"pyright-langserver --stdio", "pylsp"}));
    CHECK(Lsp::defaultCommands("cobol").empty());
    CHECK(Lsp::serverDisplayName("cpp") == "C/C++");
    // Every server a file can map to is one the settings file knows.
    for (const char *ext : {"c", "py", "go", "rs", "ts"}) {
        Lsp::languageForExtension(ext, l);
        CHECK(std::find(Settings::lspServers().begin(), Settings::lspServers().end(),
                        l.server) != Settings::lspServers().end());
    }

    GROUP("lsp:split-command");
    CHECK((Lsp::splitCommand("pyright-langserver --stdio") ==
           std::vector<std::string>{"pyright-langserver", "--stdio"}));
    CHECK((Lsp::splitCommand("  \"/opt/my tools/clangd\"  --log=error ") ==
           std::vector<std::string>{"/opt/my tools/clangd", "--log=error"}));
    CHECK((Lsp::splitCommand("a\\ b 'c \"d\"' \"e\\\"f\"") ==
           std::vector<std::string>{"a b", "c \"d\"", "e\"f"}));
    CHECK((Lsp::splitCommand("x ''") == std::vector<std::string>{"x", ""}));
    CHECK(Lsp::splitCommand("   ").empty());
}

void testLspParsing() {
    GROUP("lsp:diagnostics");
    std::string uri;
    std::vector<Lsp::Diagnostic> d = Lsp::parseDiagnostics(parsed(
        "{\"uri\":\"file:///a.cpp\",\"diagnostics\":["
        "{\"range\":{\"start\":{\"line\":2,\"character\":4},\"end\":{\"line\":2,\"character\":9}},"
        " \"severity\":1,\"message\":\"use of undeclared identifier 'x'\",\"source\":\"clang\","
        " \"code\":\"undeclared_var_use\"},"
        "{\"range\":{\"start\":{\"line\":5,\"character\":0},\"end\":{\"line\":5,\"character\":1}},"
        " \"severity\":2,\"message\":\"unused\",\"code\":42},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}},"
        " \"message\":\"no severity\"}]}"), &uri);
    CHECK(uri == "file:///a.cpp");
    CHECK(d.size() == 3);
    CHECK(d.size() == 3 && d[0].range.start == (Lsp::Position{2, 4}) &&
          d[0].range.end == (Lsp::Position{2, 9}));
    CHECK(d.size() == 3 && d[0].severity == Lsp::Severity::Error && d[0].source == "clang");
    CHECK(d.size() == 3 && d[1].severity == Lsp::Severity::Warning && d[1].code == "42");
    CHECK(d.size() == 3 && d[2].severity == Lsp::Severity::Error);   // omitted -> error
    CHECK(Lsp::parseDiagnostics(parsed("{\"uri\":\"u\",\"diagnostics\":[]}")).empty());

    GROUP("lsp:completion");
    bool incomplete = false;
    std::vector<Lsp::CompletionItem> c = Lsp::parseCompletion(parsed(
        "{\"isIncomplete\":true,\"items\":["
        "{\"label\":\" size\",\"sortText\":\"2\",\"kind\":2,\"detail\":\"int\","
        " \"textEdit\":{\"range\":{\"start\":{\"line\":3,\"character\":6},"
        "\"end\":{\"line\":3,\"character\":6}},\"newText\":\"size()\"}},"
        "{\"label\":\"\xE2\x80\xA2" "push_back\",\"sortText\":\"1\",\"insertText\":\"push_back\"},"
        "{\"label\":\"empty\",\"filterText\":\"isEmpty\"},"
        "{\"label\":\"\"},"
        "{\"label\":\"at\",\"textEdit\":{\"insert\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":1,\"character\":3}},\"replace\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":1,\"character\":5}},\"newText\":\"at\"}}]}"), &incomplete);
    CHECK(incomplete);
    CHECK(c.size() == 4);   // the empty label is dropped
    // Sorted by sortText, falling back to the label: "1", "2", "at", "empty".
    CHECK(c.size() == 4 && c[0].label == "push_back" && c[1].label == "size");
    CHECK(c.size() == 4 && c[2].label == "at" && c[3].label == "empty");
    CHECK(c.size() == 4 && c[1].hasEdit && c[1].textToInsert() == "size()" &&
          c[1].editRange.start == (Lsp::Position{3, 6}));
    CHECK(c.size() == 4 && c[0].textToInsert() == "push_back" && !c[0].hasEdit);
    CHECK(c.size() == 4 && c[3].textToInsert() == "empty" && c[3].filterKey() == "isEmpty");
    CHECK(c.size() == 4 && c[2].hasEdit && c[2].editRange.end == (Lsp::Position{1, 3}));
    CHECK(c.size() == 4 && c[1].detail == "int" && c[1].kind == 2);
    // A bare array, and null.
    CHECK(Lsp::parseCompletion(parsed("[{\"label\":\"x\"}]"), &incomplete).size() == 1 &&
          !incomplete);
    CHECK(Lsp::parseCompletion(Json()).empty());

    GROUP("lsp:completion-filter");
    std::vector<Lsp::CompletionItem> all = Lsp::parseCompletion(parsed(
        "[{\"label\":\"push_back\"},{\"label\":\"pop_back\"},{\"label\":\"size\"},"
        "{\"label\":\"Capacity\"},{\"label\":\"reserve\"}]"));
    auto labels = [](const std::vector<Lsp::CompletionItem> &v) {
        std::string s;
        for (const auto &i : v) s += i.label + " ";
        return s;
    };
    CHECK(Lsp::filterCompletions(all, "").size() == 5);
    CHECK(labels(Lsp::filterCompletions(all, "p")) == "pop_back push_back Capacity ");
    CHECK(labels(Lsp::filterCompletions(all, "cap")) == "Capacity ");   // any case
    CHECK(labels(Lsp::filterCompletions(all, "pb")) == "pop_back push_back ");
    // Prefix matches come before subsequence matches.
    CHECK(labels(Lsp::filterCompletions(all, "s")) == "size push_back reserve ");
    CHECK(Lsp::filterCompletions(all, "zz").empty());

    GROUP("lsp:hover");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"int x\\n\\nA count.\"}}")) ==
          "int x\n\nA count.");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":{\"kind\":\"markdown\",\"value\":\"### x\\n```cpp\\nint x\\n```\\n"
        "Uses \\\\_under\\\\_\"}}")) == "### x\nint x\nUses _under_");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":[{\"language\":\"python\",\"value\":\"def f()\"},\"Docs.\",\"\"]}")) ==
          "def f()\n\nDocs.");
    CHECK(Lsp::parseHover(parsed("{\"contents\":\"plain\"}")) == "plain");
    CHECK(Lsp::parseHover(Json()).empty());

    GROUP("lsp:definition");
    std::vector<Lsp::Location> loc = Lsp::parseLocations(parsed(
        "{\"uri\":\"file:///a.h\",\"range\":{\"start\":{\"line\":4,\"character\":7},"
        "\"end\":{\"line\":4,\"character\":12}}}"));
    CHECK(loc.size() == 1 && loc[0].uri == "file:///a.h" &&
          loc[0].range.start == (Lsp::Position{4, 7}));
    loc = Lsp::parseLocations(parsed(
        "[{\"targetUri\":\"file:///b.h\","
        "\"targetRange\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":9,\"character\":1}},"
        "\"targetSelectionRange\":{\"start\":{\"line\":2,\"character\":6},"
        "\"end\":{\"line\":2,\"character\":9}}},"
        "{\"uri\":\"file:///c.h\",\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}}]"));
    CHECK(loc.size() == 2 && loc[0].uri == "file:///b.h" &&
          loc[0].range.start == (Lsp::Position{2, 6}));   // the name, not the body
    CHECK(loc.size() == 2 && loc[1].uri == "file:///c.h");
    CHECK(Lsp::parseLocations(Json()).empty());
    CHECK(Lsp::parseLocations(parsed("[]")).empty());
}

// A scripted exchange: everything the client writes is collected and parsed
// back, and the "server" answers by feeding framed replies to receive().
struct FakeServer {
    std::vector<Json> sent;
    Lsp::Framer framer;
    void take(const std::string &bytes) {
        framer.feed(bytes);
        std::string body;
        while (framer.next(body)) sent.push_back(parsed(body));
    }
    std::vector<std::string> methods() const {
        std::vector<std::string> m;
        for (const Json &j : sent) m.push_back(j["method"].asString());
        return m;
    }
};

std::string reply(int id, const std::string &resultJson) {
    return Lsp::Framer::frame("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                              ",\"result\":" + resultJson + "}");
}

void testLspClient() {
    GROUP("lsp:client-handshake");
    FakeServer srv;
    Lsp::Client client([&](const std::string &b) { srv.take(b); });
    bool ready = false;
    client.onReady = [&] { ready = true; };
    CHECK(client.state() == Lsp::Client::State::Idle);

    client.initialize("/Users/me/my proj", 1234);
    CHECK(client.state() == Lsp::Client::State::Initializing);
    CHECK(srv.sent.size() == 1);
    const Json &init = srv.sent[0];
    CHECK(init["method"].asString() == "initialize" && init["id"].asInt() == 1);
    CHECK(init["jsonrpc"].asString() == "2.0");
    CHECK(init["params"]["processId"].asInt() == 1234);
    CHECK(init["params"]["rootUri"].asString() == "file:///Users/me/my%20proj");
    CHECK(init["params"]["workspaceFolders"][0]["name"].asString() == "my proj");
    CHECK(!init["params"]["capabilities"]["textDocument"]["completion"]
                ["completionItem"]["snippetSupport"].asBool(true));

    // Before the server answers, document traffic and requests wait.
    client.didOpen("file:///a.cpp", "cpp", "int x;");
    int hoverResult = 0;
    int hid = client.hover("file:///a.cpp", {0, 4}, [&](const Json &r, const Json &) {
        hoverResult = (int)r["n"].asInt();
    });
    CHECK(srv.sent.size() == 1 && client.queuedMessages() == 2);

    // A server request arriving mid-handshake is answered right away.
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":\"p1\",\"method\":\"window/workDoneProgress/create\","
        "\"params\":{}}"));
    CHECK(srv.sent.size() == 2 && srv.sent[1]["id"].asString() == "p1" &&
          srv.sent[1].has("result"));

    client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":{\"save\":"
                            "{\"includeText\":true}},\"completionProvider\":"
                            "{\"triggerCharacters\":[\".\",\">\",\":\"]}},"
                            "\"serverInfo\":{\"name\":\"clangd\"}}"));
    CHECK(ready && client.state() == Lsp::Client::State::Ready);
    CHECK(client.serverName() == "clangd");
    CHECK((client.completionTriggers() == std::vector<std::string>{".", ">", ":"}));
    CHECK((srv.methods() == std::vector<std::string>{
        "initialize", "", "initialized", "textDocument/didOpen", "textDocument/hover"}));
    CHECK(client.queuedMessages() == 0);
    CHECK(srv.sent.size() == 5 &&
          srv.sent[3]["params"]["textDocument"]["text"].asString() == "int x;" &&
          srv.sent[3]["params"]["textDocument"]["version"].asInt() == 1 &&
          srv.sent[3]["params"]["textDocument"]["languageId"].asString() == "cpp");
    CHECK(srv.sent.size() == 5 && srv.sent[4]["id"].asInt() == hid);

    GROUP("lsp:client-sync");
    size_t before = srv.sent.size();
    client.didChange("file:///a.cpp", "int x;");   // unchanged: nothing sent
    CHECK(srv.sent.size() == before);
    client.didChange("file:///other.cpp", "x");    // not open: nothing sent
    CHECK(srv.sent.size() == before);
    client.didChange("file:///a.cpp", "int xy;");
    CHECK(srv.sent.size() == before + 1);
    const Json &ch = srv.sent.back();
    CHECK(ch["method"].asString() == "textDocument/didChange");
    CHECK(ch["params"]["textDocument"]["version"].asInt() == 2);
    CHECK(ch["params"]["contentChanges"][0]["text"].asString() == "int xy;");
    CHECK(client.version("file:///a.cpp") == 2);
    // Save sends pending text first, and includes it because the server asked.
    client.didSave("file:///a.cpp", "int xyz;");
    CHECK((srv.methods().back() == "textDocument/didSave"));
    CHECK(srv.sent.back()["params"]["text"].asString() == "int xyz;");
    CHECK(srv.sent[srv.sent.size() - 2]["method"].asString() == "textDocument/didChange");
    CHECK(client.version("file:///a.cpp") == 3);
    // Opening an open document again is a change, not a second didOpen.
    client.didOpen("file:///a.cpp", "cpp", "int q;");
    CHECK(srv.methods().back() == "textDocument/didChange");

    GROUP("lsp:client-responses");
    // Responses out of order each reach their own handler.
    std::string order;
    int a = client.definition("file:///a.cpp", {0, 4}, [&](const Json &r, const Json &) {
        order += "a" + std::to_string(r.size());
    });
    int b = client.completion("file:///a.cpp", {0, 5}, [&](const Json &r, const Json &e) {
        order += "b" + std::to_string(r.size()) + (e.isNull() ? "" : "E");
    }, ".");
    CHECK(srv.sent.back()["params"]["context"]["triggerKind"].asInt() == 2);
    CHECK(srv.sent.back()["params"]["context"]["triggerCharacter"].asString() == ".");
    CHECK(srv.sent.back()["params"]["position"]["character"].asInt() == 5);
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(b) +
        ",\"error\":{\"code\":-32603,\"message\":\"boom\"}}") + reply(a, "[1,2]"));
    CHECK(order == "b0Ea2");
    client.receive(reply(hid, "{\"n\":7}"));   // the request queued before Ready
    CHECK(hoverResult == 7);
    // A reply nobody asked for is ignored.
    client.receive(reply(999, "null"));
    CHECK(client.pendingRequests() == 0);

    // Cancelled requests never reach their handler.
    bool ran = false;
    int c = client.hover("file:///a.cpp", {0, 0}, [&](const Json &, const Json &) { ran = true; });
    client.cancel(c);
    CHECK(srv.methods().back() == "$/cancelRequest" &&
          srv.sent.back()["params"]["id"].asInt() == c);
    client.receive(reply(c, "{}"));
    CHECK(!ran);

    GROUP("lsp:client-notifications");
    std::string diagUri;
    size_t diagCount = 0;
    client.onDiagnostics = [&](const std::string &u, const std::vector<Lsp::Diagnostic> &d) {
        diagUri = u;
        diagCount = d.size();
    };
    // A garbled message is reported, and the next one still gets through.
    std::string problem;
    client.onProtocolError = [&](const std::string &p) { problem = p; };
    client.receive(Lsp::Framer::frame("{not json") + Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
        "{\"uri\":\"file:///a.cpp\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},\"message\":\"m\"}]}}"));
    CHECK(!problem.empty());
    CHECK(diagUri == "file:///a.cpp" && diagCount == 1);

    // Server requests: configuration gets one null per item; unknown methods
    // get MethodNotFound so the server isn't left waiting.
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"workspace/configuration\","
        "\"params\":{\"items\":[{},{}]}}"));
    CHECK(srv.sent.back()["id"].asInt() == 5 && srv.sent.back()["result"].size() == 2);
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"workspace/applyEdit\",\"params\":{}}"));
    CHECK(srv.sent.back()["id"].asInt() == 6 &&
          srv.sent.back()["error"]["code"].asInt() == -32601);

    GROUP("lsp:client-close-shutdown");
    client.didClose("file:///a.cpp");
    CHECK(srv.methods().back() == "textDocument/didClose" && !client.isOpen("file:///a.cpp"));
    before = srv.sent.size();
    client.didClose("file:///a.cpp");   // twice: nothing
    CHECK(srv.sent.size() == before);

    bool done = false;
    client.shutdown([&] { done = true; });
    CHECK(client.state() == Lsp::Client::State::ShuttingDown);
    int sid = (int)srv.sent.back()["id"].asInt();
    CHECK(srv.methods().back() == "shutdown" && !done);
    // Nothing else goes out while shutting down.
    before = srv.sent.size();
    client.didOpen("file:///b.cpp", "cpp", "x");
    CHECK(client.hover("file:///b.cpp", {0, 0}, nullptr) == 0);
    CHECK(srv.sent.size() == before);
    client.receive(reply(sid, "null"));
    CHECK(done && client.state() == Lsp::Client::State::Exited);
    CHECK(srv.methods().back() == "exit");
    before = srv.sent.size();
    client.receive(Lsp::Framer::frame("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"x\"}"));
    CHECK(srv.sent.size() == before);   // an exited client answers nothing

    GROUP("lsp:client-exit-now");
    FakeServer s2;
    Lsp::Client quick([&](const std::string &b) { s2.take(b); });
    quick.initialize("/p", 1);
    quick.exitNow();   // before the handshake: exit only
    CHECK((s2.methods() == std::vector<std::string>{"initialize", "exit"}));
    FakeServer s3;
    Lsp::Client ready3([&](const std::string &b) { s3.take(b); });
    ready3.initialize("/p", 1);
    ready3.receive(reply(1, "{\"capabilities\":{}}"));
    ready3.exitNow();   // after it: shutdown and exit together, without waiting
    CHECK((s3.methods() == std::vector<std::string>{"initialize", "initialized",
                                                    "shutdown", "exit"}));
    bool d3 = false;
    ready3.shutdown([&] { d3 = true; });   // already exited: done at once
    CHECK(d3);

    // A failed initialize leaves the client exited, not waiting forever.
    FakeServer s4;
    Lsp::Client bad([&](const std::string &b) { s4.take(b); });
    std::string why;
    bad.onProtocolError = [&](const std::string &p) { why = p; };
    bad.initialize("/p", 1);
    bad.didOpen("file:///x.py", "python", "x");
    bad.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1,\"message\":\"no\"}}"));
    CHECK(bad.state() == Lsp::Client::State::Exited && why.find("no") != std::string::npos);
    CHECK(s4.sent.size() == 1 && bad.queuedMessages() == 0);
}

}  // namespace

// ---- TermLinks ---------------------------------------------------------------
static void testTermLinks() {
    using TermLinks::Link;
    auto one = [](const std::string& s) {
        auto v = TermLinks::find(s);
        return v.size() == 1 ? v[0] : Link{Link::Url, 0, 0, "<none>"};
    };
    // Compiler output: path, line, column, and the span stops before the message.
    {
        std::string s = "src/main.cpp:42:7: error: expected ';'";
        Link l = one(s);
        CHECK(l.kind == Link::File && l.target == "src/main.cpp");
        CHECK(l.line == 42 && l.column == 7);
        CHECK(s.substr(l.start, l.length) == "src/main.cpp:42:7");
    }
    { Link l = one("  at foo (lib/util.js:10:3)");
      CHECK(l.target == "lib/util.js" && l.line == 10 && l.column == 3); }
    { Link l = one("hello.py:3");
      CHECK(l.target == "hello.py" && l.line == 3 && l.column == 0); }
    // TypeScript and MSVC.
    { Link l = one("app.ts(12,5): error TS2322");
      CHECK(l.target == "app.ts" && l.line == 12 && l.column == 5); }
    // Python tracebacks.
    {
        std::string s = "  File \"/tmp/x/run.py\", line 17, in <module>";
        Link l = one(s);
        CHECK(l.target == "/tmp/x/run.py" && l.line == 17);
        CHECK(s.substr(l.start, l.length) == "/tmp/x/run.py");
    }
    // Plain paths, and punctuation that belongs to the sentence.
    { Link l = one("see ./docs/README.md.");  CHECK(l.target == "./docs/README.md"); }
    { Link l = one("~/code/notes.tex"); CHECK(l.target == "~/code/notes.tex"); }
    // URLs, with trailing punctuation and balanced parentheses.
    { Link l = one("docs at https://example.com/a?b=1.");
      CHECK(l.kind == Link::Url && l.target == "https://example.com/a?b=1"); }
    { Link l = one("(see https://en.wikipedia.org/wiki/Foo_(bar))");
      CHECK(l.target == "https://en.wikipedia.org/wiki/Foo_(bar)"); }
    { Link l = one("see https://en.wikipedia.org/wiki/Foo_(bar) for more");
      CHECK(l.target == "https://en.wikipedia.org/wiki/Foo_(bar)"); }
    { Link l = one("https://example.com/a_(b).");
      CHECK(l.target == "https://example.com/a_(b)"); }
    { Link l = one("(https://example.com/x)");
      CHECK(l.target == "https://example.com/x"); }
    // Not links: words, numbers, versions, a lone slash.
    CHECK(TermLinks::find("hello world").empty());
    CHECK(TermLinks::find("took 1.5 seconds").empty());
    CHECK(TermLinks::find("version 2.0.1").empty());
    CHECK(TermLinks::find("a / b").empty());
    // Several on a line, in order, and at().
    {
        std::string s = "a.cpp:1 b.h:2 https://x.io";
        auto v = TermLinks::find(s);
        CHECK(v.size() == 3);
        if (v.size() == 3) {
            CHECK(v[0].target == "a.cpp" && v[1].target == "b.h" &&
                  v[2].kind == Link::Url);
            CHECK(TermLinks::at(v, s.find("b.h") + 1) == &v[1]);
            CHECK(TermLinks::at(v, s.find(' ')) == nullptr);
        }
    }
    // Non-ASCII names are found whole.
    { Link l = one("données/résumé.tex:3"); CHECK(l.target == "données/résumé.tex" && l.line == 3); }
    // How Claude Code names files: in parentheses after a tool name, and
    // with a line in its summaries. The tool name itself is not a path.
    {
        std::string s = "⏺ Update(src/Terminal.mm)";
        Link l = one(s);
        CHECK(l.target == "src/Terminal.mm" && l.line == 0);
        CHECK(s.substr(l.start, l.length) == "src/Terminal.mm");
    }
    { Link l = one("  ⎿  Read src/main.mm:120 (40 lines)");
      CHECK(l.target == "src/main.mm" && l.line == 120); }
    // Inside backticks, as in Markdown output.
    { Link l = one("see `docs/demos/tour.gif` for it");
      CHECK(l.target == "docs/demos/tour.gif"); }
}

// ------------------------------------------------------------ folder search
namespace {

namespace fsys = std::filesystem;

void writeFile(const fsys::path &p, const std::string &bytes) {
    fsys::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

// Matches as "relative:line:column", in the order the search returned them.
std::vector<std::string> hitKeys(const FolderSearchResult &r) {
    std::vector<std::string> v;
    for (const auto &m : r.matches)
        v.push_back(m.relativePath + ":" + std::to_string(m.line) + ":" +
                    std::to_string(m.column));
    return v;
}

bool hasHit(const FolderSearchResult &r, const std::string &key) {
    auto v = hitKeys(r);
    return std::find(v.begin(), v.end(), key) != v.end();
}

bool anyHitIn(const FolderSearchResult &r, const std::string &relPrefix) {
    for (const auto &m : r.matches)
        if (m.relativePath.compare(0, relPrefix.size(), relPrefix) == 0) return true;
    return false;
}

void testFolderSearch() {
    using namespace FolderSearch;

    GROUP("folder-search:query");
    CHECK(normalizeQuery("  hello \t") == "hello");
    CHECK(normalizeQuery("\xC2\xA0x y\xE3\x80\x80") == "x y");   // NBSP, ideographic space
    CHECK(!isSearchable(""));
    CHECK(!isSearchable("a"));
    CHECK(!isSearchable("  a  "));
    CHECK(isSearchable("ab"));
    CHECK(isSearchable(" ab "));
    CHECK(!isSearchable("\xC3\xA9"));          // é: two bytes, one character
    CHECK(isSearchable("\xC3\xA9\xC3\xA9"));
    CHECK(!isSearchable("\xFF\xFE"));          // not UTF-8: never searched

    GROUP("folder-search:skip");
    CHECK(isSkippedDirectory("node_modules"));
    CHECK(isSkippedDirectory("build"));
    CHECK(isSkippedDirectory("__pycache__"));
    CHECK(isSkippedDirectory("DerivedData"));
    CHECK(isSkippedDirectory("venv"));
    CHECK(!isSkippedDirectory("src"));
    CHECK(!isSkippedDirectory("Build"));       // the Mac's list is case-sensitive

    GROUP("folder-search:utf8");
    CHECK(isValidUtf8("plain"));
    CHECK(isValidUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
    CHECK(isValidUtf8(std::string("nul\0inside", 10)));   // NSString accepts NUL too
    CHECK(!isValidUtf8("\xC0\x80"));           // overlong NUL
    CHECK(!isValidUtf8("\xED\xA0\x80"));       // surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80"));   // past U+10FFFF
    CHECK(!isValidUtf8("\xE2\x82"));           // truncated
    CHECK(!isValidUtf8("\x80"));               // stray continuation byte

    GROUP("folder-search:clean");
    CHECK(cleanLine("\x1b[31mred\x1b[0m text") == "red text");
    CHECK(cleanLine("\x1b[1;32;40mbold\x1b[m") == "bold");
    CHECK(cleanLine("a\x07" "b\tc") == "abc");            // bell and tab are controls
    CHECK(cleanLine("\x1b[31") == "[31");                 // no final byte: only ESC goes
    CHECK(cleanLine("x\xE2\x80\xAEy") == "xy");           // RIGHT-TO-LEFT OVERRIDE
    CHECK(cleanLine("\xEF\xBB\xBFtop") == "top");         // byte-order mark
    CHECK(cleanLine("caf\xC3\xA9") == "caf\xC3\xA9");     // letters untouched
    CHECK(cleanLine("a\xC2\x85" "b") == "ab");            // C1 NEL
    CHECK(displayText("   \t  indented  ") == "indented");
    CHECK(displayText("\x1b[32m   green") == "green");     // cleaned, then trimmed
    {
        std::string longLine(300, 'x');
        CHECK(displayText(longLine).size() == 200);
        std::string wide;
        for (int i = 0; i < 250; ++i) wide += "\xC3\xA9";   // 250 two-byte chars
        const std::string cut = displayText(wide);
        CHECK(cut.size() == 400);                           // 200 whole characters
        CHECK(isValidUtf8(cut));
        CHECK(displayText("abcdef", 3) == "abc");
    }

    GROUP("folder-search:match");
    std::size_t col = 99, len = 99;
    CHECK(findInLine("Hello World", "world", false, &col, &len) && col == 6 && len == 5);
    CHECK(findInLine("HELLO", "hello", false, &col, &len) && col == 0);
    CHECK(!findInLine("Hello World", "world", true, &col, &len));
    CHECK(findInLine("Hello World", "World", true, &col, &len) && col == 6);
    CHECK(!findInLine("hel lo", "hello", false, &col, &len));
    CHECK(!findInLine("he", "hello", false, &col, &len));
    // ASCII query after multi-byte text: the byte column counts bytes.
    CHECK(findInLine("\xC3\xA9\xC3\xA9TODO", "todo", false, &col, &len) && col == 4 && len == 4);
    // Latin-1, Greek and Cyrillic fold; match length is in the line's bytes.
    CHECK(findInLine("\xC3\x89" "COLE", "\xC3\xA9" "cole", false, &col, &len) &&
          col == 0 && len == 6);
    CHECK(findInLine("x \xCE\xA3\xCE\x99\xCE\x93\xCE\x9C\xCE\x91", "\xCF\x83\xCE\xB9\xCE\xB3",
                     false, &col, &len) && col == 2 && len == 6);   // ΣΙΓΜΑ / σιγ
    CHECK(findInLine("\xD0\x9C\xD0\x98\xD0\xA0", "\xD0\xBC\xD0\xB8\xD1\x80",
                     false, &col, &len) && len == 6);               // МИР / мир
    CHECK(findInLine("\xC5\x81" "\xC3\xB3" "d\xC5\xBA", "\xC5\x82\xC3\xB3" "d\xC5\xBA",
                     false, &col, &len));                           // Łódź / łódź
    CHECK(!findInLine("\xC3\x89" "COLE", "\xC3\xA9" "cole", true, &col, &len));

    // ---- a real folder -------------------------------------------------
    std::random_device rd;
    const fsys::path root = fsys::temp_directory_path() /
        ("minicode-search-test-" + std::to_string(rd()));
    fsys::create_directories(root);
    const fsys::path outside = fsys::temp_directory_path() /
        ("minicode-search-outside-" + std::to_string(rd()));
    fsys::create_directories(outside);

    writeFile(root / "a.txt", "hello world\nnothing here\n  Second HELLO\n");
    writeFile(root / "sub" / "b.cpp", "int x; // hello\r\nfoo\rbar\xE2\x80\xA9" "and hello again");
    writeFile(root / "sub" / "deeper" / "c.md", "\xC3\xA9\xC3\xA9hello\n");
    writeFile(root / "ansi.log", "\x1b[32m  hello green\x1b[0m\n");
    writeFile(root / "nul.txt", std::string("hello\0there", 11));
    writeFile(root / "build.txt", "hello from a file named like a skipped folder\n");
    writeFile(root / "bom.txt", "\xEF\xBB\xBFhello bom\n");
    writeFile(root / ".dotfile", "hello hidden\n");
    writeFile(root / ".hidden" / "x.txt", "hello hidden dir\n");
    writeFile(root / ".git" / "config", "hello git\n");
    for (const char *d : {"node_modules", "build", "dist", "__pycache__", "venv",
                          "DerivedData"})
        writeFile(root / d / "x.txt", "hello skipped\n");
    writeFile(root / "binary.dat", std::string("hello\xFF\xFE\x00\x01", 9));
    writeFile(root / "latin1.txt", "hello caf\xE9\n");            // not UTF-8
    writeFile(root / "big.txt", "hello\n" + std::string(1024 * 1024, 'x'));
    writeFile(root / "exactly1mb.txt",
              "hello\n" + std::string(1024 * 1024 - 6, 'y'));     // at the limit: searched
    writeFile(root / "long.txt", std::string(50, ' ') + "hello" + std::string(400, 'z'));
    writeFile(outside / "linked.txt", "hello through a symlink\n");

    std::error_code ec;
    fsys::create_directory_symlink(root, root / "sub" / "loop", ec);        // back to root
    const bool loopMade = !ec;
    fsys::create_directory_symlink(outside, root / "zlink", ec);           // out of the tree
    const bool linkMade = !ec;
    fsys::create_symlink(root / "missing", root / "broken", ec);            // dangling
#ifndef _WIN32
    const bool fifoMade = mkfifo((root / "pipe.txt").c_str(), 0600) == 0;  // must not block
#else
    const bool fifoMade = false;
#endif

    GROUP("folder-search:tree");
    FolderSearchResult r = search(root.string(), "hello");
    CHECK(!r.cancelled);
    CHECK(!r.truncated);
    CHECK(hasHit(r, "a.txt:1:1"));
    CHECK(hasHit(r, "a.txt:3:10"));                      // case-insensitive, raw column
    CHECK(hasHit(r, "sub/b.cpp:1:11"));
    CHECK(hasHit(r, "sub/b.cpp:4:5"));                   // \r\n, \r and U+2029 end lines
    CHECK(hasHit(r, "sub/deeper/c.md:1:3"));             // column in characters
    CHECK(hasHit(r, "ansi.log:1:8"));                    // the raw line holds the escape
    CHECK(hasHit(r, "nul.txt:1:1"));
    CHECK(hasHit(r, "build.txt:1:1"));                   // only folders are skipped
    CHECK(hasHit(r, "bom.txt:1:2"));                     // the BOM is a character in line 1
    CHECK(hasHit(r, "exactly1mb.txt:1:1"));
    CHECK(hasHit(r, "long.txt:1:51"));
    CHECK(!anyHitIn(r, ".dotfile"));
    CHECK(!anyHitIn(r, ".hidden"));
    CHECK(!anyHitIn(r, ".git"));
    CHECK(!anyHitIn(r, "node_modules"));
    CHECK(!anyHitIn(r, "build/"));
    CHECK(!anyHitIn(r, "dist"));
    CHECK(!anyHitIn(r, "__pycache__"));
    CHECK(!anyHitIn(r, "venv"));
    CHECK(!anyHitIn(r, "DerivedData"));
    CHECK(!anyHitIn(r, "binary.dat"));
    CHECK(!anyHitIn(r, "latin1.txt"));
    CHECK(!anyHitIn(r, "big.txt"));
    CHECK(!anyHitIn(r, "sub/loop"));                     // the loop back to root is not re-walked
    CHECK(!anyHitIn(r, "broken"));
    CHECK(!anyHitIn(r, "pipe.txt"));
    CHECK(loopMade);
    CHECK(fifoMade);
    if (linkMade) CHECK(hasHit(r, "zlink/linked.txt:1:1"));   // symlinked folders are followed
    CHECK(r.matches.size() == (linkMade ? 12u : 11u));
    CHECK(r.filesMatched == r.matches.size() - 2);       // a.txt and b.cpp have two
    CHECK(r.filesSearched >= r.filesMatched);

    // Stable order: files before folders, names in byte order, lines ascending.
    {
        auto keys = hitKeys(r);
        CHECK(!keys.empty() && keys.front() == "a.txt:1:1");
        auto pos = [&](const std::string &k) {
            return std::find(keys.begin(), keys.end(), k) - keys.begin();
        };
        CHECK(pos("a.txt:1:1") < pos("a.txt:3:10"));
        CHECK(pos("long.txt:1:51") < pos("sub/b.cpp:1:11"));    // files first
        CHECK(pos("sub/b.cpp:4:5") < pos("sub/deeper/c.md:1:3"));
    }

    // Match details: full path, byte offsets and the cleaned display text.
    for (const auto &m : r.matches) {
        if (m.relativePath == "ansi.log") {
            CHECK(m.text == "hello green");
            CHECK(m.byteColumn == 7 && m.byteLength == 5);
            CHECK(m.path == (root / "ansi.log").string());
        }
        if (m.relativePath == "sub/deeper/c.md") CHECK(m.byteColumn == 4);
        if (m.relativePath == "long.txt") {
            CHECK(m.text.size() == 200);
            CHECK(m.text.compare(0, 5, "hello") == 0);          // trimmed first
        }
        if (m.relativePath == "bom.txt") CHECK(m.text == "hello bom");
        if (m.relativePath == "nul.txt") CHECK(m.text == "hellothere");
    }

    GROUP("folder-search:options");
    CHECK(search(root.string(), "h").matches.empty());       // under two characters
    CHECK(search(root.string(), "   ").matches.empty());
    CHECK(search((root / "nope").string(), "hello").matches.empty());
    CHECK(search((root / "a.txt").string(), "hello").matches.empty());   // not a folder
    CHECK(search(root.string(), "  second hello ").matches.size() == 1); // trimmed query
    {
        FolderSearchOptions o;
        o.caseSensitive = true;
        auto cs = search(root.string(), "HELLO", nullptr, o);
        CHECK(cs.matches.size() == 1 && hasHit(cs, "a.txt:3:10"));
    }
    {
        FolderSearchOptions o;
        o.maxMatches = 3;
        auto capped = search(root.string(), "hello", nullptr, o);
        CHECK(capped.matches.size() == 3);
        CHECK(capped.truncated);
        CHECK(hitKeys(capped).front() == "a.txt:1:1");
    }
    {
        // The scope folder itself is searched even when its name is on the
        // skip list, as on the Mac.
        auto inBuild = search((root / "build").string(), "hello");
        CHECK(inBuild.matches.size() == 1 && inBuild.matches[0].relativePath == "x.txt");
        // And a root given with a trailing slash joins paths cleanly.
        auto slashed = search((root / "sub").string() + "/", "again");
        CHECK(slashed.matches.size() == 1 &&
              slashed.matches[0].path == (root / "sub" / "b.cpp").string());
    }

    GROUP("folder-search:cancel");
    {
        std::atomic<bool> stopNow{true};
        auto c = search(root.string(), "hello", &stopNow);
        CHECK(c.cancelled);
        CHECK(c.matches.empty());
        std::atomic<bool> keepGoing{false};
        auto k = search(root.string(), "hello", &keepGoing);
        CHECK(!k.cancelled && k.matches.size() == r.matches.size());
    }
    {
        // Cancelled from another thread while a large tree is being searched:
        // it stops early and says so.
        const fsys::path many = root / "many";
        for (int d = 0; d < 40; ++d)
            for (int f = 0; f < 25; ++f)
                writeFile(many / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt"),
                          std::string(20000, 'q') + "\nneedle\n");
        std::atomic<bool> flag{false};
        std::thread t([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            flag = true;
        });
        auto c = search(many.string(), "needle", &flag);
        t.join();
        CHECK(c.cancelled);
        CHECK(c.matches.size() < 1000);
        auto whole = search(many.string(), "needle");
        CHECK(whole.matches.size() == 1000 && !whole.cancelled);
    }

    fsys::remove_all(root, ec);
    fsys::remove_all(outside, ec);
}

}  // namespace

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testIncrementalHighlight();
    testTermLinks();
    benchIncrementalHighlight();
    testMarkdown();
    testTerminalStream();
    testTerminalScreen();
    testSettings();
    testSettingsColorEditing();
    testLineComments();
    testLatexDoc();
    testSyncTex();
    testJson();
    testLspFraming();
    testLspPositions();
    testLspServers();
    testLspParsing();
    testLspClient();
    testLatexClicks();
    testSyncTexText();
    testFolderSearch();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
