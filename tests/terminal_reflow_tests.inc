// terminal_reflow_tests.inc — the `screen:` groups of run_tests.cpp for soft
// wraps and reflow, mouse reporting and focus events, and the stream's
// program titles (TerminalScreen.h, TerminalStream.h), kept in their own
// file and included there.

namespace {

// Every logical line of the scrollback and screen, rows joined where they
// wrapped: each cell's text with a letter for its style, trailing default
// blanks dropped, and trailing empty lines dropped. What a reflow must keep.
std::vector<std::string> logicalCells(const TerminalScreen &s) {
    std::vector<std::string> out;
    std::string line;
    for (int l = -s.historySize(); l < s.rows(); l++) {
        bool wraps = l < s.rows() - 1 && s.lineWraps(l);
        int width = wraps ? s.lineTextWidth(l) : s.lineWidth(l);
        for (int c = 0; c < width; c++) {
            TermCell cell = s.lineCell(l, c);
            if (cell.width == 0) continue;
            line += cell.ch;
            const TermStyle &st = cell.style;
            if (st != TermStyle())
                line += "{" + std::to_string(st.fg.kind * 1000 + st.fg.index) + "," +
                        std::to_string(st.bg.kind * 1000 + st.bg.index + st.bg.r) +
                        (st.bold ? "b" : "") + (st.underline ? "u" : "") + "}";
        }
        if (wraps) continue;
        while (line.size() >= 1 && line.back() == ' ') line.pop_back();
        out.push_back(line);
        line.clear();
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

void testTerminalReflow() {
    GROUP("screen:soft-wrap");
    {
        // A row the text ran off wraps; one ended by CR LF does not, even
        // when exactly full (xterm's pending wrap).
        TerminalScreen s(5, 5);
        s.feed("hello world\r\nfives\r\n");
        CHECK(s.rowText(0) == "hello" && s.lineWraps(0) && s.lineWraps(1));
        CHECK(!s.lineWraps(2) && s.rowText(2) == "d" && !s.lineWraps(3));
        CHECK(s.text() == "hello\n worl\nd\nfives");
        // Copying joins the rows, the blank at the wrap included.
        CHECK(s.textBetween(0, 0, 2, 4) == "hello world");
        CHECK(s.textBetween(0, 3, 1, 2) == "lo wo");
        CHECK(s.textBetween(0, 0, 3, 4) == "hello world\nfives");
        // The scrollback keeps the mark, and copying across into the screen
        // joins too.
        TerminalScreen h(5, 2);
        h.setScrollbackLimit(10);
        h.feed("abcdefg");
        h.feed("\r\n");
        CHECK(h.historySize() == 1 && h.lineWraps(-1) && h.lineText(-1) == "abcde");
        CHECK(h.textBetween(-1, 0, 0, 4) == "abcdefg");
        // A wide character that does not fit leaves a blank, which is not
        // part of the text.
        TerminalScreen w(3, 2);
        w.feed("ab\xE4\xB8\xAD");
        CHECK(w.lineWraps(0) && w.lineTextWidth(0) == 2 && w.rowText(1) == "\xE4\xB8\xAD");
        CHECK(w.textBetween(0, 0, 1, 2) == "ab\xE4\xB8\xAD");
        // Erasing ends a wrap: a row erased whole, or from its first column,
        // no longer continues the row above.
        TerminalScreen e(4, 3);
        e.feed("abcdefgh");
        CHECK(e.lineWraps(0));
        e.feed("\033[2;1H\033[2K");
        CHECK(!e.lineWraps(0));
        TerminalScreen e2(4, 3);
        e2.feed("abcdef\r\033[K");
        CHECK(!e2.lineWraps(0));
        TerminalScreen e3(4, 3);
        e3.feed("abcdef\033[2J");
        CHECK(!e3.lineWraps(0) && !e3.lineWraps(1));
        TerminalScreen e4(4, 3);
        e4.feed("abcdef\033[1;2H\033[J");
        CHECK(!e4.lineWraps(0));
        // Scrolling a wrapped row away and filling rows: the new ones do not
        // wrap.
        TerminalScreen sc(4, 2);
        sc.feed("abcdefgh\r\n");
        CHECK(!sc.lineWraps(0) && !sc.lineWraps(1) && sc.rowText(0) == "efgh");
        // zsh's PROMPT_SP: after OSC 133;A, the auto-wrap that moves the
        // prompt below unfinished output ("foo" with no newline) is not a
        // soft wrap, so the prompt never joins that output.
        TerminalScreen sp(10, 3);
        sp.feed("foo\033]133;A\007%         \r \r$ ls");
        CHECK(sp.rowText(0) == "foo%" && !sp.lineWraps(0) && sp.rowText(1) == "$ ls");
        // After a newline the PROMPT_SP filler never wraps, and a long
        // prompt after it wraps as usual.
        TerminalScreen sp2(5, 3);
        sp2.feed("\033]133;A\007%    \r \r$ abcdef");
        CHECK(sp2.rowText(0) == "$ abc" && sp2.lineWraps(0));
    }

    GROUP("screen:reflow");
    {
        // Narrower, then back: the long line wraps and joins, the cursor
        // stays after its character, and the scrollback takes the overflow.
        TerminalScreen s(10, 3);
        s.setScrollbackLimit(100);
        s.feed("0123456789abcdef\r\n$ ");
        CHECK(s.rowText(0) == "0123456789" && s.lineWraps(0) && s.cursorRow() == 2);
        s.resize(4, 3);
        CHECK(s.historySize() == 2 && s.lineText(-2) == "0123" && s.lineText(-1) == "4567");
        CHECK(s.rowText(0) == "89ab" && s.rowText(1) == "cdef" && s.rowText(2) == "$");
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        s.resize(10, 3);
        CHECK(s.historySize() == 0 && s.text() == "0123456789\nabcdef\n$");
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        // Typing after a reflow continues the cursor's line.
        s.feed("ls");
        CHECK(s.rowText(2) == "$ ls");
        // A cursor waiting to wrap stays waiting at the same width, and at
        // the end of the row after a narrower one.
        TerminalScreen p(4, 2);
        p.feed("abcd");
        p.resize(4, 3);
        p.feed("e");
        CHECK(p.rowText(0) == "abcd" && p.rowText(1) == "e" && p.lineWraps(0));
        TerminalScreen p2(6, 2);
        p2.feed("abcd");
        p2.resize(4, 2);
        p2.feed("e");
        CHECK(p2.rowText(0) == "abcd" && p2.rowText(1) == "e");
        // Blank rows below the text are cut before anything goes up, and a
        // cursor out past a short line keeps its column.
        TerminalScreen b(10, 6);
        b.setScrollbackLimit(10);
        b.feed("one\r\ntwo\033[2;8H");
        b.resize(5, 3);
        CHECK(b.historySize() == 0 && b.text() == "one\ntwo");
        CHECK(b.cursorRow() == 2 && b.cursorCol() == 2);
        // The cursor stays on screen even when its line goes on below it
        // (a background filled to the edge) further than the screen holds.
        TerminalScreen bg(6, 2);
        bg.setScrollbackLimit(10);
        bg.feed("ab\033[44m\033[K\r\n\033[K\033[m\033[1;3H");
        bg.resize(2, 2);
        CHECK(bg.cursorRow() >= 0 && bg.rowText(bg.cursorRow()).empty());
        CHECK(bg.lineText(bg.cursorRow() - 1) == "ab");
        // Colors, wide characters and combining marks come through, with
        // the wide character moving whole to the next row.
        TerminalScreen c(8, 2);
        c.setScrollbackLimit(10);
        c.feed("\033[31mab\xE6\x97\xA5" "e\xCC\x81\033[42mxy\033[m");
        c.resize(3, 4);
        CHECK(c.rowText(0) == "ab" && c.lineWraps(0) && c.lineTextWidth(0) == 2);
        CHECK(c.rowText(1) == "\xE6\x97\xA5" "e\xCC\x81" && c.cell(1, 0).width == 2);
        CHECK(c.cell(1, 0).style.fg.index == 1 && c.cell(2, 0).style.bg.index == 2);
        c.resize(8, 2);
        CHECK(c.rowText(0) == "ab\xE6\x97\xA5" "e\xCC\x81xy" && !c.lineWraps(0));
        // The main screen behind the alternate one is reflowed, and its
        // saved cursor kept on its character; the alternate one is cut.
        TerminalScreen a(10, 3);
        a.setScrollbackLimit(10);
        a.feed("0123456789AB\r\n$ \033[?1049h\033[Hvim screen");
        a.resize(5, 3);
        CHECK(a.altScreen() && a.rowText(0) == "vim s");
        a.feed("\033[?1049l");
        CHECK(a.rowText(2) == "$" && a.cursorRow() == 2 && a.cursorCol() == 2);
        CHECK(a.textBetween(-a.historySize(), 0, 1, 4) == "0123456789AB");
        // Places the host keeps follow their text.
        TerminalScreen k(10, 3);
        k.setScrollbackLimit(100);
        for (int i = 0; i < 20; i++) k.feed("line " + std::to_string(i) + " abcdefghij\r\n");
        // "line 5 ..." is in the scrollback: its 'c' (col 9).
        int l5 = -1;
        for (int l = -k.historySize(); l < 0; l++)
            if (k.lineText(l).rfind("line 5 ", 0) == 0) l5 = l;
        std::vector<TermPos> keep = {{l5, 9}, {l5 + 2, 0}};
        CHECK(k.lineCell(l5, 9).ch == "c");
        k.resize(6, 3, &keep);
        CHECK(k.lineCell(keep[0].line, keep[0].col).ch == "c");
        CHECK(k.lineText(keep[1].line).rfind("line 6", 0) == 0 && keep[1].col == 0);
        k.resize(10, 3, &keep);
        CHECK(keep[0].line == l5 && keep[0].col == 9);
        // A place whose line leaves the scrollback goes above it.
        TerminalScreen d(10, 2);
        d.setScrollbackLimit(2);
        d.feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\nc\r\nd");
        std::vector<TermPos> gone = {{-2, 0}};
        d.resize(5, 2, &gone);
        CHECK(d.historySize() == 2 && gone[0].line < -d.historySize());
        // Lines keep their numbers' meaning: historyBase() counts what went
        // into the scrollback, so the first kept line keeps its number.
        CHECK(d.historyBase() - d.historySize() >= 0);
    }

    GROUP("screen:reflow-random");
    {
        // Random lines (ASCII, accents, combining marks, wide characters,
        // emoji, colors, tabs, trailing blanks) written at one width, then
        // reflowed to another and back: every logical line comes back the
        // same, cell for cell, and so does what the cursor follows.
        std::mt19937 rng(11);
        const char *pieces[] = {"a", "Z", "0", " ", " ", "é", "e\xCC\x81", "日", "本",
                                "\xF0\x9F\x98\x80", "─", "\t", "xy", "longerword"};
        const char *styles[] = {"\033[m", "\033[31m", "\033[1;44m", "\033[42m", "\033[4m"};
        int ok = 0, tried = 0;
        for (int round = 0; round < 400; round++) {
            int w1 = 2 + (int)(rng() % 30), w2 = 2 + (int)(rng() % 30);
            int rows = 2 + (int)(rng() % 6);
            TerminalScreen s(w1, rows);
            s.setScrollbackLimit(100000);
            int lines = (int)(rng() % 25);
            for (int l = 0; l < lines; l++) {
                int n = (int)(rng() % (3 * w1 + 2));
                for (int i = 0; i < n; i++) {
                    if (rng() % 5 == 0) s.feed(styles[rng() % 5]);
                    s.feed(pieces[rng() % 14]);
                }
                s.feed("\033[m");
                if (l + 1 < lines || rng() % 2) s.feed("\r\n");
            }
            // Rows scrolled in while a background was set are filled with
            // it; past the cursor they could be cut (screen:reflow).
            s.feed("\033[J");
            auto before = logicalCells(s);
            int beforeCol = s.cursorCol();
            // A few places on characters, which must stay on them.
            std::vector<TermPos> keep;
            std::vector<std::string> keptText;
            for (int i = 0; i < 6; i++) {
                int line = (int)(rng() % (s.historySize() + s.rows())) - s.historySize();
                int col = (int)(rng() % s.cols());
                TermCell cell = s.lineCell(line, col);
                if (cell.width == 0 || cell.ch == " ") continue;
                keep.push_back({line, col});
                keptText.push_back(cell.ch);
            }
            s.resize(w2, 2 + (int)(rng() % 6), &keep);   // rows too
            auto mid = logicalCells(s);
            bool placesKept = true;
            for (size_t i = 0; i < keep.size(); i++)
                if (s.lineCell(keep[i].line, keep[i].col).ch != keptText[i]) placesKept = false;
            s.resize(w1, rows, &keep);
            for (size_t i = 0; i < keep.size(); i++)
                if (s.lineCell(keep[i].line, keep[i].col).ch != keptText[i]) placesKept = false;
            auto after = logicalCells(s);
            tried++;
            if (!placesKept) std::printf("    round %d: a kept place moved off its text\n", round);
            if (before == mid && before == after && s.cursorCol() == beforeCol && placesKept) {
                ok++;
            } else if (ok + 3 > tried) {
                std::printf("    round %d (%d -> %d, %d rows): %zu/%zu/%zu lines, col %d/%d\n",
                            round, w1, w2, rows, before.size(), mid.size(), after.size(),
                            beforeCol, s.cursorCol());
                for (size_t i = 0; i < std::max(before.size(), after.size()); i++) {
                    std::string x = i < before.size() ? before[i] : "(none)";
                    std::string y = i < after.size() ? after[i] : "(none)";
                    std::string z = i < mid.size() ? mid[i] : "(none)";
                    if (x != y || x != z)
                        std::printf("      %zu: '%s'\n         '%s'\n         '%s'\n", i,
                                    x.c_str(), z.c_str(), y.c_str());
                }
            }
        }
        CHECK(ok == tried);
    }

    GROUP("screen:reflow-timing");
    {
        // 10,000 lines of scrollback, half of them wider than the narrower
        // width, reflowed narrower and back, and a change of rows alone.
        TerminalScreen s(120, 40);
        s.setScrollbackLimit(10000);
        std::string chunk;
        for (int i = 0; i < 10040; i++) {
            chunk += "\033[32m" + std::to_string(i) + "\033[m src/TerminalScreen.cpp:" +
                     std::to_string(i % 900) + ": warning: something about this line";
            if (i % 2) chunk += " that goes on past eighty columns, as compiler output does";
            chunk += "\r\n";
        }
        s.feed(chunk);
        auto before = logicalCells(s);
        auto time = [&](int cols, int rows) {
            const double t0 = cpuMs();
            s.resize(cols, rows);
            return cpuMs() - t0;
        };
        double narrower = time(80, 40), wider = time(120, 40), taller = time(120, 50);
        double shorter = time(120, 40);
        CHECK(logicalCells(s) == before);
        CHECK(s.historySize() == 10000 || s.historySize() == 10001);
        std::printf("  screen:reflow  10,000 lines: 120 to 80 columns %.1f ms, back %.1f ms, "
                    "rows only %.1f / %.1f ms\n", narrower, wider, taller, shorter);
        CHECK_TIME(narrower, 500);
        CHECK_TIME(wider, 500);
    }

    GROUP("screen:find-wrapped");
    {
        // A match runs over a soft wrap, never over a hard line end.
        TerminalScreen s(5, 4);
        s.setScrollbackLimit(10);
        s.feed("hello world\r\nworl\r\nd");
        auto m = s.find("world", false);
        CHECK(m.size() == 1 && m[0] == (TermMatch{0, 1, 5, 1, 1}));
        // Over the scrollback's edge into the screen.
        s.feed("\r\n\r\n\r\n");
        auto h = s.find("world", false);
        CHECK(h.size() == 1 && h[0].line < 0 && h[0].endLine == h[0].line + 1 &&
              h[0].width == 5);
        // Past a wide character's leftover blank: "ab中" at 3 columns.
        TerminalScreen w(3, 2);
        w.feed("ab\xE4\xB8\xAD");
        auto wm = w.find("b\xE4\xB8\xAD", false);
        CHECK(wm.size() == 1 && wm[0] == (TermMatch{0, 1, 3, 1, 2}));
    }
}

void testTerminalMouse() {
    GROUP("screen:mouse");
    {
        TerminalScreen s(300, 40);
        using M = TermMouse;
        CHECK(s.mouseTracking() == 0 && s.encodeMouse(M::Press, 0, 1, 1, 0).empty());
        // 1000: presses and releases in xterm's bytes; no motion.
        s.feed("\033[?1000h");
        CHECK(s.mouseTracking() == 1000);
        CHECK(s.encodeMouse(M::Press, 0, 0, 0, 0) == "\033[M !!");
        CHECK(s.encodeMouse(M::Press, 2, 9, 4, TermModCtrl) == "\033[M2*%");
        CHECK(s.encodeMouse(M::Release, 0, 9, 4, 0) == "\033[M#*%");
        CHECK(s.encodeMouse(M::Motion, 0, 9, 4, 0).empty());
        CHECK(s.encodeMouse(M::WheelUp, -1, 9, 4, 0) == "\033[M`*%");
        CHECK(s.encodeMouse(M::WheelDown, -1, 9, 4, TermModAlt) == "\033[Mi*%");
        // Past column 223 xterm's bytes cannot say where: nothing is sent.
        CHECK(s.encodeMouse(M::Press, 0, 223, 0, 0).empty());
        CHECK(s.encodeMouse(M::Press, 0, 222, 0, 0) == "\033[M \xFF!");
        // 1006, SGR: decimal, the button kept on release (lower-case m).
        s.feed("\033[?1006h");
        CHECK(s.mouseEncoding() == 1006);
        CHECK(s.encodeMouse(M::Press, 0, 249, 30, 0) == "\033[<0;250;31M");
        CHECK(s.encodeMouse(M::Release, 2, 249, 30, 0) == "\033[<2;250;31m");
        CHECK(s.encodeMouse(M::WheelDown, -1, 0, 0, TermModShift) == "\033[<69;1;1M");
        // 1002: motion while a button is down; 1003: all motion.
        s.feed("\033[?1002h");
        CHECK(s.encodeMouse(M::Motion, 0, 4, 5, 0) == "\033[<32;5;6M");
        CHECK(s.encodeMouse(M::Motion, -1, 4, 5, 0).empty());
        s.feed("\033[?1003h");
        CHECK(s.encodeMouse(M::Motion, -1, 4, 5, 0) == "\033[<35;5;6M");
        // 1015, urxvt's.
        s.feed("\033[?1006l\033[?1015h");
        CHECK(s.encodeMouse(M::Press, 1, 249, 30, 0) == "\033[33;250;31M");
        // 1005, UTF-8 coordinates.
        s.feed("\033[?1015l\033[?1005h");
        CHECK(s.encodeMouse(M::Press, 0, 249, 0, 0) == "\033[M \xC4\x9A!");
        // DECRQM knows them; any reset of tracking turns it off.
        s.feed("\033[?1003$p\033[?1005$p\033[?1006$p");
        CHECK(s.takeReplies() == "\033[?1003;1$y\033[?1005;1$y\033[?1006;2$y");
        s.feed("\033[?1000l");
        CHECK(s.mouseTracking() == 0 && s.encodeMouse(M::Press, 0, 0, 0, 0).empty());
        // X10 (9): presses only, without modifiers.
        s.feed("\033[?1005l\033[?9h");
        CHECK(s.encodeMouse(M::Press, 0, 0, 0, TermModCtrl) == "\033[M !!");
        CHECK(s.encodeMouse(M::Release, 0, 0, 0, 0).empty());
        // Focus events.
        CHECK(!s.focusEvents());
        s.feed("\033[?1004h");
        CHECK(s.focusEvents() && TerminalScreen::encodeFocus(true) == "\033[I" &&
              TerminalScreen::encodeFocus(false) == "\033[O");
        // A full reset turns everything off, and so does resetMouseModes.
        s.feed("\033c");
        CHECK(s.mouseTracking() == 0 && !s.focusEvents() && s.mouseEncoding() == 0);
        s.feed("\033[?1002h\033[?1006h\033[?1004h");
        s.resetMouseModes();
        CHECK(s.mouseTracking() == 0 && !s.focusEvents() && s.mouseEncoding() == 0);
    }

    GROUP("stream:titles");
    {
        // OSC 0 and 2 reach the host as Title events, in order with the
        // shell's marks; OSC 1 (the icon's name) does not.
        TerminalStream st;
        auto ev = st.feed("\033]133;C\007\033]0;vim notes.md\007x\033]1;icon\007"
                          "\033]2;\007\033]133;A\007");
        std::vector<std::string> titles;
        std::vector<int> kinds;
        for (const TermEvent &e : ev) {
            if (e.kind == TermEvent::Line) continue;
            kinds.push_back((int)e.kind);
            if (e.kind == TermEvent::Title) titles.push_back(e.text);
        }
        CHECK((titles == std::vector<std::string>{"vim notes.md", ""}));
        CHECK(kinds.size() == 4 && kinds[0] == TermEvent::CommandStart &&
              kinds[1] == TermEvent::Title && kinds[3] == TermEvent::PromptStart);
        // Control characters in a title are dropped; a long one is cut.
        auto ev2 = st.feed("\033]2;a\tb\033\\" "\033]2;" + std::string(600, 'x') + "\007");
        std::vector<std::string> t2;
        for (const TermEvent &e : ev2)
            if (e.kind == TermEvent::Title) t2.push_back(e.text);
        CHECK(t2.size() == 2 && t2[0] == "ab" && t2[1].size() == 256);
    }
}

} // namespace
