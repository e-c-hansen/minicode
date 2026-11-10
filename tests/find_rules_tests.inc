// find_rules_tests.inc — the `editspans:*` and `findrules:*` groups of
// run_tests.cpp: several replacements over one text (EditSpans.h, the
// shape of every port's multi-range edit), and what the editor's find bar
// decides (FindRules.h): the seed, the count, the search with a scope in
// UTF-16 and code points, the changes Replace and Replace All make, and
// the theme colors, held to the Mac's table. Included there.

static void testEditSpansAndFindRules() {
    using EditSpans::Span;

    GROUP("editspans:valid");
    {
        CHECK(EditSpans::valid({}, 0));
        CHECK(EditSpans::valid({{0, 2, 5}, {2, 1, 0}, {5, 0, 3}}, 5));   // touching
        CHECK(EditSpans::valid({{3, 0, 1}, {3, 0, 2}, {3, 1, 1}}, 4));   // insertions at one place
        CHECK(!EditSpans::valid({{2, 2, 1}, {3, 1, 1}}, 9));             // overlapping
        CHECK(!EditSpans::valid({{4, 1, 1}, {1, 1, 1}}, 9));             // descending
        CHECK(!EditSpans::valid({{4, 2, 1}}, 5));                        // past the end
        CHECK(!EditSpans::valid({{6, 0, 1}}, 5));
        CHECK(EditSpans::valid({{5, 0, 1}}, 5));                         // at the end
    }

    GROUP("editspans:map");
    {
        // "aXbYc": X (1) becomes "xyz", Y (3) goes.
        const std::vector<Span> s = {{1, 1, 3}, {3, 1, 0}};
        CHECK(EditSpans::mapThrough(0, s) == 0);
        CHECK(EditSpans::mapThrough(1, s) == 1);   // at a span's start: stays
        CHECK(EditSpans::mapThrough(2, s) == 4);   // after X: moved by 2
        CHECK(EditSpans::mapThrough(3, s) == 5);
        CHECK(EditSpans::mapThrough(4, s) == 5);   // after Y: moved by 2 - 1
        CHECK(EditSpans::mapThrough(5, s) == 6);
        // Inside a span: as far in, but not past its new end.
        const std::vector<Span> shrink = {{2, 6, 2}};
        CHECK(EditSpans::mapThrough(3, shrink) == 3);
        CHECK(EditSpans::mapThrough(7, shrink) == 4);
        CHECK(EditSpans::mapThrough(8, shrink) == 4);
        CHECK(EditSpans::mapThrough(10, shrink) == 6);
        // An insertion at the place: after it, as typing leaves the caret.
        CHECK(EditSpans::mapThrough(4, {{4, 0, 3}}) == 7);
        CHECK(EditSpans::mapThrough(4, {{4, 0, 3}, {4, 0, 2}}) == 9);
        CHECK(EditSpans::mapThrough(3, {{4, 0, 3}}) == 3);
        // Against applying the spans to a string, at random.
        std::mt19937 rng(7);
        bool same = true;
        for (int round = 0; round < 500 && same; round++) {
            std::string text(1 + rng() % 30, 'a');
            for (char &c : text) c = (char)('a' + rng() % 26);
            std::vector<Span> spans;
            std::vector<std::string> with;
            size_t at = 0;
            while (at <= text.size() && spans.size() < 6) {
                const size_t start = at + rng() % 4;
                if (start > text.size()) break;
                const size_t len = std::min<size_t>(rng() % 4, text.size() - start);
                const size_t newLen = rng() % 4;
                spans.push_back({start, len, newLen});
                with.push_back(std::string(newLen, '#'));
                at = start + len;
            }
            CHECK(EditSpans::valid(spans, text.size()));
            // A marker character's place, carried through by hand.
            const size_t probe = rng() % (text.size() + 1);
            std::string after = text;
            size_t mapped = probe;
            for (size_t k = spans.size(); k-- > 0;) {
                const Span &sp = spans[k];
                after.replace(sp.start, sp.length, with[k]);
                if (mapped >= sp.start + sp.length) mapped = mapped - sp.length + sp.newLength;
                else if (mapped > sp.start) mapped = sp.start + std::min(mapped - sp.start, sp.newLength);
            }
            if (EditSpans::mapThrough(probe, spans) != mapped) same = false;
            if (mapped > after.size()) same = false;
        }
        CHECK(same);
    }

    GROUP("editspans:code-points");
    {
        // "a😀b😀c": the faces are two units each.
        const std::u16string t = u"a\U0001F600b\U0001F600c";
        CHECK(t.size() == 7);
        std::vector<size_t> cp = EditSpans::codePoints(t, {0, 1, 3, 4, 6, 7, 9});
        CHECK((cp == std::vector<size_t>{0, 1, 2, 3, 4, 5, 5}));
        // Inside a pair: its start.
        cp = EditSpans::codePoints(t, {2, 5});
        CHECK((cp == std::vector<size_t>{1, 3}));
        CHECK(EditSpans::unitsAt(t, 0) == 0);
        CHECK(EditSpans::unitsAt(t, 2) == 3);
        CHECK(EditSpans::unitsAt(t, 5) == 7);
        CHECK(EditSpans::unitsAt(t, 9) == 7);
        CHECK(EditSpans::codePoints(u"", {0, 3}) == (std::vector<size_t>{0, 0}));
    }

    GROUP("findrules:seed");
    {
        using FindRules::seed;
        const std::u16string line = u"7 alpha Alpha alphabet alpha.";
        // A selection within the line.
        FindRules::Seed s = seed(line, 8, 13, false);
        CHECK(s.found && s.query == u"Alpha" && !s.word && s.start == 8 && s.end == 13);
        CHECK(seed(line, 13, 8, false).query == u"Alpha");   // either way round
        // No selection: the word at the caret, inside it or touching it.
        s = seed(line, 17, 17, false);
        CHECK(s.found && s.word && s.query == u"alphabet" && s.start == 14 && s.end == 22);
        CHECK(seed(line, 22, 22, false).query == u"alphabet");
        // On punctuation and blanks: nothing.
        CHECK(!seed(u"a  .  b", 3, 3, false).found);
        // VS Code's separators: a dot ends a word.
        CHECK(seed(u"arr.size()", 6, 6, false).query == u"size");
        // Over a line break: nothing (the query stays as it was).
        CHECK(!seed(u"one\ntwo", 1, 6, false).found);
        CHECK(!seed(u"one\r\ntwo", 1, 4, false).found);
        CHECK(!seed(u"one two", 1, 6, false).found);
        // Too long.
        std::u16string big(FindRules::kMaxSeed + 2, u'x');
        CHECK(!seed(big, 0, FindRules::kMaxSeed, false).found);
        CHECK(seed(big, 0, FindRules::kMaxSeed - 1, false).found);
        // Escaped for a regular expression.
        s = seed(line, 23, 29, true);
        CHECK(s.query == u"alpha\\.");
        CHECK(FindRules::escapeRegex(u"a\\{b}*+?|^$.[c](d)-/") ==
              u"a\\\\\\{b\\}\\*\\+\\?\\|\\^\\$\\.\\[c\\]\\(d\\)-/");
        // An escaped seed finds itself as a regex.
        TextFind::Options o;
        o.regex = true;
        const std::u16string odd = u"x = (a+b)*[c]? {d} $e ^f |g. \\h";
        const TextFind::Query q = TextFind::Query::make(FindRules::escapeRegex(odd), o);
        CHECK(q.usable());
        const std::vector<TextFind::Match> m = TextFind::findAll(u"<<" + odd + u">>", q);
        CHECK(m.size() == 1 && m[0].start == 2 && m[0].length == odd.size());
        // Past the text: nothing.
        CHECK(!seed(u"abc", 2, 9, false).found);
    }

    GROUP("findrules:count");
    {
        using FindRules::CountState;
        CountState st;
        FindRules::Count c = FindRules::count(st);
        CHECK(c.text == "No results" && !c.error && !c.navigable && !c.keep);
        st.hasQuery = true;
        st.queryError = "Invalid regular expression: missing )";
        c = FindRules::count(st);
        CHECK(c.text == "No results" && c.error && c.tip == st.queryError && !c.navigable);
        st.queryError.clear();
        c = FindRules::count(st);   // asked, nothing back yet
        CHECK(c.text == "…" && !c.keep);
        st.searched = true;          // back, for an older text
        CHECK(FindRules::count(st).keep);
        st.current = true;
        c = FindRules::count(st);
        CHECK(c.text == "No results" && c.noResults && !c.error && !c.navigable);
        st.tooSlow = true;
        c = FindRules::count(st);
        CHECK(c.text == "Too slow" && c.error && c.tip.find("two seconds") != std::string::npos);
        st.tooSlow = false;
        st.matches = 17;
        st.currentIndex = 2;
        c = FindRules::count(st);
        CHECK(c.text == "3 of 17" && c.navigable && !c.noResults && c.tip.empty());
        st.currentIndex = -1;
        CHECK(FindRules::count(st).text == "? of 17");
        st.matches = FindRules::kMatchLimit;
        st.truncated = true;
        st.currentIndex = 0;
        c = FindRules::count(st);
        CHECK(c.text == "1 of 20000+" && c.tip.find("20,000") != std::string::npos);
    }

    GROUP("findrules:search");
    {
        TextFind::Options o;
        // A face before the matches: units and code points part.
        const std::u16string t = u"\U0001F600 alpha Alpha\nalphabet \U0001F600alpha";
        FindRules::Found f = FindRules::findAll(t, TextFind::Query::make(std::u16string(u"alpha"), o));
        CHECK(f.units.size() == 4 && f.points.size() == 4 && !f.truncated && !f.cancelled);
        CHECK(f.units[0].start == 3 && f.points[0].start == 2 && f.points[0].length == 5);
        CHECK(f.units[3].start == t.size() - 5 && f.points[3].start == t.size() - 7);
        CHECK(f.units[3].groups.size() == 1);   // the groups stay with the units
        // Find in Selection: only matches wholly inside, in code points.
        f = FindRules::findAll(t, TextFind::Query::make(std::u16string(u"alpha"), o), 2, 13);
        CHECK(f.points.size() == 2 && f.points[1].start == 8);
        f = FindRules::findAll(t, TextFind::Query::make(std::u16string(u"alpha"), o), 2, 12);
        CHECK(f.points.size() == 1);
        // The limit: 20000 and a "+".
        std::u16string many;
        for (int i = 0; i < 20010; i++) many += u"ab ";
        f = FindRules::findAll(many, TextFind::Query::make(std::u16string(u"ab"), o));
        CHECK(f.points.size() == FindRules::kMatchLimit && f.truncated);
        f = FindRules::findAll(many, TextFind::Query::make(std::u16string(u"ab"), o), 0, 30);
        CHECK(f.points.size() == 10 && !f.truncated);
        // A raised flag: nothing, and it says so (it is looked at between lines).
        std::u16string lines;
        for (int i = 0; i < 1000; i++) lines += u"ab\n";
        std::atomic<bool> stop(true);
        f = FindRules::findAll(lines, TextFind::Query::make(std::u16string(u"ab"), o), 0, 0, &stop);
        CHECK(f.cancelled && f.points.empty());
        // Nothing to look for.
        CHECK(FindRules::findAll(t, TextFind::Query()).points.empty());
        // Scope ends: a selection ending after a line break ends before it.
        CHECK(FindRules::scopeEnd(u"ab\ncd\n", 0, 6) == 5);
        CHECK(FindRules::scopeEnd(u"ab\ncd\n", 2, 3) == 3);    // the break alone stays
        CHECK(FindRules::scopeEnd(u"ab\ncd\n", 1, 2) == 2);
        CHECK(FindRules::scopeEnd(u"ab\ncd\n", 4, 4) == 4);    // empty: none
        CHECK(FindRules::scopeEnd(3, 9, U'\n') == 8);
    }

    GROUP("findrules:replace");
    {
        TextFind::Options o;
        o.regex = true;
        // Groups, around a face: code points and UTF-8.
        const std::u16string t = u"ann@x \U0001F600 bob@y";
        const TextFind::Query q = TextFind::Query::make(std::u16string(u"(\\w+)@(\\w)"), o);
        std::vector<FindRules::Change> ch = FindRules::replaceAll(t, q, u"$2:$1 é");
        CHECK(ch.size() == 2);
        CHECK(ch[0].start == 0 && ch[0].length == 5 && ch[0].text == "x:ann \xC3\xA9" &&
              ch[0].newLength == 7);
        CHECK(ch[1].start == 8 && ch[1].length == 5 && ch[1].text == "y:bob \xC3\xA9");
        // Applied last first in code points, the text the core makes.
        std::u32string pts;
        for (size_t i = 0; i < t.size(); i++) {
            char32_t c = t[i];
            if (c >= 0xD800 && c <= 0xDBFF) c = 0x10000 + ((c - 0xD800) << 10) + (t[++i] - 0xDC00);
            pts += c;
        }
        // UTF-8 to code points (the changes' texts are BMP and two-byte).
        auto points = [](const std::string &s) {
            std::u32string out;
            for (size_t i = 0; i < s.size(); i++) {
                const unsigned char b = (unsigned char)s[i];
                if (b < 0x80) out += (char32_t)b;
                else out += (char32_t)(((b & 0x1F) << 6) | ((unsigned char)s[++i] & 0x3F));
            }
            return out;
        };
        for (size_t k = ch.size(); k-- > 0;) {
            const std::u32string with = points(ch[k].text);
            CHECK(with.size() == ch[k].newLength);
            pts.replace(ch[k].start, ch[k].length, with);
        }
        CHECK(pts == U"x:ann é \U0001F600 y:bob é");
        // In a scope (code points).
        ch = FindRules::replaceAll(t, q, u"Z", 6, 20);
        CHECK(ch.size() == 1 && ch[0].start == 8);
        // Preserve Case, as VS Code's.
        TextFind::Options pc;
        pc.preserveCase = true;
        ch = FindRules::replaceAll(u"Cat CAT cat", TextFind::Query::make(std::u16string(u"cat"), pc),
                                   u"dog");
        CHECK(ch.size() == 3 && ch[0].text == "Dog" && ch[1].text == "DOG" && ch[2].text == "dog");
        // Replace: the one match, its groups expanded against the whole text.
        const FindRules::Found f = FindRules::findAll(t, q);
        FindRules::Change one;
        CHECK(FindRules::replaceOne(t, f, 1, q, u"<$1>", &one));
        CHECK(one.start == 8 && one.length == 5 && one.text == "<bob>" && one.newLength == 5);
        CHECK(!FindRules::replaceOne(t, f, 2, q, u"x", &one));
        // Nothing to look for: nothing to change.
        CHECK(FindRules::replaceAll(t, TextFind::Query(), u"x").empty());
    }

    GROUP("findrules:palette");
    {
        const char *themes[] = {"dark-modern", "dark-plus", "monokai", "solarized-dark",
                                "tomorrow-night-blue", "light-modern", "light-plus",
                                "quiet-light", "solarized-light"};
        for (const char *t : themes)
            CHECK(std::string(FindRules::palette(t, true).theme) == t);
        CHECK(std::string(FindRules::palette("nope", true).theme) == "dark-plus");
        CHECK(std::string(FindRules::palette("nope", false).theme) == "light-plus");
        CHECK(FindRules::currentMatch(true) != FindRules::currentMatch(false));
        // The Mac's table (FindWidget.mm's kPalettes) and this one agree,
        // number for number, so the two ports' bars look alike.
        std::ifstream mm("src/FindWidget.mm");
        if (!mm) {
            std::printf("  (src/FindWidget.mm not found here; palette mirror skipped)\n");
        } else {
            std::stringstream ss;
            ss << mm.rdbuf();
            const std::string text = ss.str();
            for (const FindRules::Palette &p : FindRules::palettes()) {
                const std::string key = std::string("{\"") + p.theme + "\"";
                const size_t at = text.find(key);
                CHECK(at != std::string::npos);
                if (at == std::string::npos) continue;
                const size_t end = text.find('}', at);
                std::vector<uint32_t> mac;
                for (size_t i = at + key.size(); i < end;) {
                    const size_t comma = text.find(',', i);
                    std::string item = text.substr(i, std::min(comma, end) - i);
                    i = std::min(comma, end) + 1;
                    item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
                    while (!item.empty() && (item.back() == 'U' || item.back() == 'u')) item.pop_back();
                    if (!item.empty()) mac.push_back((uint32_t)std::strtoul(item.c_str(), nullptr, 0));
                }
                const std::vector<uint32_t> ours = {
                    p.widgetBg, p.widgetBorder, p.widgetFg, p.shadow, p.inputBg, p.inputBorder,
                    p.inputFg, p.placeholder, p.focus, p.optBg, p.optBorder, p.optFg,
                    p.errorFg, p.badgeBg, p.badgeFg, p.sideBarBg, p.selection};
                if (mac != ours) std::printf("  FindRules' %s differs from FindWidget.mm's\n", p.theme);
                CHECK(mac == ours);
            }
        }
    }
}
