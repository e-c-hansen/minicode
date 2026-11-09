// quick_diff_tests.inc — the `quickdiff:*` groups of run_tests.cpp: the
// editor's change bars (QuickDiff.h): added, changed and deleted lines at
// the start, middle and end of a file, CRLF against LF, a last line with no
// newline, moving with edits, Next and Previous Change, and the texts of
// Revert Change and Stage Change. Included there.
#include "QuickDiff.h"

static void testQuickDiff() {
    using namespace QuickDiff;
    auto bar = [](Kind k, size_t line, size_t lines, size_t oldLine, size_t oldLines) {
        Bar b;
        b.kind = k;
        b.line = line;
        b.lines = lines;
        b.oldLine = oldLine;
        b.oldLines = oldLines;
        return b;
    };
    using V = std::vector<Bar>;
    // Reverting every bar, last first, gives the original back; staging
    // every bar, one at a time against the new original, gives the buffer.
    auto revertsAll = [](const std::u16string& o, const std::u16string& n) {
        std::u16string t = n;
        std::vector<Bar> bs = bars(o, n);
        for (size_t i = bs.size(); i-- > 0;) {
            const Edit e = revertEdit(bs[i], o, t);
            t.replace(e.start, e.length, e.text);
        }
        return t == o;
    };
    auto stagesAll = [](const std::u16string& o, const std::u16string& n) {
        std::u16string idx = o;
        for (int guard = 0; guard < 50; guard++) {
            std::vector<Bar> bs = bars(idx, n);
            if (bs.empty()) return idx == n;
            idx = stagedText(bs[0], idx, n);
        }
        return false;
    };

    GROUP("quickdiff:bars");
    {
        const std::u16string o = u16("a\nb\nc\nd\ne\n");
        CHECK(bars(o, o).empty());
        // Changed in the middle.
        CHECK(bars(o, u16("a\nb\nC\nd\ne\n")) == V({bar(Kind::Modified, 2, 1, 2, 1)}));
        // Added at the start, in the middle, at the end.
        CHECK(bars(o, u16("z\na\nb\nc\nd\ne\n")) == V({bar(Kind::Added, 0, 1, 0, 0)}));
        CHECK(bars(o, u16("a\nb\nx\ny\nc\nd\ne\n")) == V({bar(Kind::Added, 2, 2, 2, 0)}));
        CHECK(bars(o, u16("a\nb\nc\nd\ne\nf\n")) == V({bar(Kind::Added, 5, 1, 5, 0)}));
        // Deleted at the start, in the middle, at the end: lines is 0, and
        // line is the line now below them.
        CHECK(bars(o, u16("b\nc\nd\ne\n")) == V({bar(Kind::Deleted, 0, 0, 0, 1)}));
        CHECK(bars(o, u16("a\nb\ne\n")) == V({bar(Kind::Deleted, 2, 0, 2, 2)}));
        CHECK(bars(o, u16("a\nb\nc\n")) == V({bar(Kind::Deleted, 3, 0, 3, 2)}));
        // Several, in order; a changed run of a different length.
        V m = bars(o, u16("A\nb\nc\nD1\nD2\ne\n"));
        CHECK(m == V({bar(Kind::Modified, 0, 1, 0, 1), bar(Kind::Modified, 3, 2, 3, 1)}));
        CHECK(describe(m[0]) == "1 line changed" && describe(m[1]) == "1 line changed to 2");
        CHECK(describe(bar(Kind::Added, 0, 3, 0, 0)) == "3 lines added" &&
              describe(bar(Kind::Deleted, 2, 0, 2, 2)) == "2 lines deleted");
        // Blank lines between two changes stay unmarked (the formatter's
        // joined hunks would have marked them).
        CHECK(bars(u16("a\n\nb\n"), u16("A\n\nB\n")) ==
              V({bar(Kind::Modified, 0, 1, 0, 1), bar(Kind::Modified, 2, 1, 2, 1)}));
        // An empty original: every line added; an emptied buffer.
        CHECK(bars(u16(""), u16("x\ny\n")) ==
              V({bar(Kind::Added, 0, 2, 0, 0)}));
        CHECK(bars(u16("x\n"), u16("")) == V({bar(Kind::Deleted, 0, 0, 0, 1)}));
    }

    GROUP("quickdiff:line-endings");
    {
        // CRLF against LF is no change; a real change still shows.
        const std::u16string lf = u16("a\nb\nc\n"), crlf = u16("a\r\nb\r\nc\r\n");
        CHECK(bars(lf, crlf).empty() && bars(crlf, lf).empty());
        CHECK(bars(lf, u16("a\r\nB\r\nc\r\n")) == V({bar(Kind::Modified, 1, 1, 1, 1)}));
        // A last line with no newline: adding the newline adds the empty
        // line after it, as the editor shows it; changing it is a change.
        CHECK(bars(u16("a\nb"), u16("a\nb\n")) == V({bar(Kind::Added, 2, 1, 2, 0)}));
        CHECK(bars(u16("a\nb\n"), u16("a\nb")) == V({bar(Kind::Deleted, 2, 0, 2, 1)}));
        CHECK(bars(u16("a\nb"), u16("a\nc")) == V({bar(Kind::Modified, 1, 1, 1, 1)}));
        CHECK(bars(u16("a\nb"), u16("a\nb\nc")) == V({bar(Kind::Added, 2, 1, 2, 0)}));
        // An old Mac file's lone "\r" ends a line, as the gutter counts it.
        const std::u16string cr = u16("a\rb\rc\r");
        CHECK(bars(cr, u16("a\rB\rc\r")) == V({bar(Kind::Modified, 1, 1, 1, 1)}));
        CHECK(bars(cr, lf).empty());
        CHECK(revertsAll(cr, u16("a\rB\rX\rc\r")) && stagesAll(cr, u16("a\rc\r")));
        CHECK(oldLines(bar(Kind::Modified, 0, 1, 0, 2), cr) ==
              std::vector<std::u16string>({u16("a"), u16("b")}));
    }

    GROUP("quickdiff:revert");
    {
        const std::u16string o = u16("a\nb\nc\nd\ne\n");
        const char* cases[] = {"a\nb\nC\nd\ne\n", "z\na\nb\nc\nd\ne\n", "a\nb\nx\ny\nc\nd\ne\n",
                               "a\nb\nc\nd\ne\nf\n", "b\nc\nd\ne\n", "a\nb\ne\n", "a\nb\nc\n",
                               "A\nb\nc\nD1\nD2\ne\n", "", "q\n", "a\nb\nc\nd\ne", "x\ny"};
        for (const char* n : cases) {
            CHECK(revertsAll(o, u16(n)));
            CHECK(stagesAll(o, u16(n)));
        }
        // No newline at the end, on either side.
        const char* pairs[][2] = {{"a\nb", "a\nb\n"}, {"a\nb\n", "a\nb"}, {"a\nb", "a\nc"},
                                  {"a\nb", "a\nb\nc"}, {"a", ""}, {"", "a"}, {"x\ny\nz", "x\nz"}};
        for (auto& p : pairs) {
            CHECK(revertsAll(u16(p[0]), u16(p[1])));
            CHECK(stagesAll(u16(p[0]), u16(p[1])));
        }
        // One bar reverted leaves the others.
        const std::u16string n = u16("A\nb\nc\nD1\nD2\ne\n");
        V bs = bars(o, n);
        Edit e = revertEdit(bs[1], o, n);
        std::u16string t = n;
        t.replace(e.start, e.length, e.text);
        CHECK(t == u16("A\nb\nc\nd\ne\n"));
        CHECK(e.start == 6 && e.length == 6 && e.text == u16("d\n"));
        // Stage one bar: the index gets that change only.
        CHECK(stagedText(bs[0], o, n) == u16("A\nb\nc\nd\ne\n"));
        CHECK(stagedText(bs[1], o, n) == u16("a\nb\nc\nD1\nD2\ne\n"));
        // A CRLF original keeps CRLF in what is staged; the buffer's LF
        // lines get it too.
        const std::u16string co = u16("a\r\nb\r\nc\r\n");
        V cb = bars(co, u16("a\nB\nX\nc\n"));
        CHECK(cb.size() == 1 && stagedText(cb[0], co, u16("a\nB\nX\nc\n")) == u16("a\r\nB\r\nX\r\nc\r\n"));
        // An LF original with CRLF typed in: LF staged.
        V lb = bars(u16("a\nb\n"), u16("a\r\nB\r\n"));
        CHECK(lb.size() == 1 && stagedText(lb[0], u16("a\nb\n"), u16("a\r\nB\r\n")) == u16("a\nB\n"));
        // The popover's old lines, without their breaks.
        CHECK(oldLines(bar(Kind::Modified, 1, 1, 1, 2), co) ==
              std::vector<std::u16string>({u16("b"), u16("c")}));
        CHECK(oldLines(bar(Kind::Added, 1, 1, 1, 0), co).empty());
    }

    GROUP("quickdiff:navigate");
    {
        const V bs = {bar(Kind::Modified, 2, 2, 2, 2), bar(Kind::Deleted, 7, 0, 7, 3),
                      bar(Kind::Added, 10, 1, 13, 0)};
        CHECK(anchorLine(bs[0]) == 2 && anchorLine(bs[1]) == 6 && anchorLine(bs[2]) == 10);
        CHECK(anchorLine(bar(Kind::Deleted, 0, 0, 0, 1)) == 0);
        CHECK(barAt(bs, 2) == 0 && barAt(bs, 3) == 0 && barAt(bs, 4) == -1);
        CHECK(barAt(bs, 6) == 1 && barAt(bs, 7) == -1 && barAt(bs, 10) == 2);
        // Next: below the caret, wrapping to the first; Previous the other way.
        CHECK(nextBar(bs, 0) == 0 && nextBar(bs, 2) == 1 && nextBar(bs, 6) == 2 &&
              nextBar(bs, 10) == 0 && nextBar(bs, 99) == 0);
        CHECK(previousBar(bs, 99) == 2 && previousBar(bs, 10) == 1 && previousBar(bs, 6) == 0 &&
              previousBar(bs, 2) == 2 && previousBar(bs, 0) == 2);
        CHECK(nextBar({}, 3) == -1 && previousBar({}, 3) == -1);
    }

    GROUP("quickdiff:shift");
    {
        const V bs = {bar(Kind::Modified, 2, 2, 2, 2), bar(Kind::Deleted, 7, 0, 7, 3),
                      bar(Kind::Added, 10, 1, 13, 0)};
        // Two lines typed above everything: all move down two.
        V t = bs;
        shift(t, 0, 0, 2);
        CHECK(t[0].line == 4 && t[1].line == 9 && t[2].line == 12 && t[0].lines == 2);
        // A Return inside the first bar: it grows, the rest move.
        t = bs;
        shift(t, 3, 0, 1);
        CHECK(t[0].line == 2 && t[0].lines == 3 && t[1].line == 8 && t[2].line == 11);
        // Typing within a line moves nothing.
        t = bs;
        shift(t, 3, 0, 0);
        CHECK(t == bs);
        // Lines 4 to 6 joined into line 4: what is below moves up two.
        t = bs;
        shift(t, 4, 2, 0);
        CHECK(t[0].line == 2 && t[0].lines == 2 && t[1].line == 5 && t[2].line == 8);
        // An edit below every bar moves none.
        t = bs;
        shift(t, 20, 3, 0);
        CHECK(t == bs);
        // Bars follow an edit, then the next diff agrees with where they went.
        const std::u16string o = u16("a\nb\nc\nd\n");
        std::u16string n = u16("a\nb\nC\nd\n");
        V moved = bars(o, n);
        n = u16("new\na\nb\nC\nd\n");   // a line put in above
        shift(moved, 0, 0, 1);
        CHECK(moved == V({bar(Kind::Modified, 3, 1, 2, 1)}));
        CHECK(bars(o, n) == V({bar(Kind::Added, 0, 1, 0, 0), moved[0]}));
    }

    GROUP("quickdiff:settings");
    {
        Settings d = parseSettings("");
        CHECK(d.gitGutter());
        CHECK(d.gitGutterColor(0).rgb() == 0x487E02 && d.gitGutterColor(1).rgb() == 0x1B81A8 &&
              d.gitGutterColor(2).rgb() == 0xF14C4C);
        Settings light = parseSettings("theme = light-modern\n");
        CHECK(light.gitGutterColor(0).rgb() == 0x48985D && light.gitGutterColor(2).rgb() == 0xE51400);
        std::vector<SettingsError> errs;
        Settings s = parseSettings("editor.git-gutter = off\neditor.git-modified = #112233\n"
                                   "editor.git-added = nope\n", &errs);
        CHECK(!s.gitGutter() && s.gitGutterColor(1).rgb() == 0x112233);
        CHECK(errs.size() == 1 && errs[0].line == 3);
        CHECK(s.gitGutterColor(0).rgb() == 0x487E02);
    }

    GROUP("quickdiff:utf8");
    {
        const std::string s = "h\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";   // hé € 😀
        const std::u16string w = fromUtf8(s);
        CHECK(w.size() == 7 && w[1] == 0xE9 && w[3] == 0x20AC && w[5] == 0xD83D && w[6] == 0xDE00);
        CHECK(toUtf8(w) == s);
        CHECK(fromUtf8("a\xFF" "b") == std::u16string({u'a', 0xFFFD, u'b'}));
        CHECK(fromUtf8("\xC0\x80") == std::u16string({0xFFFD, 0xFFFD}));   // overlong
        CHECK(toUtf8(std::u16string({0xD800, u'x'})) == "\xEF\xBF\xBDx");
    }

    GROUP("quickdiff:bom");
    {
        // A file with a UTF-8 BOM: the tab's text has none (kept apart and
        // written back), the index's starts with U+FEFF. Round 6 compared
        // them as they were: line 0 always changed, and Revert Change put
        // U+FEFF in the buffer, so the save wrote EF BB BF EF BB BF.
        const std::u16string index = fromUtf8("\xEF\xBB\xBF" "a\nb\nc\n");
        std::u16string o = index;
        CHECK(bars(o, u16("a\nb\nc\n")).size() == 1);   // what happened before
        CHECK(stripBom(o, true) && o == u16("a\nb\nc\n"));
        CHECK(bars(o, u16("a\nb\nc\n")).empty());
        // Revert gives the buffer's lines back without a U+FEFF.
        const std::u16string n = u16("A\nb\nc\n");
        V bs = bars(o, n);
        CHECK(bs.size() == 1 && bs[0] == bar(Kind::Modified, 0, 1, 0, 1));
        const Edit e = revertEdit(bs[0], o, n);
        CHECK(e.start == 0 && e.text == u16("a\n"));
        // An edit to line 1 is a bar of its own, not merged with line 0.
        CHECK(bars(o, u16("a\nB\nc\n")) == V({bar(Kind::Modified, 1, 1, 1, 1)}));
        // Stage Change keeps the index's BOM.
        std::u16string staged = stagedText(bs[0], o, n);
        restoreBom(staged, true);
        CHECK(toUtf8(staged) == "\xEF\xBB\xBF" "A\nb\nc\n");
        // Nothing taken when the buffer's file has no BOM (the work tree
        // lost it: a change), or the index has none.
        std::u16string kept = index;
        CHECK(!stripBom(kept, false) && kept == index && bars(kept, u16("a\nb\nc\n")).size() == 1);
        std::u16string plain = u16("a\n");
        CHECK(!stripBom(plain, true) && plain == u16("a\n"));
        std::u16string s2 = u16("x\n");
        restoreBom(s2, false);
        CHECK(s2 == u16("x\n"));
        std::u16string empty;
        CHECK(!stripBom(empty, true) && empty.empty());
    }

    GROUP("quickdiff:utf16");
    {
        // A UTF-16 file (round 6's D10): the index's blob is UTF-16 with
        // its mark too, read unit for unit and the mark kept apart as the
        // tab keeps it; Stage Change writes it back that way.
        using FolderSearch::TextEncoding;
        auto utf16 = [](const std::u16string& t, bool le) {
            std::string b = le ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
            for (char16_t u : t) {
                const char hi = (char)(u >> 8), lo = (char)(u & 0xFF);
                b += le ? lo : hi;
                b += le ? hi : lo;
            }
            return b;
        };
        for (bool le : {true, false}) {
            const TextEncoding enc = le ? TextEncoding::Utf16LE : TextEncoding::Utf16BE;
            const std::u16string text = u"a\r\nb \U0001F600\r\nc\r\n";
            const Original o = readOriginal(utf16(text, le), enc, true);
            CHECK(o.text == text && o.bom && o.encoding == enc);
            CHECK(bars(o.text, text).empty());
            const std::u16string n = u"a\r\nB \U0001F600\r\nc\r\n";
            const V bs = bars(o.text, n);
            CHECK(bs == V({bar(Kind::Modified, 1, 1, 1, 1)}));
            CHECK(revertEdit(bs[0], o.text, n).text == u"b \U0001F600\r\n");
            CHECK(stagedBlob(stagedText(bs[0], o.text, n), o) == utf16(n, le));
            // A lone surrogate in the index stays itself both ways.
            const std::u16string odd = std::u16string(u"x") + char16_t(0xD800) + u"\n";
            const Original lone = readOriginal(utf16(odd, le), enc, true);
            CHECK(lone.text == odd && stagedBlob(lone.text, lone) == utf16(odd, le));
        }
        // The index in the other order, or in UTF-8: read as UTF-8, every
        // line a change (the file's encoding changed), nothing stripped.
        const Original other = readOriginal(utf16(u"a\n", false), TextEncoding::Utf16LE, true);
        CHECK(other.encoding == TextEncoding::Utf8 && !other.bom);
        CHECK(bars(other.text, u16("a\n")).size() == 1);
        const Original eight = readOriginal("a\n", TextEncoding::Utf16LE, true);
        CHECK(eight.text == u16("a\n") && eight.encoding == TextEncoding::Utf8 && !eight.bom);
        CHECK(stagedBlob(u16("b\n"), eight) == "b\n");
        // A UTF-8 file goes the old way: its BOM stripped only when kept apart.
        const Original u8bom = readOriginal("\xEF\xBB\xBF" "a\n", TextEncoding::Utf8, true);
        CHECK(u8bom.text == u16("a\n") && u8bom.bom);
        CHECK(stagedBlob(u16("b\n"), u8bom) == "\xEF\xBB\xBF" "b\n");
        const Original kept = readOriginal("\xEF\xBB\xBF" "a\n", TextEncoding::Utf8, false);
        CHECK(kept.text.size() == 3 && !kept.bom);
        // An odd-length blob is not taken as UTF-16.
        CHECK(readOriginal(utf16(u"a", true) + "x", TextEncoding::Utf16LE, true).encoding ==
              TextEncoding::Utf8);
    }

    GROUP("quickdiff:speed");
    {
        // A 100,000-line file with three edits, as off the main thread after
        // each pause in typing.
        std::string big;
        for (int i = 0; i < 100000; i++) big += "line " + std::to_string(i) + "\n";
        std::u16string o = u16(big), n = o;
        n.replace(10, 1, u"X");
        n.insert(n.size() / 2, u"new line\n");
        n.erase(n.size() - 30, 12);
        const double t0 = cpuMs();
        const V bs = bars(o, n);
        const double ms = cpuMs() - t0;
        std::printf("  quick diff: %.1f ms for 100,000 lines with three edits\n", ms);
        CHECK_TIME(ms, 1500);   // unoptimized here, and run off the main thread
        CHECK(bs.size() == 3);
    }
}
