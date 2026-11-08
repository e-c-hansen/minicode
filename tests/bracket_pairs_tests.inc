// bracket_pairs_tests.inc — the `bracketpairs:*` groups of run_tests.cpp:
// bracket-pair colors (BracketPairs.h), depths kept up under random edits
// against a full recount done here without BracketPairs, splices across
// blocks, brackets in strings and comments, lines too long to lex,
// unexpected closers, and the cost of edits and queries in a 100,000-line
// file. Included there.
#include "BracketPairs.h"

namespace {

// The depth at each line's start and each bracket's depth (-1 unexpected),
// counted here from a full lex: brackets outside strings, escapes and
// comments, none on a line longer than the lexer's limit.
struct Recount {
    std::vector<int32_t> lineDepth;
    std::vector<BracketPairs::Bracket> brackets;
};
Recount recount(const std::u16string &text, const std::string &ext) {
    IncrementalHighlighter<char16_t> h(ext);
    std::vector<Token> toks;
    StringSource<char16_t> src(text);
    h.reset(src, &toks);
    std::vector<bool> code(text.size(), true);
    for (const Token &t : toks)
        if (isStringStyle(t.style) || t.style == TokenStyle::Comment)
            for (size_t k = t.start; k < t.start + t.length && k < text.size(); ++k)
                code[k] = false;
    Recount r;
    int32_t d = 0;
    for (size_t line = 0; line < h.lineCount(); ++line) {
        r.lineDepth.push_back(d);
        const size_t a = h.lineStart(line);
        const size_t b = line + 1 < h.lineCount() ? h.lineStart(line + 1) : text.size();
        size_t br = 0;   // the break's length
        if (b > a && text[b - 1] == u'\n') br = (b - a >= 2 && text[b - 2] == u'\r') ? 2 : 1;
        else if (b > a && (text[b - 1] == u'\r' || text[b - 1] == 0x2029)) br = 1;
        if (b - a - br > kMaxTokenizationLineLength) continue;
        for (size_t k = a; k < b; ++k) {
            const char16_t c = text[k];
            if (!code[k]) continue;
            if (c == u'(' || c == u'[' || c == u'{') {
                r.brackets.push_back({k, d});
                d++;
            } else if (c == u')' || c == u']' || c == u'}') {
                if (d == 0) {
                    r.brackets.push_back({k, -1});
                } else {
                    d--;
                    r.brackets.push_back({k, d});
                }
            }
        }
    }
    r.lineDepth.push_back(d);   // the end
    return r;
}

// Every bracket of the text by BracketPairs, from a full lex.
std::vector<BracketPairs::Bracket> pairsColors(const std::u16string &text,
                                               const std::string &ext) {
    IncrementalHighlighter<char16_t> h(ext);
    std::vector<Token> toks;
    StringSource<char16_t> src(text);
    h.reset(src, &toks);
    std::vector<BracketPairs::Bracket> out;
    BracketPairs::colorLines(src, h, 0, h.lineCount(), toks, 0, &out);
    return out;
}

bool sameBrackets(const std::vector<BracketPairs::Bracket> &a,
                  const std::vector<BracketPairs::Bracket> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].at != b[i].at || a[i].depth != b[i].depth) return false;
    return true;
}

}  // namespace

