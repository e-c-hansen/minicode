// regex_tests.inc — the `regex:*` groups of run_tests.cpp: Regex.h's linear
// engine as TextFind runs it, compared with std::regex on random patterns,
// JavaScript's answers where std::regex gives others, the patterns left to
// std::regex, and the time limit on those. Included by run_tests.cpp.

#include "Regex.h"

namespace {

// Random patterns and texts over a small alphabet, so they meet often.
struct RegexGen {
    std::mt19937 g;
    explicit RegexGen(unsigned seed) : g(seed) {}
    size_t below(size_t n) { return n ? g() % n : 0; }

    // Set while building: a repetition past its minimum whose body can
    // match nothing (where libc++, libstdc++ and JavaScript disagree), and
    // a capturing group inside a repetition (libstdc++ does not unset it
    // at each repetition, as JavaScript and libc++ do).
    bool emptyLoop = false, groupInLoop = false;
    // A \b or \B, which libc++ never finds at the end of a line once a
    // search goes on past the line's start (match_prev_avail).
    bool wordBoundary = false;
    int loops = 0;

    std::u16string literal() {
        static const char16_t *const lits[] = {u"a", u"b", u"c", u"A", u"B", u"\u00e9",
                                              u"\u00c9", u"\u0436", u"\u0416", u"-",
                                              u" ", u"_", u"1", u"\\.", u"\\-", u"x"};
        return lits[below(sizeof lits / sizeof *lits)];
    }

    std::u16string charClass() {
        static const char16_t *const items[] = {u"a", u"b", u"c", u"A", u"\u00e9", u"\u0436",
                                               u"_", u"1", u"a-c", u"A-C", u"\u00e0-\u00e9",
                                               u"\u0430-\u044f", u"\\d", u"\\w", u"\\s",
                                               u"\\-", u"."};
        std::u16string s = u"[";
        if (below(3) == 0) s += u"^";
        const size_t n = 1 + below(3);
        for (size_t k = 0; k < n; ++k) s += items[below(sizeof items / sizeof *items)];
        return s + u"]";
    }

    std::u16string atom(int depth, bool &canEmpty) {
        canEmpty = false;
        switch (below(depth > 2 ? 7 : 10)) {
        case 0: case 1: case 2: case 3: return literal();
        case 4: return u".";
        case 5: return charClass();
        case 6: {
            static const char16_t *const sets[] = {u"\\d", u"\\w", u"\\s", u"\\D", u"\\W",
                                                  u"\\S"};
            return sets[below(6)];
        }
        case 7: {
            if (loops) groupInLoop = true;
            return u"(" + alternation(depth + 1, canEmpty) + u")";
        }
        case 8: return u"(?:" + alternation(depth + 1, canEmpty) + u")";
        default: return u"(?:" + alternation(depth + 1, canEmpty) + u")";
        }
    }

    std::u16string term(int depth, bool &canEmpty) {
        if (below(10) == 0) {
            static const char16_t *const as[] = {u"^", u"$", u"\\b", u"\\B"};
            canEmpty = true;
            const size_t k = below(4);
            if (k >= 2) wordBoundary = true;
            return as[k];
        }
        int min = 1, max = 1;
        std::u16string q;
        switch (below(12)) {
        case 0: q = u"*"; min = 0; max = -1; break;
        case 1: q = u"+"; min = 1; max = -1; break;
        case 2: q = u"?"; min = 0; max = 1; break;
        case 3: {
            min = (int)below(3);
            max = min + (int)below(3);
            q = u"{" + std::u16string(1, char16_t(u'0' + min));
            if (min != max) q += u"," + std::u16string(1, char16_t(u'0' + max));
            q += u"}";
            break;
        }
        case 4: min = (int)below(3); max = -1;
            q = u"{" + std::u16string(1, char16_t(u'0' + min)) + u",}";
            break;
        default: break;
        }
        if (!q.empty() && below(3) == 0) q += u"?";
        if (!q.empty()) ++loops;
        bool bodyEmpty = false;
        std::u16string a = atom(depth, bodyEmpty);
        if (!q.empty()) --loops;
        if (!q.empty() && bodyEmpty && (max < 0 || max > min)) emptyLoop = true;
        canEmpty = q.empty() ? bodyEmpty : (min == 0 || bodyEmpty);
        return a + q;
    }

