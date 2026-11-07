// multiedit_tests.inc — the `multicaret:text-reader`, `linediff:large-reformat`
// and `linediff:anchors` groups of run_tests.cpp: the line and caret
// commands reading the text in blocks near the carets (as the Mac's editor
// hands it to them) with the same results as over a whole string, and
// LineDiff keeping a large reformat in many hunks. Included there.

static void testMultiEdit() {
    using namespace MultiCaret;

    GROUP("multicaret:text-reader");
    {
        // A reader over a string, counting what it is asked for.
        auto over = [](const std::u16string &s) {
            return Text(s.size(), [&s](size_t start, size_t n, char16_t *out) {
                std::copy(s.begin() + (long)start, s.begin() + (long)(start + n), out);
            });
        };
        auto same = [](const Edit &x, const Edit &y) {
            if (x.carets != y.carets || x.changes.size() != y.changes.size()) return false;
            for (size_t i = 0; i < x.changes.size(); i++)
                if (x.changes[i].start != y.changes[i].start ||
                    x.changes[i].length != y.changes[i].length ||
                    x.changes[i].text != y.changes[i].text)
                    return false;
            return true;
        };
        // Random texts of lines (LF, CRLF and lone CR breaks, blanks, a
        // surrogate pair) past several blocks, random carets and selections:
        // every command answers the same over the reader as over the string.
        std::mt19937 rng(13);
        const char16_t *bits[] = {u"ab", u" ", u"\t", u"x_1", u"(", u"\n", u"\r\n", u"\r",
                                  u"\U0001F600", u"word", u"  "};
        bool allSame = true;
        for (int round = 0; round < 300 && allSame; round++) {
            std::u16string s;
            const size_t pieces = 1 + rng() % (round < 200 ? 60 : 1500);
            for (size_t k = 0; k < pieces; k++) s += bits[rng() % 11];
            std::vector<Caret> cs;
            const size_t n = 1 + rng() % 6;
            for (size_t k = 0; k < n; k++) {
                size_t p = rng() % (s.size() + 1), q = rng() % 3 ? p : rng() % (s.size() + 1);
                cs.push_back({p, q});
            }
            cs = normalize(cs, s.size());
            const Text lazy = over(s);
            for (int m = 0; m < 6; m++)
                for (bool ext : {false, true})
                    if (move(s, cs, (Move)m, ext) != move(lazy, cs, (Move)m, ext)) allSame = false;
            if (!same(backspace(s, cs), backspace(lazy, cs))) allSame = false;
            if (!same(deleteForward(s, cs), deleteForward(lazy, cs))) allSame = false;
            for (bool down : {false, true}) {
                if (!same(duplicateLines(s, cs, down), duplicateLines(lazy, cs, down))) allSame = false;
                if (!same(moveLines(s, cs, down), moveLines(lazy, cs, down))) allSame = false;
            }
            if (expandLineSelection(s, cs) != expandLineSelection(lazy, cs)) allSame = false;
            if (!same(deleteLines(s, cs), deleteLines(lazy, cs))) allSame = false;
            if (lineCopy(s, cs) != lineCopy(lazy, cs)) allSame = false;
            if (copy(s, cs) != copy(lazy, cs)) allSame = false;
            if (!same(pasteLinesAbove(s, cs, u"p\nq\n"), pasteLinesAbove(lazy, cs, u"p\nq\n")))
                allSame = false;
        }
        CHECK(allSame);

        // Two carets in a 4,000,000-character text: an arrow, Backspace and
        // Move Line read a few blocks around each, not the text.
        std::u16string big;
        for (int i = 0; i < 100000; i++) big += u"    value = compute(x, y)  # 40 chars\n";
        const std::vector<Caret> two = {{1000, 1000}, {3000000, 3000000}};
        {
            const Text t = over(big);
            (void)move(t, two, Move::Right, false);
            (void)move(t, two, Move::LineStart, false);
            (void)backspace(t, two);
            (void)moveLines(t, two, true);
            (void)duplicateLines(t, two, false);
            CHECK(t.charactersRead() > 0 && t.charactersRead() <= 4 * 1024 * two.size());
        }
        // A whole-text Text reads nothing through a reader.
        const Text whole(big);
        CHECK(whole.size() == big.size() && whole[5] == big[5] && whole.charactersRead() == 0);
        CHECK(whole.substr(big.size() - 3, 10) == big.substr(big.size() - 3));
        const Text lazy = over(big);
        CHECK(lazy.substr(1020, 10) == big.substr(1020, 10));            // across two blocks
        CHECK(lazy.substr(5000, 3000) == big.substr(5000, 3000));        // a long piece at once
        CHECK(lazy.substr(big.size(), 4).empty());
    }

    GROUP("linediff:large-reformat");
    {
        // 100,000 lines with repeats ("}", blank lines) between unique ones.
        std::vector<std::u16string> lines;
        for (int i = 0; i < 100000; i++) {
            switch (i % 5) {
            case 0: lines.push_back(u16("def f" + std::to_string(i) + "(x):\n")); break;
            case 1: lines.push_back(u"    if x:\n"); break;
            case 2: lines.push_back(u16("        return " + std::to_string(i) + "\n")); break;
            case 3: lines.push_back(u"    return x\n"); break;
            default: lines.push_back(u"\n"); break;
            }
        }
        std::u16string a;
        for (const auto &l : lines) a += l;
        // A reformat touching every 50th line (2,000 lines), then every 5th
        // (20,000): one hunk for each, where the old 1,000-edit cap made one
        // hunk of the whole middle.
        for (int every : {50, 5}) {
            std::u16string b;
            size_t changed = 0;
            for (size_t i = 0; i < lines.size(); i++) {
                if (i % (size_t)every == 1) {
                    b += u"    if  x :\n";
                    changed++;
                } else {
                    b += lines[i];
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            const std::vector<LineDiff::Hunk> h = LineDiff::diff(a, b, 40000, 10.0);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            std::printf("  linediff: 100,000 lines, %zu changed: %zu hunks in %.1f ms\n", changed,
                        h.size(), ms);
            CHECK(h.size() == changed);
            CHECK(LineDiff::apply(a, b, h) == b);
            bool ones = true;
            for (const LineDiff::Hunk &k : h) ones = ones && k.oldLines == 1 && k.newLines == 1;
            CHECK(ones);
        }
        // Past the budget the rest is coarser, never wrong.
        std::u16string b = a;
        for (size_t p = 0; (p = b.find(u"    if x:\n", p)) != std::u16string::npos; p += 12)
            b.replace(p, 10, u"    if  x :\n");
        const std::vector<LineDiff::Hunk> h0 = LineDiff::diff(a, b, 50000, 0.0);
        CHECK(!h0.empty() && LineDiff::apply(a, b, h0) == b);
        // More changed lines than maxEdits: the whole middle as one hunk.
        const std::vector<LineDiff::Hunk> h1 = LineDiff::diff(a, b, 1000);
        CHECK(h1.size() == 1 && LineDiff::apply(a, b, h1) == b);
    }

    GROUP("linediff:anchors");
    {
        // Large random edits over lines that repeat: hunks in order, apart,
        // and the result right, whichever way each piece was matched.
        std::mt19937 rng(21);
        bool ok = true;
        for (int round = 0; round < 40; round++) {
            std::vector<std::string> la;
            const int n = 2000 + (int)(rng() % 3000);
            for (int i = 0; i < n; i++)
                la.push_back(rng() % 4 ? "u" + std::to_string(i) : std::string(1, char('a' + rng() % 3)));
            std::vector<std::string> lb;
            for (const std::string &l : la) {
                const unsigned r = rng() % 10;
                if (r == 0) continue;                                  // deleted
                if (r == 1) lb.push_back("new" + std::to_string(rng() % 50));   // added
                lb.push_back(r == 2 ? l + "!" : l);                    // changed or kept
            }
            std::u16string x, y;
            for (const auto &l : la) x += u16(l + "\n");
            for (const auto &l : lb) y += u16(l + "\n");
            const std::vector<LineDiff::Hunk> h = LineDiff::diff(x, y, 40000, 10.0);
            if (LineDiff::apply(x, y, h) != y) ok = false;
            for (size_t i = 1; i < h.size(); i++)
                if (h[i].oldStart <= h[i - 1].oldStart + h[i - 1].oldLength) ok = false;
            if (h.size() < 100) ok = false;   // split up, not one hunk
        }
        CHECK(ok);
    }
}
