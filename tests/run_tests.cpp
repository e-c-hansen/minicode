// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include "TerminalStream.h"
#include "Settings.h"
#include "LineComments.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include "LegacyHighlighter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

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

// The reference tokens for a document: the frozen whole-file lexer, cut at
// lines, in the document's own units.
std::vector<Token> reference(const std::string &text, const std::string &ext) {
    return splitAtLines(legacy::highlight(text, ext), text);
}
std::vector<Token> reference(const std::u16string &text, const std::string &ext) {
    std::vector<size_t> map;
    std::string u8 = toU8(text, &map);
    std::vector<Token> toks;
    for (const Token &t : legacy::highlight(u8, ext)) {
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
                          "scripts/release.sh", "tests/LegacyHighlighter.h"}) {
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
    const char *exts[] = {"cpp", "py", "js", "sh", "txt"};

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

    GROUP("incremental:random-edits");
    FuzzStats stats;
    unsigned seed = 1;
    for (const Sample &s : samples) {
        for (const char *e : exts) {
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
    CHECK(std::count(keys.begin(), keys.end(), '\n') == 33);   // 4 window, 18 panel, 7 syntax, 4 markdown
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

}  // namespace

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testIncrementalHighlight();
    benchIncrementalHighlight();
    testMarkdown();
    testTerminalStream();
    testSettings();
    testSettingsColorEditing();
    testLineComments();
    testLatexDoc();
    testSyncTex();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