static void testBracketPairs() {
    using namespace BracketPairs;

    GROUP("bracketpairs:sums");
    {
        auto sum = [](const char *s) {
            std::u16string t = u16(s);
            return summarize(t.data(), t.size(), 0, nullptr, 0);
        };
        CHECK(sum("").net == 0 && sum("").floor == 0);
        CHECK(sum("((").net == 2 && sum("((").floor == 2);
        CHECK(sum(")").net == -1 && sum(")").floor == 0);
        CHECK(sum("))((").net == 0 && sum("))((").floor == 2);
        CHECK(sum(")").after(3) == 2 && sum(")").after(0) == 0);
        CHECK(sum("))((").after(1) == 2 && sum("))((").after(5) == 5);
        // Composing two lines is walking both.
        std::mt19937 rng(11);
        bool ok = true;
        for (int k = 0; k < 2000 && ok; ++k) {
            std::string a, b;
            for (int m = (int)(rng() % 8); m-- > 0;) a += "()"[rng() % 2];
            for (int m = (int)(rng() % 8); m-- > 0;) b += "()"[rng() % 2];
            const int d = (int)(rng() % 4);
            ok = sum(a.c_str()).then(sum(b.c_str())).after(d) == sum((a + b).c_str()).after(d) &&
                 sum(a.c_str()).then(sum(b.c_str())) == sum((a + b).c_str());
        }
        CHECK(ok);
    }

    GROUP("bracketpairs:colors");
    {
        auto colors = [](const char *s, const char *ext) {
            std::string m;
            for (const Bracket &b : pairsColors(u16(s), ext))
                m += b.depth < 0 ? 'u' : (char)('0' + b.depth);
            return m;
        };
        CHECK(colors("f(a[b{c}])", "cpp") == "012210");
        CHECK(colors("(\n  [\n  ]\n)", "cpp") == "0110");
        // A closer with nothing open is unexpected and leaves the depth at 0.
        CHECK(colors(") (x) ]", "cpp") == "u00u");
        CHECK(colors("(()))(", "cpp") == "0110u0");
        // All kinds count together.
        CHECK(colors("([)]", "cpp") == "0110");
        CHECK(colorIndex(0) == 0 && colorIndex(1) == 1 && colorIndex(2) == 2 &&
              colorIndex(3) == 0 && colorIndex(-1) == 3);
    }

    GROUP("bracketpairs:strings-comments");
    {
        auto colors = [](const char *s, const char *ext) {
            std::string m;
            for (const Bracket &b : pairsColors(u16(s), ext))
                m += b.depth < 0 ? 'u' : (char)('0' + b.depth);
            return m;
        };
        CHECK(colors("x = \"(\" + f(a) # (", "py") == "00");
        CHECK(colors("x = '\\(' + f(a)", "py") == "00");
        CHECK(colors("g(/* ) */ a) // (", "cpp") == "00");
        CHECK(colors("s = \"\"\"(\n)\"\"\" (x)", "py") == "00");
        CHECK(colors("r = /[(]/; f(x)", "js") == "00");
        // An f-string's field braces and the brackets inside count, its
        // text does not.
        CHECK(colors("f'({a[1]})'", "py") == "0110");
        CHECK(colors("`(${f(x)})`", "js") == "0110");
        // Without a grammar every bracket counts (C's rules).
        CHECK(colors("(x)", "txt") == "00");
    }

    GROUP("bracketpairs:long-line");
    {
        // A line longer than the lexer's limit has no colors and leaves the
        // depth alone; one of exactly the limit (a CRLF line too) is colored.
        std::u16string longLine(kMaxTokenizationLineLength + 1, u' ');
        longLine[0] = u'(';
        longLine[1] = u'(';
        std::u16string t = u"(\n" + longLine + u"\n)";
        std::vector<Bracket> b = pairsColors(t, "cpp");
        CHECK(b.size() == 2 && b[0].depth == 0 && b[1].depth == 0);
        std::u16string exact(kMaxTokenizationLineLength, u' ');
        exact[0] = u'(';
        t = u"(\r\n" + exact + u"\r\n))";
        b = pairsColors(t, "cpp");
        CHECK(b.size() == 4 && b[1].depth == 1 && b[2].depth == 1 && b[3].depth == 0);
        // Depths agree.
        IncrementalHighlighter<char16_t> h("cpp");
        std::vector<Token> toks;
        StringSource<char16_t> src(t);
        h.reset(src, &toks);
        Depths d;
        d.assign(summarizeLines(src, h, 0, h.lineCount(), toks));
        CHECK(d.depthAt(1) == 1 && d.depthAt(2) == 2 && d.depthAt(3) == 0);
    }

    GROUP("bracketpairs:depth");
    {
        // Random edits, the highlighter's re-lexed lines summed again as the
        // editor does, against a full recount after every edit.
        const char *frags[] = {"(", ")", "[", "]", "{", "}", "\"", "'", "#", "//", "/*",
                               "*/", "\n", "\r\n", "\r", "x", "  ", "f(", "\"\"\"", "\\",
                               "`", "${", "'''"};
        std::mt19937 rng(17);
        int bad = 0, edits = 0, quiet = 0;
        for (const char *ext : {"py", "cpp", "js"}) {
            std::u16string text;
            for (int k = 0; k < 400; ++k) text += u16(frags[rng() % (sizeof frags / sizeof *frags)]);
            IncrementalHighlighter<char16_t> h(ext);
            std::vector<Token> toks;
            h.reset(StringSource<char16_t>(text), &toks);
            Depths d;
            d.assign(summarizeLines(StringSource<char16_t>(text), h, 0, h.lineCount(), toks));
            Recount before = recount(text, ext);
            for (int e = 0; e < 300; ++e, ++edits) {
                const size_t pos = text.empty() ? 0 : rng() % (text.size() + 1);
                const size_t oldLen = std::min<size_t>(rng() % 6, text.size() - pos);
                std::u16string ins;
                for (int m = (int)(rng() % 4); m-- > 0;)
                    ins += u16(frags[rng() % (sizeof frags / sizeof *frags)]);
                text.replace(pos, oldLen, ins);
                StringSource<char16_t> src(text);
                const size_t linesBefore = h.lineCount();
                std::vector<Token> out;
                auto r = h.edit(src, pos, oldLen, ins.size(), out);
                const size_t replaced = (r.endLine - r.firstLine) + linesBefore - h.lineCount();
                const bool changed =
                    d.replace(r.firstLine, replaced, summarizeLines(src, h, r.firstLine,
                                                                    r.endLine, out));
                Recount now = recount(text, ext);
                bool ok = d.lineCount() == h.lineCount() &&
                          now.lineDepth.size() == h.lineCount() + 1;
                for (size_t l = 0; ok && l <= h.lineCount(); ++l) ok = d.depthAt(l) == now.lineDepth[l];
                // Unchanged, the lines after the edit start where they did.
                if (ok && !changed) {
                    quiet++;
                    const long shift = (long)h.lineCount() - (long)linesBefore;
                    for (size_t l = r.endLine; ok && l <= h.lineCount(); ++l)
                        ok = now.lineDepth[l] == before.lineDepth[(size_t)((long)l - shift)];
                }
                if (ok && e % 25 == 0) ok = sameBrackets(pairsColors(text, ext), now.brackets);
                if (!ok && bad++ < 3)
                    std::printf("    %s: depths differ after edit %d\n", ext, e);
                before = std::move(now);
            }
        }
        CHECK(bad == 0);
        CHECK(quiet > 0 && quiet < edits);   // both kinds of edit happened
    }

    GROUP("bracketpairs:blocks");
    {
        // Splices across many blocks, growing and shrinking, against a flat list.
        std::mt19937 rng(23);
        std::vector<LineSum> flat;
        auto randomSum = [&]() {
            LineSum s;
            s.net = (int32_t)(rng() % 5) - 2;
            s.floor = std::max<int32_t>(0, s.net) + (int32_t)(rng() % 2);
            return s;
        };
        for (int k = 0; k < 3000; ++k) flat.push_back(randomSum());
        Depths d;
        d.assign(flat);
        bool ok = true;
        for (int e = 0; e < 400 && ok; ++e) {
            const size_t first = rng() % (flat.size() + 1);
            const size_t old = std::min<size_t>(rng() % (e % 50 == 0 ? 2000 : 4), flat.size() - first);
            std::vector<LineSum> now;
            for (int m = (int)(rng() % (e % 37 == 0 ? 1500 : 4)); m-- > 0;) now.push_back(randomSum());
            flat.erase(flat.begin() + first, flat.begin() + first + old);
            flat.insert(flat.begin() + first, now.begin(), now.end());
            d.replace(first, old, now);
            ok = d.lineCount() == flat.size();
            int32_t depth = 0;
            for (size_t l = 0; ok && l <= flat.size(); ++l) {
                ok = d.depthAt(l) == depth;
                if (l < flat.size()) {
                    ok = ok && d.line(l) == flat[l];
                    depth = flat[l].after(depth);
                }
            }
        }
        CHECK(ok);
        d.replace(0, d.lineCount(), {});
        CHECK(d.lineCount() == 0 && d.depthAt(0) == 0);
    }

    GROUP("bracketpairs:speed");
    {
        // 100,000 lines: summing them all, then a thousand one-line edits
        // and as many depth queries, as typing and painting ask.
        std::u16string text;
        for (int k = 0; k < 100000; ++k)
            text += u16(k % 7 == 0 ? "def f(a, b):  # (\n" : "    x = g(h[1], {'k': (2)})\n");
        IncrementalHighlighter<char16_t> h("py");
        std::vector<Token> toks;
        StringSource<char16_t> src(text);
        h.reset(src, &toks);
        double t0 = cpuMs();
        Depths d;
        d.assign(summarizeLines(src, h, 0, h.lineCount(), toks));
        const double sumMs = cpuMs() - t0;
        std::mt19937 rng(5);
        t0 = cpuMs();
        int32_t check = 0;
        for (int k = 0; k < 1000; ++k) {
            const size_t line = rng() % h.lineCount();
            std::vector<Token> lt;
            h.lineTokens(src, line, line + 1, lt);
            d.replace(line, 1, summarizeLines(src, h, line, line + 1, lt));
            check += d.depthAt(rng() % h.lineCount());
        }
        const double editMs = cpuMs() - t0;
        std::printf("  bracket pairs: 100,000 lines summed in %.1f ms, 1,000 edits and "
                    "queries in %.1f ms of CPU\n", sumMs, editMs);
        CHECK(check == 0);   // the text is balanced line by line
        CHECK_TIME(sumMs, 400);
        CHECK_TIME(editMs, 150);
    }
}
