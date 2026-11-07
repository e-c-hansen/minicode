// side_by_side_tests.inc — the `sidebyside:words`, `sidebyside:panes`,
// `sidebyside:step` and `sidebyside:accept-hunks` groups of run_tests.cpp:
// what makes the compare view a review tool (SideBySide.h): the words that
// changed inside a changed pair of lines, each pane's text and spans placed
// on it, F7's stepping through the runs of changes, and the text made of
// the hunks a user accepted. (`sidebyside:align` is in control_tests.inc.)
// Included there.

static void testSideBySideReview() {
    using namespace SideBySide;
    auto pieces = [](const std::u16string &line, const std::vector<Span> &spans) {
        std::u16string out;
        for (const Span &s : spans) {
            if (!out.empty()) out += u'|';
            out += line.substr(s.start, s.length);
        }
        return out;
    };

    GROUP("sidebyside:words");
    {
        auto check = [&](const std::u16string &a, const std::u16string &b,
                         const std::u16string &wantLeft, const std::u16string &wantRight) {
            WordChanges w = wordChanges(a, b);
            const std::u16string l = pieces(a, w.left), r = pieces(b, w.right);
            CHECK(l == wantLeft);
            CHECK(r == wantRight);
            if (l != wantLeft || r != wantRight)
                fprintf(stderr, "    got left \"%s\" right \"%s\"\n", toU8(l, nullptr).c_str(),
                        toU8(r, nullptr).c_str());
        };
        // One word changed: only it, on both sides.
        check(u"    return 1", u"    return 42", u"1", u"42");
        check(u"def alpha(x):", u"def beta(x):", u"alpha", u"beta");
        // A word added, a word removed (the other side has nothing).
        check(u"x = f(a)", u"x = f(a, b)", u"", u", b");
        check(u"x = f(a, b)", u"x = f(a)", u", b", u"");
        // Two changes with only a blank between are one.
        check(u"if old thing:", u"if new stuff:", u"old thing", u"new stuff");
        // Two changes with a word between stay two.
        check(u"a = b + c", u"x = b + y", u"a|c", u"x|y");
        // Nothing shared but blanks: no word spans (the whole line is tinted).
        check(u"foo bar", u"baz qux", u"", u"");
        check(u"same", u"same", u"", u"");
        // Indentation alone changed: the blanks are the change.
        check(u"  x = 1", u"    x = 1", u"  ", u"    ");
        // Words are letters, digits and underscores, past ASCII included;
        // punctuation is a word of its own.
        check(u"naïve_name = 1", u"naïve_nom = 1", u"naïve_name", u"naïve_nom");
        check(u"a.b", u"a->b", u".", u"->");
        // UTF-16: an emoji (a surrogate pair) moves whole.
        check(u"say \U0001F600 now", u"say \U0001F601 now", u"\U0001F600", u"\U0001F601");
        // An empty side.
        check(u"", u"x", u"", u"");
        // A very long line still answers (the middle as one change).
        std::u16string longA, longB;
        for (int i = 0; i < 3000; i++) {
            longA += u"w" + std::u16string(1, (char16_t)(u'a' + i % 26)) + u" ";
            longB += u"v" + std::u16string(1, (char16_t)(u'a' + i % 26)) + u" ";
        }
        longA = u"keep " + longA + u"end";
        longB = u"keep " + longB + u"end";
        WordChanges w = wordChanges(longA, longB);
        CHECK(w.left.size() == 1 && w.right.size() == 1);
        CHECK(!w.left.empty() && w.left[0].start == 5);
    }

    GROUP("sidebyside:panes");
    {
        const std::u16string a = u"one\r\ntwo\r\nthree\r\n", b = u"one\r\n2\r\nthree\r\nfour";
        std::vector<Row> rows = align(a, b);
        std::vector<size_t> ls, rs;
        const std::u16string left = paneText(a, rows, true, &ls);
        const std::u16string right = paneText(b, rows, false, &rs);
        CHECK(left == u"one\ntwo\nthree\n");
        CHECK(right == u"one\n2\nthree\nfour");
        CHECK(ls.size() == rows.size() && rs.size() == rows.size());
        CHECK(rs[3] == 12 && right.substr(rs[3]) == u"four");
        // A lone CR or U+2028 is inside a line: the panes keep in step.
        rows = align(u"a\rb\nc\n", u"a\rb\nC\n");
        CHECK(rows.size() == 2);
        CHECK(paneText(u"a\rb\nc\n", rows, true, &ls) == u"a\rb\nc");
        CHECK(paneText(u"x y\n", align(u"x y\n", u"z\n"), true, nullptr) == u"x y");
        // Spans over the text (a comment across two lines, a word on the
        // third) land on the rows that show those lines, cut at the breaks.
        const std::u16string t = u"/* a\nb */\nint x;\n";
        rows = align(u"zero\n" + t, t);   // left has an extra first line
        const std::u16string pane = paneText(t, rows, false, &rs);
        CHECK(pane == u"\n/* a\nb */\nint x;");
        std::vector<Span> spans = spansOnPane(t, rows, false, rs, {{0, 9, 7}, {10, 3, 1}});
        CHECK(spans.size() == 3);
        if (spans.size() == 3) {
            CHECK(pane.substr(spans[0].start, spans[0].length) == u"/* a" && spans[0].style == 7);
            CHECK(pane.substr(spans[1].start, spans[1].length) == u"b */");
            CHECK(pane.substr(spans[2].start, spans[2].length) == u"int" && spans[2].style == 1);
        }
        // A span reaching past the text, and one on a CRLF break, are cut.
        spans = spansOnPane(u"ab\r\n", align(u"ab\r\n", u"ab\r\n"), true, {0}, {{1, 50, 0}});
        CHECK(spans.size() == 1 && spans[0].start == 1 && spans[0].length == 1);
    }

    GROUP("sidebyside:step");
    {
        // Rows: = ~ = = - + = ~  -> hunks at 1, 4..5, 7.
        const std::u16string a = u"a\nb\nc\nd\ne\nf\ng\n", b = u"a\nB\nc\nd\nE\nE2\nf\nG\n";
        std::vector<Row> rows = align(a, b);
        std::vector<Hunk> hs = hunks(rows);
        CHECK(hs.size() == 3);
        if (hs.size() == 3) {
            CHECK(hs[0].first == 1 && hs[0].end == 2);
            CHECK(hs[1].first == 4 && hs[1].end == 6);
            CHECK(hs[2].first == 7);
        }
        CHECK(changeStarts(rows) == std::vector<size_t>({1, 4, 7}));
        // From the top, forward: the first, then each next, wrapping.
        CHECK(stepHunk(hs, -1, true) == 0);
        CHECK(stepHunk(hs, 1, true) == 1);
        CHECK(stepHunk(hs, 4, true) == 2);
        CHECK(stepHunk(hs, 7, true) == 0);
        // Backward, wrapping from the first to the last.
        CHECK(stepHunk(hs, 7, false) == 1);
        CHECK(stepHunk(hs, 1, false) == 2);
        CHECK(stepHunk(hs, 0, false) == 2);
        // From a row between runs.
        CHECK(stepHunk(hs, 2, true) == 1);
        CHECK(stepHunk(hs, 3, false) == 0);
        CHECK(stepHunk({}, 0, true) == -1);
        CHECK(hunks(align(u"x\n", u"x\n")).empty());
        // A large rewrite (every tenth of 20,000 lines) keeps a run per
        // change, past the 2,000 edits that once made it one run of
        // everything (LineDiff splits it now).
        std::u16string big, bigNew;
        for (int i = 0; i < 20000; i++) {
            const std::string n = std::to_string(i);
            const std::u16string line = u"line " + std::u16string(n.begin(), n.end());
            big += line + u"\n";
            bigNew += (i % 10 == 5 ? line + u" changed" : line) + u"\n";
        }
        rows = align(big, bigNew);
        CHECK(hunks(rows).size() == 2000);
        CHECK(rows.size() == 20000);
    }

    GROUP("sidebyside:accept-hunks");
    {
        const std::u16string a = u"def a():\n    return 1\n\nkeep\n\ndef b():\n    return 2\n";
        const std::u16string b = u"def a():\n    return 10\n\nkeep\n\ndef b():\n    return 20\n    # more\n";
        std::vector<Row> rows = align(a, b);
        CHECK(hunks(rows).size() == 2);
        CHECK(assemble(a, b, rows, {true, true}) == b);
        CHECK(assemble(a, b, rows, {false, false}) == a);
        CHECK(assemble(a, b, rows, {true, false}) ==
              u"def a():\n    return 10\n\nkeep\n\ndef b():\n    return 2\n");
        CHECK(assemble(a, b, rows, {false, true}) ==
              u"def a():\n    return 1\n\nkeep\n\ndef b():\n    return 20\n    # more\n");
        // Missing choices count as rejected.
        CHECK(assemble(a, b, rows, {}) == a);
        // A new file: one hunk of added lines.
        rows = align(u"", u"x\ny\n");
        CHECK(assemble(u"", u"x\ny\n", rows, {true}) == u"x\ny\n");
        CHECK(assemble(u"", u"x\ny\n", rows, {false}).empty());
        // CRLF and a last line without "\n" come through as they are.
        const std::u16string c = u"1\r\n2\r\n3", d = u"1\r\nTWO\r\n3\r\n4";
        rows = align(c, d);
        CHECK(assemble(c, d, rows, std::vector<bool>(hunks(rows).size(), true)) == d);
        CHECK(assemble(c, d, rows, std::vector<bool>(hunks(rows).size(), false)) == c);
        // Random texts: all accepted gives b, none gives a (up to the last
        // line's "\n"), and each choice changes only its own hunk's lines.
        std::mt19937 rng(7);
        for (int round = 0; round < 300; round++) {
            auto text = [&](int n) {
                std::u16string s;
                for (int i = 0; i < n; i++) s += std::u16string(1, (char16_t)(u'a' + rng() % 4)) + u"\n";
                return s;
            };
            const std::u16string x = text((int)(rng() % 12)), y = text((int)(rng() % 12));
            rows = align(x, y);
            const size_t n = hunks(rows).size();
            CHECK(assemble(x, y, rows, std::vector<bool>(n, true)) == y);
            CHECK(assemble(x, y, rows, std::vector<bool>(n, false)) == x);
            std::vector<bool> pick(n);
            for (size_t i = 0; i < n; i++) pick[i] = rng() % 2;
            const std::u16string mixed = assemble(x, y, rows, pick);
            // Row by row: a Same row's line, an accepted hunk's new lines,
            // a rejected hunk's old lines, in order.
            std::u16string want;
            const std::vector<Hunk> hs = hunks(rows);
            auto line = [](const std::u16string &t, int k) {
                size_t at = 0;
                for (int i = 0; i < k; i++) at = t.find(u'\n', at) + 1;
                return t.substr(at, t.find(u'\n', at) + 1 - at);
            };
            for (size_t r = 0, h = 0; r < rows.size(); r++) {
                while (h < hs.size() && hs[h].end <= r) h++;
                const bool inHunk = h < hs.size() && hs[h].first <= r;
                if (!inHunk) want += line(y, rows[r].right);
                else if (pick[h] && rows[r].right >= 0) want += line(y, rows[r].right);
                else if (!pick[h] && rows[r].left >= 0) want += line(x, rows[r].left);
            }
            CHECK(mixed == want);
        }
    }
}
