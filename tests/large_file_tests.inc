// large_file_tests.inc — included by run_tests.cpp. Long lines are not
// lexed (VS Code's editor.maxTokenizationLineLength), a lone '\r' and U+2029
// end lines in the highlighter as in LineIndex, and large-file mode's
// thresholds (VS Code's 20 MB or 300,000 lines).
#include "LargeFile.h"

namespace {

// Each '\n' of a text made another break: '\r' or U+2029.
std::u16string withBreaks(const std::string &text, char16_t brk) {
    std::u16string u = toU16(text);
    for (char16_t &c : u)
        if (c == u'\n') c = brk;
    return u;
}

// The tokens of a text, each as (style, the text it covers), so texts whose
// offsets differ compare.
std::vector<std::pair<int, std::u16string>> tokenTexts(const std::vector<Token> &toks,
                                                      const std::u16string &u) {
    std::vector<std::pair<int, std::u16string>> out;
    for (const Token &t : toks) out.push_back({(int)t.style, u.substr(t.start, t.length)});
    return out;
}

std::vector<Token> lexU16(const std::u16string &u, const std::string &ext) {
    IncrementalHighlighter<char16_t> h(ext);
    std::vector<Token> toks;
    h.reset(StringSource<char16_t>(u), &toks);
    return toks;
}

}  // namespace

