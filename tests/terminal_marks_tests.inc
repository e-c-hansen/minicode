// terminal_marks_tests.inc — the `term:width`, `term:clusters` and
// `screen:marks*` groups of run_tests.cpp: character widths from Unicode's
// data (termCharWidth, tables written by tools/gen-term-width.py), emoji
// sequences drawn as one cell, and the shell integration's marks kept on
// the screen for moving between commands (TerminalScreen::commands).

namespace {

// The marks the grid mode's zsh prints, with a nonce.
std::string mark(const std::string &body, const std::string &nonce = "n0") {
    return "\x1b]133;" + body + ";mc=" + nonce + "\x07";
}

// A prompt, a command typed at it and run, its output, and the end mark
// with a status, as zsh's precmd and preexec print them.
std::string session(const std::string &command, const std::string &output, int status) {
    return mark("A") + "% " + command + "\r\n" + mark("C") + output +
           mark("D;" + std::to_string(status));
}

}  // namespace

void testTerminalMarks() {
    GROUP("term:width");
    {
        struct { uint32_t cp; int width; const char *what; } known[] = {
            {'a', 1, "a letter"},
            {0x00AD, 1, "the soft hyphen, which shows"},
            {0x0301, 0, "a combining acute"},
            {0x05B4, 0, "Hebrew hiriq"},
            {0x064E, 0, "Arabic fatha"},
            {0x094D, 0, "Devanagari virama"},
            {0x093E, 1, "a Devanagari vowel sign that takes room (Mc)"},
            {0x0E31, 0, "Thai mai han-akat"},
            {0x1160, 0, "a Hangul medial vowel"},
            {0x11A8, 0, "a Hangul final consonant"},
            {0x1100, 2, "a Hangul initial consonant"},
            {0xAC00, 2, "a Hangul syllable"},
            {0x200B, 0, "zero width space"},
            {0x200D, 0, "zero width joiner"},
            {0x202E, 0, "a bidi override"},
            {0x2713, 1, "a check mark"},
            {0x2764, 1, "a heart, text by default"},
            {0x302A, 0, "an ideographic tone mark (Mn, though wide)"},
            {0x4E2D, 2, "a CJK ideograph"},
            {0xFE0F, 0, "VS16"},
            {0xFF21, 2, "a fullwidth letter"},
            {0x1F1E6, 1, "a regional indicator alone"},
            {0x1F3FB, 0, "a skin tone modifier"},
            {0x1F600, 2, "an emoji"},
            {0x1FAE8, 2, "an emoji new in Unicode 15"},
            {0x20000, 2, "a plane 2 ideograph"},
            {0x2FFFD, 2, "an unassigned plane 2 code point, wide by default"},
            {0xE0001, 0, "a tag character"},
            {0xE0100, 0, "a variation selector supplement"},
            {0xF0000, 1, "private use"},
        };
        int wrong = 0;
        for (const auto &k : known) {
            if (termCharWidth(k.cp) != k.width) {
                if (wrong++ < 5)
                    std::printf("  term:width: U+%04X (%s) is %d, want %d\n", k.cp, k.what,
                                termCharWidth(k.cp), k.width);
            }
        }
        CHECK(wrong == 0);
        // Every combining mark of the scripts the assessment named is zero,
        // where the old hand-written table gave 1,256 of them a column.
        int combining = 0;
        for (uint32_t cp = 0x0591; cp <= 0x05C7; cp++)   // Hebrew points
            if (cp != 0x05BE && cp != 0x05C0 && cp != 0x05C3 && cp != 0x05C6 && termCharWidth(cp))
                combining++;
        for (uint32_t cp = 0x064B; cp <= 0x065F; cp++)   // Arabic harakat
            if (termCharWidth(cp)) combining++;
        CHECK(combining == 0);
        CHECK(termIsPictographic(0x1F468) && termIsPictographic(0x2764) &&
              !termIsPictographic('a') && !termIsPictographic(0x1F1E6));
    }

    GROUP("term:clusters");
    {
        // A family: four people joined by zero width joiners take one wide
        // cell, as they draw as one picture.
        const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                                   "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
        TerminalScreen s(10, 3);
        s.feed(family + "x");
        CHECK(s.cell(0, 0).width == 2 && s.cell(0, 0).ch == family);
        CHECK(s.cell(0, 1).width == 0 && s.cell(0, 2).ch == "x" && s.cursorCol() == 3);
        // A flag: two regional indicators, one wide cell; a third starts
        // the next flag.
        TerminalScreen f(10, 3);
        f.feed("\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5" "x\xF0\x9F\x87\xAF");
        CHECK(f.cell(0, 0).width == 2 && f.cell(0, 0).ch == "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5");
        CHECK(f.cell(0, 2).ch == "x" && f.cell(0, 3).width == 1 && f.cursorCol() == 4);
        // VS16 makes a text symbol an emoji, two columns; without it, one.
        TerminalScreen h(10, 3);
        h.feed("\xE2\x9D\xA4\xEF\xB8\x8F" "a\xE2\x9D\xA4" "b");
        CHECK(h.cell(0, 0).width == 2 && h.cell(0, 2).ch == "a");
        CHECK(h.cell(0, 3).width == 1 && h.cell(0, 4).ch == "b");
        // VS16 after a letter changes nothing.
        TerminalScreen v(10, 3);
        v.feed("a\xEF\xB8\x8F" "b");
        CHECK(v.cell(0, 0).width == 1 && v.cell(0, 1).ch == "b");
        // A skin tone joins the emoji before it.
        TerminalScreen t(10, 3);
        t.feed("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD" "x");
        CHECK(t.cell(0, 0).width == 2 && t.cell(0, 0).ch.size() == 8 && t.cell(0, 2).ch == "x");
        // A joiner then a letter: the letter takes its own cell.
        TerminalScreen j(10, 3);
        j.feed("\xF0\x9F\x91\xA8\xE2\x80\x8D" "a");
        CHECK(j.cell(0, 2).ch == "a");
        // At the right edge there is no room to widen: the heart stays
        // narrow and the next character wraps as usual.
        TerminalScreen e(4, 3);
        e.feed("abc\xE2\x9D\xA4\xEF\xB8\x8F" "x");
        CHECK(e.cell(0, 3).width == 1 && e.cell(1, 0).ch == "x");
        // Widening over the left half of a wide character blanks its right half.
        TerminalScreen w(10, 3);
        w.feed("a\xE4\xB8\xAD" "b\x1b[1;1H\xE2\x9D\xA4\xEF\xB8\x8F");
        CHECK(w.cell(0, 0).width == 2 && w.cell(0, 1).width == 0 && w.cell(0, 2).ch == " " &&
              w.cell(0, 2).width == 1 && w.cell(0, 3).ch == "b");
        // The log joins the same sequences into one character.
        TerminalStream st;
        auto ev = st.feed(family + "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\r\n");
        CHECK(!ev.empty() && ev[0].runs.size() == 1 &&
              ev[0].runs[0].text == family + "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5");
    }

    GROUP("screen:marks");
    {
        TerminalScreen s(30, 6);
        s.setScrollbackLimit(100);
        s.setMarkNonce("n0");
        s.feed(session("ls", "a\r\nb\r\n", 0));
        s.feed(session("false", "", 1));
        s.feed(session("echo hi", "hi\r\n", 0));
        s.feed(mark("A") + "% ");
        auto cmds = s.commands();
        CHECK(cmds.size() == 4);
        if (cmds.size() == 4) {
            CHECK(cmds[0].ran && cmds[0].ended && cmds[0].status == 0);
            CHECK(cmds[1].ran && cmds[1].ended && cmds[1].status == 1);
            CHECK(!cmds[3].ran && !cmds[3].ended);      // the prompt waiting now
            CHECK(s.commandOutput(cmds[0]) == "a\nb");
            CHECK(s.commandOutput(cmds[1]) == "");
            CHECK(s.commandOutput(cmds[2]) == "hi");
            CHECK(s.lineText(cmds[2].promptLine) == "% echo hi");
            // Three commands and a prompt took seven lines: the first one is
            // in the scrollback now, and its number says so.
            CHECK(cmds[0].promptLine < 0 && s.lineText(cmds[0].promptLine) == "% ls");
            // Previous and next, from the prompt waiting: only commands
            // that ran.
            int last = cmds[3].promptLine;
            CHECK(s.previousCommandLine(last) == cmds[2].promptLine);
            CHECK(s.previousCommandLine(cmds[2].promptLine) == cmds[1].promptLine);
            CHECK(s.previousCommandLine(cmds[0].promptLine) == INT_MIN);
            CHECK(s.nextCommandLine(cmds[0].promptLine) == cmds[1].promptLine);
            CHECK(s.nextCommandLine(cmds[2].promptLine) == INT_MIN);
        }
        // Marks without the nonce are anyone's: kept nowhere.
        size_t before = s.markCount();
        s.feed("\x1b]133;A\x07\x1b]133;C;mc=other\x07\x1b]133;D;1\x07");
        CHECK(s.markCount() == before);
        // A command still running: its output runs to the cursor.
        s.feed("sleep\r\n" + mark("C") + "partial");
        cmds = s.commands();
        CHECK(!cmds.empty() && cmds.back().ran && !cmds.back().ended &&
              s.commandOutput(cmds.back()) == "partial");
    }

    GROUP("screen:marks-scroll");
    {
        // Marks stay on their text as it scrolls into the scrollback, and
        // go when it leaves.
        TerminalScreen s(20, 4);
        s.setScrollbackLimit(10);
        s.setMarkNonce("n0");
        s.feed(session("one", "1\r\n", 0));
        for (int i = 0; i < 5; i++) s.feed("x\r\n");
        auto cmds = s.commands();
        CHECK(cmds.size() == 1 && s.lineText(cmds[0].promptLine) == "% one" &&
              s.lineText(cmds[0].outputLine) == "1");
        for (int i = 0; i < 20; i++) s.feed("y\r\n");
        CHECK(s.commands().empty() && s.markCount() == 0);
        // Without a scrollback (Android) they go as their line leaves the
        // screen.
        TerminalScreen n(20, 3);
        n.feed(session("one", "", 0));
        CHECK(n.commands().size() == 1);
        for (int i = 0; i < 4; i++) n.feed("z\r\n");
        CHECK(n.commands().empty());
        // A scroll region moves the marks inside it, not those above it.
        TerminalScreen r(20, 6);
        r.feed("top\r\n" + mark("A", "") + "% p\r\n");
        r.feed("\x1b[3;6r\x1b[6;1H\n");   // region rows 3-6, a line feed at its bottom
        auto rc = r.commands();
        CHECK(rc.size() == 1 && rc[0].promptLine == 1 && r.lineText(1) == "% p");
        r.feed("\x1b[r\x1b[1;1H\x1b[2L");   // two lines inserted at the top
        rc = r.commands();
        CHECK(rc.size() == 1 && rc[0].promptLine == 3 && r.lineText(3) == "% p");
        // Clearing the screen drops its marks; clearing the scrollback, those.
        r.feed("\x1b[2J");
        CHECK(r.commands().empty());
    }

    GROUP("screen:marks-prompt-sp");
    {
        // Output without a final newline: zsh's PROMPT_SP fills the line,
        // wraps, and draws the prompt below. The prompt mark, which came
        // where the output stopped, goes down with it.
        TerminalScreen s(10, 5);
        s.setMarkNonce("n0");
        s.feed(mark("A") + "% printf x\r\n" + mark("C") + "x" + mark("D;0") + mark("A") +
               "\x1b[7m%\x1b[m" + std::string(9, ' ') + "\r \r% ");
        auto cmds = s.commands();
        CHECK(cmds.size() == 2);
        if (cmds.size() == 2) {
            CHECK(cmds[1].promptLine == 2 && cmds[1].promptCol == 0);
            CHECK(s.commandOutput(cmds[0]) == "x");
        }
    }

    GROUP("screen:marks-reflow");
    {
        // A resize carries the marks with their text, through rewrapped
        // lines and into the scrollback and back.
        TerminalScreen s(20, 6);
        s.setScrollbackLimit(100);
        s.setMarkNonce("n0");
        s.feed(session("long", std::string(35, 'w') + "\r\n", 3));
        s.feed(session("short", "s\r\n", 0));
        s.feed(mark("A") + "% ");
        std::string out0 = s.commandOutput(s.commands()[0]);
        CHECK(out0 == std::string(35, 'w'));
        for (int cols : {9, 33, 5, 20}) {
            s.resize(cols, cols == 5 ? 3 : 6);
            auto cmds = s.commands();
            CHECK(cmds.size() == 3);
            if (cmds.size() != 3) break;
            CHECK(s.lineText(cmds[0].promptLine) == "% long" || cols < 7);
            CHECK(s.commandOutput(cmds[0]) == out0);
            CHECK(s.commandOutput(cmds[1]) == "s");
            CHECK(cmds[0].status == 3);
            CHECK(s.previousCommandLine(cmds[2].promptLine) == cmds[1].promptLine);
        }
        // Many marks over a long scrollback reflow quickly (each line looks
        // only at its own marks).
        TerminalScreen big(80, 24);
        big.setScrollbackLimit(100000);
        big.setMarkNonce("n0");
        for (int i = 0; i < 5000; i++) big.feed(session("c", "line one\r\nline two\r\n", i % 2));
        CHECK(big.commands().size() == 5000);
        auto t0 = std::chrono::steady_clock::now();
        big.resize(50, 24);
        big.resize(80, 24);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              t0).count();
        CHECK(big.commands().size() == 5000);
        CHECK(big.commandOutput(big.commands()[10]) == "line one\nline two");
        std::printf("  screen:marks-reflow: 5,000 commands (15,000 lines) reflowed twice in "
                    "%.1f ms\n", ms);
        CHECK(ms < 2000);
        // Output scrolling under them costs nothing per mark: each line feed
        // looks the rows below the screen up by halving.
        std::string lines;
        for (int i = 0; i < 1000; i++) lines += "output line\r\n";
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; i++) big.feed(lines);
        double feedMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("  screen:marks: 100,000 lines scrolled under 5,000 marked commands in "
                    "%.0f ms\n", feedMs);
        CHECK(big.commands().size() < 5000);   // the oldest left the scrollback
    }
}