    std::u16string sequence(int depth, bool &canEmpty) {
        canEmpty = true;
        std::u16string s;
        const size_t n = below(4) + (depth == 0 ? 1 : 0);
        for (size_t k = 0; k < n; ++k) {
            bool e = false;
            s += term(depth, e);
            canEmpty = canEmpty && e;
        }
        return s;
    }

    std::u16string alternation(int depth, bool &canEmpty) {
        std::u16string s = sequence(depth, canEmpty);
        while (below(depth ? 4 : 6) == 0) {
            bool e = false;
            s += u"|" + sequence(depth, e);
            canEmpty = canEmpty || e;
        }
        return s;
    }

    std::u16string pattern() {
        emptyLoop = groupInLoop = wordBoundary = false;
        loops = 0;
        bool e = false;
        return alternation(0, e);
    }

    std::u16string text() {
        static const char16_t chars[] = u"abcABxx\u00e9\u00c9\u0436\u0416- _1.";
        std::u16string t;
        const size_t lines = 1 + below(3);
        for (size_t l = 0; l < lines; ++l) {
            if (l) t += u"\n";
            const size_t n = below(11);
            for (size_t k = 0; k < n; ++k) t += chars[below(sizeof chars / 2 - 1)];
        }
        return t;
    }
};

std::string regexMatchesText(const std::vector<TextFind::Match> &ms, bool groups) {
    std::string s;
    for (const auto &m : ms) {
        s += std::to_string(m.start) + "+" + std::to_string(m.length);
        if (groups)
            for (size_t k = 1; k < m.groups.size(); ++k)
                s += m.groups[k].first == std::string::npos
                         ? std::string("(-)")
                         : "(" + std::to_string(m.groups[k].first) + "+" +
                               std::to_string(m.groups[k].second) + ")";
        s += " ";
    }
    return s;
}

std::string regexNarrow(const std::u16string &s) {
    std::string o;
    for (char16_t c : s) {
        if (c >= 0x20 && c < 0x7f) o.push_back((char)c);
        else {
            char b[12];
            std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
            o += b;
        }
    }
    return o;
}

}  // namespace