void testLargeFiles() {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    GROUP("syntax:long-line");
    {
        // A 5,000,000-unit minified line between two ordinary ones: the long
        // line gets no tokens (plain), the lines around it are lexed, and a
        // comment opened above it carries past it unchanged.
        std::u16string longLine(5000000, u'a');
        for (size_t i = 0; i < longLine.size(); i += 100) longLine[i] = u'"';
        std::u16string text = u"int x = 1; // above\n" + longLine + u"\nint y = 2;\n";
        IncrementalHighlighter<char16_t> h("cpp");
        std::vector<Token> toks;
        auto t0 = Clock::now();
        h.reset(StringSource<char16_t>(text), &toks);
        const double full = ms(Clock::now() - t0);
        CHECK(h.lineCount() == 4);
        const size_t a = h.lineStart(1), b = h.lineStart(2);
        bool inLong = false, below = false;
        for (const Token &t : toks) {
            if (t.start < b && t.start + t.length > a) inLong = true;
            if (t.start >= b && t.style == TokenStyle::Number) below = true;
        }
        CHECK(!inLong);   // plain, one run of the default color
        CHECK(below);
        CHECK(h.endState(1) == h.endState(0));
        std::printf("  long line: a 5,000,000-unit line lexed (skipped) in %.2f ms\n", full);

        // A keystroke in it relexes nothing: no tokens come back, the range
        // is the long line alone, and it costs no read of the line.
        struct CountingSource : TextSource<char16_t> {
            const std::u16string &s;
            mutable size_t units = 0;
            explicit CountingSource(const std::u16string &t) : s(t) {}
            size_t length() const override { return s.size(); }
            void read(size_t pos, size_t len, char16_t *out) const override {
                units += len;
                s.copy(out, len, pos);
            }
        };
        std::vector<Token> out;
        const size_t at = a + 2500000;
        text.insert(at, 1, u'x');
        CountingSource src(text);
        t0 = Clock::now();
        auto r = h.edit(src, at, 0, 1, out);
        const double key = ms(Clock::now() - t0);
        CHECK(out.empty());
        CHECK(r.firstLine == 1 && r.endLine == 2);
        CHECK(src.units < 100);
        std::vector<Token> again;
        h.lineTokens(src, 1, 2, again);
        CHECK(again.empty());
        CHECK(src.units < 100);
        std::printf("  long line: a keystroke in it %.3f ms, %zu units read\n", key, src.units);

        // At the limit a line is still lexed; one unit past it, not. The
        // break does not count toward the length.
        for (size_t len : {kMaxTokenizationLineLength, kMaxTokenizationLineLength + 1}) {
            std::string line = "x = 1 # " + std::string(len - 8, 'c');
            for (const char *brk : {"\n", "\r\n", "\r"}) {
                std::string t = line + brk + "y = 2\n";
                std::vector<Token> tk = SyntaxHighlighter::highlight(t, "py");
                bool commented = false;
                for (const Token &k : tk)
                    if (k.style == TokenStyle::Comment && k.start == 6) commented = true;
                CHECK(commented == (len <= kMaxTokenizationLineLength));
                CHECK(!tk.empty() && tk.back().style == TokenStyle::Number);   // the 2 below
            }
        }
        // A string opened above a long line stays open past it, as VS Code
        // carries the state over a line it does not tokenize.
        std::string t = "s = '''open\n" + std::string(30000, 'z') + "\nstill''' + 1\n";
        IncrementalHighlighter<char> p("py");
        p.reset(StringSource<char>(t), nullptr);
        CHECK(p.endState(1).kind == LexState::TripleString);
        CHECK(p.endState(2).kind == LexState::Normal);
    }

    GROUP("syntax:lone-cr");
    {
        // A file with old Mac breaks, or U+2029, lexes as the same file with
        // '\n': the same tokens over the same text, and lines in the same
        // places (the frozen lexer read it all as one line: a '#' comment
        // ran to the end of the file).
        const char *samples[][2] = {
            {"py", "x = 1  # one\ny = '''a\nb''' + f\"{z}\"\n@dec\ndef g(): pass\n"},
            {"cpp", "#include <a>\nint a; // c\n/* b\nc */ int d = 0x1F;\nauto s = R\"(x\ny)\";\n"},
            {"js", "let a = `t\n${b}` / 2; // c\nconst r = /x/g;\n"},
            {"sh", "echo $HOME # c\nx='a\nb'\n"},
            {"tex", "\\section{A} % c\n$a\nb$ and \\[\nx\n\\]\n"},
        };
        for (auto &smp : samples) {
            const std::string ext = smp[0], lf = smp[1];
            const std::u16string u = toU16(lf);
            const auto want = tokenTexts(splitAtLines(lexU16(u, ext), u), u);
            for (char16_t brk : {u'\r', (char16_t)0x2029}) {
                const std::u16string v = withBreaks(lf, brk);
                std::vector<Token> got = lexU16(v, ext);
                auto texts = tokenTexts(got, v);
                for (auto &tt : texts)
                    for (char16_t &c : tt.second)
                        if (c == brk) c = u'\n';
                if (texts != want)
                    std::printf("    %s with break U+%04X differs\n", ext.c_str(), (unsigned)brk);
                CHECK(texts == want);
                IncrementalHighlighter<char16_t> h(ext), g(ext);
                h.reset(StringSource<char16_t>(v), nullptr);
                g.reset(StringSource<char16_t>(u), nullptr);
                bool same = h.lineCount() == g.lineCount();
                for (size_t k = 0; same && k < h.lineCount(); k++)
                    same = h.lineStart(k) == g.lineStart(k) && h.endState(k) == g.endState(k);
                CHECK(same);
            }
            // UTF-8: '\r' and U+2029's three bytes; the token before a
            // U+2029 that runs to the line's end takes all three.
            std::string cr = lf, ps;
            std::replace(cr.begin(), cr.end(), '\n', '\r');
            for (char c : lf) ps += c == '\n' ? std::string("\xE2\x80\xA9") : std::string(1, c);
            const auto wantU8 = SyntaxHighlighter::highlight(lf, ext);
            CHECK(SyntaxHighlighter::highlight(cr, ext) == wantU8);
            std::vector<Token> psToks = SyntaxHighlighter::highlight(ps, ext);
            CHECK(psToks.size() == wantU8.size());
            IncrementalHighlighter<char> h8(ext);
            h8.reset(StringSource<char>(ps), nullptr);
            CHECK(h8.lineCount() == (size_t)std::count(lf.begin(), lf.end(), '\n') + 1);
        }
        // A '#' comment ends at a lone '\r' and what follows is code.
        std::vector<Token> tk = SyntaxHighlighter::highlight("# note\rx = 42\r", "py");
        CHECK(tk.size() == 2 && tk[0].style == TokenStyle::Comment && tk[0].length == 6 &&
              tk[1].style == TokenStyle::Number && tk[1].start == 11);
        // "\r\n" stays one break; '\r' at the very end is a break before an
        // empty last line, as LineIndex has it.
        IncrementalHighlighter<char16_t> h("txt");
        h.reset(StringSource<char16_t>(u"a\r\nb\rc\r"), nullptr);
        CHECK(h.lineCount() == 4 && h.lineStart(1) == 3 && h.lineStart(2) == 5 &&
              h.lineStart(3) == 7);
        LineIndex li;
        const std::u16string lt = u"a\r\nb\rc\r";
        li.reset(lt.data(), lt.size());
        CHECK(li.lineCount() == h.lineCount());
    }

    GROUP("incremental:cr-edits");
    {
        // Edits that make and unmake a lone '\r': a '\n' typed after one
        // joins it into "\r\n" (one line fewer), deleting that '\n' splits
        // them again, and text typed between '\r' and '\n' does too.
        std::u16string t = u"# a\rb = 1\r# c\r\nd = 2\n";
        IncrementalHighlighter<char16_t> h("py");
        h.reset(StringSource<char16_t>(t), nullptr);
        auto agrees = [&](const char *what) {
            IncrementalHighlighter<char16_t> f("py");
            std::vector<Token> want, got;
            f.reset(StringSource<char16_t>(t), &want);
            h.lineTokens(StringSource<char16_t>(t), 0, h.lineCount(), got);
            bool ok = f.lineCount() == h.lineCount() && got == want;
            for (size_t k = 0; ok && k < h.lineCount(); k++)
                ok = f.lineStart(k) == h.lineStart(k) && f.endState(k) == h.endState(k);
            if (!ok) std::printf("    after %s the lines differ from a full lex\n", what);
            return ok;
        };
        std::vector<Token> out;
        CHECK(h.lineCount() == 5);
        t.insert(4, 1, u'\n');            // "# a\r\n": the break after "a" is now "\r\n"
        h.edit(StringSource<char16_t>(t), 4, 0, 1, out);
        CHECK(h.lineCount() == 5 && agrees("a '\\n' after a '\\r'"));
        t.erase(4, 1);
        h.edit(StringSource<char16_t>(t), 4, 1, 0, out);
        CHECK(h.lineCount() == 5 && agrees("deleting it"));
        const size_t crlf = t.find(u"\r\n");
        t.insert(crlf + 1, u"x");         // "\rx\n": two breaks now
        h.edit(StringSource<char16_t>(t), crlf + 1, 0, 1, out);
        CHECK(h.lineCount() == 6 && agrees("typing between '\\r' and '\\n'"));
        t.erase(crlf + 1, 1);
        h.edit(StringSource<char16_t>(t), crlf + 1, 1, 0, out);
        CHECK(h.lineCount() == 5 && agrees("deleting that"));
        t.replace(0, 1, u"\r");           // a '\r' typed over the first '#'
        h.edit(StringSource<char16_t>(t), 0, 1, 1, out);
        CHECK(agrees("a '\\r' at the start"));
        // UTF-8: half of U+2029 typed, then the rest.
        std::string u8 = "x = 1 # c\xE2\x80";
        IncrementalHighlighter<char> g("py");
        g.reset(StringSource<char>(u8), nullptr);
        CHECK(g.lineCount() == 1);
        u8 += "\xA9y = 2";
        std::vector<Token> o8;
        auto r = g.edit(StringSource<char>(u8), 11, 0, 6, o8);
        CHECK(g.lineCount() == 2 && g.lineStart(1) == 12 && r.endLine == 2);
        CHECK(!o8.empty() && o8.back().style == TokenStyle::Number);
    }

    GROUP("fold:long-line");
    {
        // A quoted closer in a line too long to lex does not end the
        // function around it.
        std::string c = "int f() {\n  x();\n  s = \"}\" + \"" + std::string(30000, 'a') +
                        "\";\n  y();\n}\n";
        std::vector<Fold::Range> r = Fold::compute(toU16(c), "a.c");
        CHECK(r.size() == 1 && r[0].start == 0 && r[0].end == 3);
        // An ordinary line's quoted closer was never counted either.
        std::string shortLine = "int f() {\n  x();\n  s = \"}\";\n  y();\n}\n";
        r = Fold::compute(toU16(shortLine), "a.c");
        CHECK(r.size() == 1 && r[0].start == 0 && r[0].end == 3);
    }

    GROUP("large-file:thresholds");
    {
        using namespace LargeFile;
        CHECK(kSizeThreshold == 20u * 1024 * 1024 && kLineCountThreshold == 300000);
        CHECK(!isLarge(kSizeThreshold, kLineCountThreshold));
        CHECK(isLarge(kSizeThreshold + 1, 1) && isLarge(10, kLineCountThreshold + 1));
        // Lines as LineIndex counts them, and no further than asked.
        const std::u16string t = u"a\r\nb\rc\nd\x2029" u"e\r";
        CHECK(countLines(t.data(), t.size(), 100) == 6);
        CHECK(countLines(t.data(), t.size(), 2) == 3);
        std::u16string many(kLineCountThreshold, u'\n');   // 300,001 lines
        CHECK(isLargeText(many.data(), many.size()));
        many.pop_back();                                    // 300,000: not over
        CHECK(!isLargeText(many.data(), many.size()));
        std::u16string few(kLineCountThreshold - 1, u'x');
        CHECK(!isLargeText(few.data(), few.size()));
        // What the editor does with one.
        Mode m = modeFor(25u << 20, true, true, false);
        CHECK(m.large && m.featuresOff && !m.languageServer);
        m = modeFor(25u << 20, true, true, true);           // turned on for the tab
        CHECK(m.large && !m.featuresOff && m.languageServer);
        m = modeFor(25u << 20, true, false, false);         // the setting off
        CHECK(!m.featuresOff && m.languageServer);
        m = modeFor(60u << 20, true, false, false);         // past VS Code's sync limit
        CHECK(!m.featuresOff && !m.languageServer);
        m = modeFor(1000, false, true, false);
        CHECK(!m.large && !m.featuresOff && m.languageServer);
    }
}