static void testRegex() {
    using namespace TextFind;
    using V = std::vector<std::string>;
    auto rx = [](const std::u16string &p, bool cs = false) {
        Options o;
        o.regex = true;
        o.caseSensitive = cs;
        return Query::make(p, o);
    };
    // Matches with their groups, "start+length(group)...".
    auto all = [](const std::u16string &t, const Query &q) {
        return regexMatchesText(findAll(t, q), true);
    };

    GROUP("regex:oracle");
    {
        // The linear engine against std::regex on random patterns: the same
        // matches, and the same groups where the two libraries agree with
        // JavaScript (see RegexGen).
        RegexGen gen(20261107);
        int compared = 0, spansOnly = 0, skipped = 0, refused = 0, failures = 0;
        const int rounds = 3000 * kFuzzScale;
        for (int i = 0; i < rounds; ++i) {
            const std::u16string p = gen.pattern();
            Options o;
            o.regex = true;
            o.caseSensitive = gen.below(2) == 0;
            o.wholeWord = gen.below(8) == 0;
            const Query lin = Query::make(p, o), back = Query::makeBacktracking(p, o);
            if (!back.usable()) continue;
            if (!lin.linear()) {
                ++refused;
                continue;
            }
            if (gen.emptyLoop) {
                ++skipped;
                continue;
            }
            const bool groups = !gen.groupInLoop;
            if (!groups) ++spansOnly;
            for (int t = 0; t < 4; ++t) {
                std::u16string text = gen.text();
#ifdef _LIBCPP_VERSION
                if (gen.wordBoundary) {   // see RegexGen::wordBoundary
                    for (size_t k = text.find(u'\n'); k != std::u16string::npos;
                         k = text.find(u'\n', k + 2))
                        text.insert(k, u".");
                    text += u".";
                }
#endif
                const std::string a = regexMatchesText(findAll(text, lin), groups);
                const std::string b = regexMatchesText(findAll(text, back), groups);
                ++compared;
                if (a != b && ++failures <= 5)
                    std::printf("  regex %s%s%s on \"%s\": linear %s, std::regex %s\n",
                                regexNarrow(p).c_str(), o.caseSensitive ? "" : " (any case)",
                                o.wholeWord ? " (whole word)" : "",
                                regexNarrow(text).c_str(), a.c_str(), b.c_str());
            }
        }
        CHECK(failures == 0);
        // Most patterns run on the linear engine.
        CHECK(refused * 20 < rounds);
        std::printf("  regex oracle: %d texts compared (%d patterns by spans only), %d "
                    "patterns with empty repetitions left to the JavaScript cases, %d refused\n",
                    compared, spansOnly, skipped, refused);
    }

    GROUP("regex:javascript");
    {
        // JavaScript's answers (node's), where libc++ or libstdc++ give
        // others: each repetition starts with its groups unset...
        CHECK(all(u"ab", rx(u"(?:(a)|b)+")) == "0+2(-) ");
        CHECK(all(u"ab", rx(u"(?:(a)|(b))+")) == "0+2(-)(1+1) ");
        CHECK(all(u"bab", rx(u"(?:a|(b))*")) == "0+3(2+1) 3+0(-) ");
        // ...and one past the minimum that matched nothing does not count.
        CHECK(all(u"b", rx(u"(a*)?")) == "0+0(-) 1+0(-) ");
        CHECK(all(u"b", rx(u"(a*)*")) == "0+0(-) 1+0(-) ");
        CHECK(all(u"b", rx(u"(a*){0,2}")) == "0+0(-) 1+0(-) ");
        CHECK(all(u"b", rx(u"(a*)+")) == "0+0(0+0) 1+0(1+0) ");
        CHECK(all(u"aab", rx(u"(a|)+b")) == "0+3(1+1) ");
        CHECK(all(u"aa", rx(u"(?:|a)*")) == "0+2 2+0 ");
        CHECK(all(u"aa", rx(u"(?:a*?)*")) == "0+2 2+0 ");
        CHECK(all(u"ab", rx(u"(a?)*?b")) == "0+2(0+1) ");
        // Leftmost first, not longest.
        CHECK(all(u"abcd", rx(u"(a|ab)(c|bcd)(d*)")) == "0+4(0+1)(1+3)(4+0) ");
        CHECK(all(u"aaa", rx(u"a+?")) == "0+1 1+1 2+1 ");
        // \b at a line's end, which libc++ misses after a first match.
        CHECK(all(u"a\nxy", rx(u"\\b")) == "0+0 1+0 2+0 4+0 ");
        CHECK(all(u"ab", rx(u"\\B")) == "1+0 ");
        CHECK(all(u"<a><b>", rx(u"<.*?>")) == "0+3 3+3 ");
        // Escapes and classes.
        CHECK(all(u"a.b", rx(u"\\.")) == "1+1 ");
        CHECK(all(u"A\u00a0\u2028x", rx(u"\\s")) == "1+1 2+1 ");
        CHECK(all(u"\u00e9\u0663a", rx(u"\\w")) == "2+1 ");   // ASCII only
        CHECK(all(u"\u00e9\u0663a", rx(u"\\d")) == "");
        CHECK(all(u"x\u2028y", rx(u".")) == "0+1 2+1 ");     // . leaves out U+2028
        CHECK(all(u"\x08", rx(u"[\\b]")) == "0+1 ");
        CHECK(all(u"Az", rx(u"\\u0041\\x7a", true)) == "0+2 ");
        CHECK(all(u"a-b", rx(u"[a-]")) == "0+1 1+1 ");
        CHECK(all(u"-", rx(u"[\\-]")) == "0+1 ");
        CHECK(all(u"aaaa", rx(u"a{2}")) == "0+2 2+2 ");
        CHECK(all(u"aaaaa", rx(u"a{2,3}")) == "0+3 3+2 ");
        CHECK(all(u"aaaaa", rx(u"a{2,}?")) == "0+2 2+2 ");
        // Folding: classes and ranges past ASCII, either side.
        CHECK(all(u"\u0416\u0436", rx(u"[\u0430-\u044f]")) == "0+1 1+1 ");
        CHECK(all(u"\u0416\u0436", rx(u"[\u0410-\u042f]")) == "0+1 1+1 ");
        CHECK(all(u"\u0416\u0436", rx(u"[\u0410-\u042f]", true)) == "0+1 ");
        CHECK(all(u"Ab", rx(u"[^a]")) == "1+1 ");
        CHECK(all(u"\u03a3\u03c3\u03c2", rx(u"\u03c3")) == "0+1 1+1 2+1 ");
        // A surrogate pair is one character.
        CHECK(all(u"\U0001F600x", rx(u"^.x$")) == "0+3 ");
        CHECK(all(u"a\U0001F600", rx(u"[\U0001F600]")) == "1+2 ");
    }

    GROUP("regex:linear");
    {
        // Patterns that make a backtracking engine try every way of
        // splitting the line cost the linear engine one pass.
        auto ms = [](const char16_t *p, const std::u16string &t) {
            Options o;
            o.regex = true;
            const Query q = Query::make(p, o);
            const auto t0 = std::chrono::steady_clock::now();
            const size_t n = findAll(t, q).size();
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            return std::make_pair(n, ms);
        };
        const std::u16string as(30, u'a');
        auto r1 = ms(u"(a+)+b", as);
        CHECK(r1.first == 0 && r1.second < 50);
        auto r2 = ms(u"(a|aa)*c", as + as + as);
        CHECK(r2.first == 0 && r2.second < 50);
        auto r3 = ms(u"(x+x+)+y", std::u16string(5000, u'x'));
        CHECK(r3.first == 0 && r3.second < 500);
        // Many lines, with and without matches, and a long line with
        // none: about a pass over the text each (std::regex took 311 ms to
        // scan 2.5 MB without a match).
        std::u16string big;
        for (int i = 0; i < 20000; ++i) big += u"return value + other_name(42);\n";
        auto r4 = ms(u"ret[a-z]+n\\b", big);
        auto r5 = ms(u"[a-z]+_nothere\\(", big);
        auto r6 = ms(u"a.*z", std::u16string(100000, u'a'));
        CHECK(r4.first == 20000 && r5.first == 0 && r6.first == 0);
        CHECK(r4.second < 3000 && r5.second < 3000 && r6.second < 3000);
        // A cancel stops a long line within it.
        std::atomic<bool> stop(true);
        bool cancelled = false;
        Options o;
        o.regex = true;
        CHECK(findAll(std::u16string(200000, u'a'), Query::make(u"a.*z", o), 0, nullptr, &stop,
                      &cancelled)
                  .empty() &&
              cancelled);
        std::printf("  regex linear: (a+)+b on 30 a's %.2f ms, (x+x+)+y on 5,000 x's %.1f ms, "
                    "ret[a-z]+n\\b over 600 KB %.0f ms, [a-z]+_nothere\\( %.0f ms, a.*z on "
                    "100,000 a's %.0f ms\n",
                    r1.second, r3.second, r4.second, r5.second, r6.second);
        // The fold table stops where Regex.cpp's class folding stops.
        bool unchanged = true;
        for (char32_t c = 0x500; c < 0x110000; ++c)
            if (foldCase(c) != c) unchanged = false;
        CHECK(unchanged);
    }

    GROUP("regex:refused");
    {
        // What the linear engine leaves to std::regex still works there.
        std::string why;
        CHECK(Regex::compile(U"(a)\\1", false, &why) == nullptr && why == "a backreference");
        CHECK(Regex::compile(U"a(?=b)", false) == nullptr);
        CHECK(Regex::compile(U"[[:alpha:]]", false) == nullptr);
        CHECK(Regex::compile(U"a]", false) == nullptr);
        CHECK(Regex::compile(U"a{1001}", false) == nullptr);
        CHECK(Regex::compile(U"(?:a{1000}){1000}", false) == nullptr);   // too big
        CHECK(Regex::compile(U"a{1000}", false) != nullptr);
        const Query back = rx(u"(\\w)\\1");
        CHECK(back.usable() && !back.linear());
        CHECK(all(u"abba cc", back) == "1+2(1+1) 5+2(5+1) ");
        CHECK(all(u"ab ac", rx(u"a(?=c)")) == "3+1 ");
        CHECK(all(u"x1", rx(u"[[:digit:]]")) == "1+1 ");
        CHECK(rx(u"a\\d+").linear() && rx(u"(?:a|b)*?c").linear());
        // Invalid stays invalid, with std::regex's reason.
        CHECK(!rx(u"(a").usable() && rx(u"(a").error().find("Invalid regular expression") == 0);
        CHECK(!rx(u"a**").usable() || !rx(u"a**").linear());
        // The UTF-8 side (Find in Folder) goes through the same engines.
        size_t col = 0, len = 0;
        Options o;
        o.regex = true;
        CHECK(findInLine("xx abab", Query::make(std::string("(ab)\\1"), o), &col, &len) &&
              col == 3 && len == 4);
        CHECK(findInLine("x caf\xC3\xA9", Query::make(std::string("\\w+\xC3\xA9"), o), &col,
                         &len) &&
              col == 2 && len == 5);
    }

    GROUP("regex:too-slow");
    {
        // A pattern std::regex runs (a backreference) that takes long on a
        // line: the search is left behind at the limit, finds nothing, and
        // so does every later one with the query.
        setSlowPatternSeconds(0.05);
        Options o;
        o.regex = true;
        const std::u16string p = u"(a)(.*)\\1z";
        const Query q = Query::make(p, o);
        CHECK(q.usable() && !q.linear() && !q.tooSlow());
        CHECK(all(u"xaaz", q) == "1+3(1+1)(2+0) ");   // fine on a short line
        const std::u16string slowLine(20000, u'a');
        auto t0 = std::chrono::steady_clock::now();
        const auto found = findAll(slowLine + u"\naaz", q);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                             .count();
        CHECK(found.empty() && q.tooSlow() && s < 1);
        t0 = std::chrono::steady_clock::now();
        CHECK(findAll(u"aaz", q).empty());
        size_t col = 0, len = 0;
        CHECK(!findInLine("aaz", q, &col, &len));
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.05);
        // A new query starts afresh; per line (Find in Folder) the limit is
        // the same.
        const Query again = Query::make(std::string("(a)(.*)\\1z"), o);
        CHECK(findInLine("aaz", again, &col, &len) && col == 0 && len == 3);
        t0 = std::chrono::steady_clock::now();
        CHECK(findAllInLine(std::string(20000, 'a'), again).empty() && again.tooSlow());
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1);
        // A cancel while it waits is a cancel, not "too slow".
        const Query third = Query::make(p, o);
        std::atomic<bool> stop(true);
        bool cancelled = false;
        findAll(slowLine, third, 0, nullptr, &stop, &cancelled);
        CHECK(cancelled && !third.tooSlow());
        // Find in Folder stops at the first such line and says why.
        const fsys::path root = fsys::temp_directory_path() /
                                ("minicode-regex-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fsys::remove_all(root, ec);
        writeFile(root / "a.txt", "aaz\n" + std::string(20000, 'a') + "\n");
        writeFile(root / "b.txt", "aaz\n");
        FolderSearchOptions fo;
        fo.regex = true;
        const FolderSearchResult fr = FolderSearch::search(root.string(), "(a)(.*)\\1z", nullptr, fo);
        CHECK(fr.tooSlow && !fr.cancelled && fr.matches.size() <= 2);
        fo.regex = false;
        CHECK(!FolderSearch::search(root.string(), "aaz", nullptr, fo).tooSlow);
        fsys::remove_all(root, ec);
        setSlowPatternSeconds(2.0);
        std::printf("  regex too slow: given up after %.2f s (limit 0.05 s)\n", s);
    }
    (void)V{};
}
