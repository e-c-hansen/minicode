// terminal_fuzz.inc — the `fuzz:terminal` group of run_tests.cpp: random
// sessions of escape-heavy output, resizes and clears fed to TerminalScreen
// and TerminalStream, with the screen's structure checked after every
// piece. A structural fuzzer like this one found the split wide characters
// of insert mode and ICH in seconds, and a heap corruption at one column.
// Seeded, so a failure repeats; the rounds are kept short for `make test`
// (run it under ASan by hand for more: the seeds and counts are below).

#include <random>

namespace {

// What must hold of a screen whatever was fed to it: every wide character
// whole (a left half followed by its right half, a right half after its
// left), the cursor on the screen, no cell longer than its character plus
// kTermMaxCellBytes of marks, and the same of every scrollback line.
// Returns an empty string, or what is wrong.
std::string screenProblem(const TerminalScreen &s) {
    for (int r = 0; r < s.rows(); r++) {
        for (int c = 0; c < s.cols(); c++) {
            const TermCell &cell = s.cell(r, c);
            if (cell.width == 2 && (c + 1 >= s.cols() || s.cell(r, c + 1).width != 0))
                return "orphan left half at row " + std::to_string(r) + " col " + std::to_string(c);
            if (cell.width == 0 && (c == 0 || s.cell(r, c - 1).width != 2))
                return "orphan right half at row " + std::to_string(r) + " col " + std::to_string(c);
            if (cell.ch.size() > kTermMaxCellBytes)
                return "a cell of " + std::to_string(cell.ch.size()) + " bytes";
        }
    }
    if (s.cursorRow() < 0 || s.cursorRow() >= s.rows() || s.cursorCol() < 0 ||
        s.cursorCol() >= s.cols())
        return "cursor off the screen";
    for (int l = -s.historySize(); l < 0; l++) {
        int w = s.lineWidth(l);
        for (int c = 0; c < w; c++) {
            TermCell cell = s.lineCell(l, c);
            if (cell.width == 2 && (c + 1 >= w || s.lineCell(l, c + 1).width != 0))
                return "scrollback orphan at line " + std::to_string(l);
            if (cell.ch.size() > kTermMaxCellBytes) return "a long scrollback cell";
        }
    }
    // The shell integration's marks stay on lines that exist.
    auto onALine = [&](int line) { return line >= -s.historySize() && line < s.rows(); };
    int lastPrompt = INT_MIN;
    for (const TermCommand &c : s.commands()) {
        if (!onALine(c.promptLine) || (c.ran && !onALine(c.outputLine)) ||
            (c.ended && !onALine(c.endLine)))
            return "a mark off the lines, prompt at " + std::to_string(c.promptLine);
        if (c.promptLine < lastPrompt) return "marks out of order";   // they are found by halving
        lastPrompt = c.promptLine;
    }
    return "";
}

void testTerminalFuzz() {
    const double started = cpuMs();
    GROUP("fuzz:terminal");
    {
        const char *pieces[] = {
            "a", "b", " ", "\t", "\r", "\n", "\r\n", "日", "\xF0\x9F\x98\x80", "e\xCC\x81",
            "\xCC\x81", "\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81",
            "\x1b[H", "\x1b[5;3H", "\x1b[J", "\x1b[1J", "\x1b[2J", "\x1b[3J", "\x1b[K",
            "\x1b[1K", "\x1b[2K", "\x1b[2L", "\x1b[2M", "\x1b[3@", "\x1b[@", "\x1b[3P",
            "\x1b[2X", "\x1b[2;4r", "\x1b[r", "\x1b[S", "\x1b[2T", "\x1b" "7", "\x1b" "8",
            "\x1b[?1049h", "\x1b[?1049l", "\x1b[?1047h", "\x1b[?1047l", "\x1b[?47h",
            "\x1b[?47l", "\x1b[?6h", "\x1b[?6l", "\x1b[?7l", "\x1b[?7h", "\x1b[4h", "\x1b[4l",
            "\x1b" "M", "\x1b" "D", "\x1b" "E", "\x1b[5b", "\x1b[42m", "\x1b[m", "\x1b[20C",
            "\x1b[3D", "\x1b[99G", "\x1b[2G", "\x1b[s", "\x1b[u", "\x1b]133;A\x07",
            "\x1b[!p", "\x1b" "c", "\x1b[3Z", "\x1b[2I", "\x1b(0q\x1b(B", "\x1b[?1048h",
            "\x1b[?1048l", "\x1b#8", "\x1bP", "\x1b\\", "\x18", "\x1a", "\x1b]2;t",
            "\x07", "\x1b[38:2::1:2:3;48;2;4;5;6;58:2::7:8:9;4:3;1m", "\x1b[?2026h",
            "\x1b[?2026l", "\x1b_apc", "\xC3", "\xA9",
            // Emoji sequences: a joiner, a person, a flag's letters, VS16 and a
            // heart, a skin tone; and the shell integration's other marks.
            "\xE2\x80\x8D", "\xF0\x9F\x91\xA9", "\xF0\x9F\x87\xAF", "\xF0\x9F\x87\xB5",
            "\xEF\xB8\x8F", "\xE2\x9D\xA4", "\xF0\x9F\x8F\xBD", "\x1b]133;C\x07",
            "\x1b]133;D;1\x07",
            // Mode 2027, the cursor's shape, the kitty keyboard flags,
            // queries, and the newer SGR attributes.
            "\x1b[?2027l", "\x1b[?2027h", "\xE0\xA4\xBF", "\x1b[5 q", "\x1b[>1u",
            "\x1b[<u", "\x1b[=1;2u", "\x1b[?u", "\x1b]11;?\x07", "\x1b]4;1;?\x1b\\",
            "\x1b[c", "\x1b[>c", "\x1b[>q", "\x1b[4:3;58;5;9;8;53m", "\x1b[59;28;55m"};
        const int kPieces = (int)(sizeof pieces / sizeof *pieces);
        int problems = 0, rounds = 0;
        std::string first;
        for (unsigned seed = 1; seed <= 3; seed++) {
            std::mt19937 rng(seed);
            for (int round = 0; round < 120; round++, rounds++) {
                TerminalScreen s(2 + rng() % 20, 1 + rng() % 8);
                s.setScrollbackLimit(rng() % 3 == 0 ? 0 : 1 + rng() % 12);
                TerminalStream st;
                for (int step = 0; step < 40; step++) {
                    int n = rng() % 25;
                    for (int i = 0; i < n; i++) {
                        const char *piece = pieces[rng() % kPieces];
                        s.feed(piece);
                        st.feed(piece);
                        std::string p = screenProblem(s);
                        if (!p.empty() && problems++ == 0)
                            first = p + " (seed " + std::to_string(seed) + ", round " +
                                    std::to_string(round) + ")";
                    }
                    if (rng() % 4 == 0) {
                        std::vector<TermPos> keep;
                        for (int k = 0; k < 3; k++)
                            keep.push_back({(int)(rng() % 30) - 15, (int)(rng() % 25)});
                        // 0 and 1 columns are asked for too: both mean 2.
                        s.resize(rng() % 25, 1 + rng() % 9, &keep);
                        s.feed("Q日");
                        std::string p = screenProblem(s);
                        if (!p.empty() && problems++ == 0) first = "after a resize: " + p;
                    }
                    if (rng() % 15 == 0) s.clearToCursorLine();
                    if (rng() % 10 == 0) (void)s.find(rng() % 2 ? "a" : "日", rng() % 2);
                }
            }
        }
        if (problems) std::printf("  fuzz:terminal: %d problems, first %s\n", problems, first.c_str());
        CHECK(problems == 0);
        CHECK(rounds == 360);
    }

    GROUP("fuzz:terminal-shrink");
    {
        // Making the screen shorter and back loses no line, wherever the
        // cursor is: lines below it that no longer fit are set aside, and
        // lines above it go into the scrollback, and both come back.
        std::mt19937 rng(5);
        const char *chars[] = {"a", "b", "c", " ", "日", "é", "\xF0\x9F\x98\x80", "xyz"};
        int lost = 0, cursorMoved = 0;
        for (int round = 0; round < 400; round++) {
            int cols = 3 + rng() % 20, rows = 2 + rng() % 8;
            TerminalScreen s(cols, rows);
            s.setScrollbackLimit(1000);
            int lines = rng() % 12;
            for (int l = 0; l < lines; l++) {
                int n = rng() % (2 * cols + 3);
                for (int i = 0; i < n; i++) s.feed(chars[rng() % 8]);
                if (l + 1 < lines) s.feed("\r\n");
            }
            char buf[32];
            std::snprintf(buf, sizeof buf, "\033[%d;%dH", 1 + (int)(rng() % rows),
                          1 + (int)(rng() % cols));
            s.feed(buf);
            std::string under = s.cell(s.cursorRow(), s.cursorCol()).ch;
            auto before = logicalCells(s);
            s.resize(cols, 1 + rng() % rows);
            s.resize(cols, rows);
            if (logicalCells(s) != before) lost++;
            if (s.cell(s.cursorRow(), s.cursorCol()).ch != under) cursorMoved++;
        }
        CHECK(lost == 0);
        CHECK(cursorMoved == 0);
    }
    const double fuzzMs = cpuMs() - started;
    std::printf("  fuzz:terminal: 360 random sessions and 400 shrinks in %.0f ms\n", fuzzMs);
    CHECK_TIME(fuzzMs, 10000);   // 0.6 to 0.9 s on an M3, 3.8 s on its efficiency cores
}

// The marks' nonce, the parser's limits and the rest of the terminal
// models' hardening.
void testTerminalTrust() {
    auto kinds = [](const std::vector<TermEvent> &ev) {
        std::string k;
        for (const TermEvent &e : ev) {
            switch (e.kind) {
            case TermEvent::Line: k += 'L'; break;
            case TermEvent::PromptStart: k += 'A'; break;
            case TermEvent::InputStart: k += 'B'; break;
            case TermEvent::CommandStart: k += 'C'; break;
            case TermEvent::CommandEnd: k += 'D'; break;
            case TermEvent::Directory: k += '7'; break;
            case TermEvent::Title: k += 'T'; break;
            }
        }
        return k;
    };

    GROUP("stream:mark-nonce");
    {
        TerminalStream st;
        st.setMarkNonce("c0ffee");
        // Marks without the nonce, or with another, are anyone's: dropped.
        CHECK(kinds(st.feed("\033]133;A\007\033]133;B\007\033]7;file:///forged\007")) == "");
        CHECK(kinds(st.feed("\033]133;A;mc=bad\007\033]133;B;mc=c0ffeex\007")) == "");
        CHECK(kinds(st.feed("\033]133;A;mc=c0ffee;\007\033]133;A;mc=c0ffe\007")) == "");
        // With it, they count, and read as they did.
        auto ev = st.feed("\033]133;A;mc=c0ffee\007\033]133;B;mc=c0ffee\007"
                          "\033]633;E;ls -l x;y;mc=c0ffee\007\033]133;C;mc=c0ffee\007"
                          "\033]133;D;3;mc=c0ffee\007"
                          "\033]7;file://h/tmp/a%3Bb%07;mc=c0ffee\033\\");
        CHECK(kinds(ev) == "ABCD7");
        if (ev.size() == 5) {
            CHECK(ev[2].text == "ls -l x;y");
            CHECK(ev[3].status == 3);
            CHECK(ev[4].text == "/tmp/a;b\007");
        }
        // A forged 633;E never names a command: it must carry the nonce too.
        auto c = st.feed("\033]633;E;rm -rf /\007\033]133;C;mc=c0ffee\007");
        CHECK(kinds(c) == "C" && c[0].text.empty());
        // Titles are not marks, and need none.
        CHECK(kinds(st.feed("\033]2;vim\007")) == "T");
        // No nonce set (Android, the tests above): every mark counts.
        TerminalStream open;
        CHECK(kinds(open.feed("\033]133;A\007\033]133;D;1\007")) == "AD");
    }

    GROUP("stream:events-only");
    {
        TerminalStream st;
        st.setEventsOnly(true);
        auto ev = st.feed("hello\r\n\033[31mred\033[m\033]133;A\007more\t\b\r\n\033]2;t\007");
        CHECK(kinds(ev) == "AT");
        TerminalStream lines;
        CHECK(kinds(lines.feed("hello\r\n\033]133;A\007")) == "LA");
    }

    GROUP("stream:string-sequences-end");
    {
        // ESC ends a DCS (or SOS, PM, APC) body and starts a new sequence,
        // so a binary file's stray ESC P no longer eats the next prompt's
        // marks; CAN and SUB abort it, and an OSC too.
        TerminalStream st;
        CHECK(kinds(st.feed("\033Pbinary junk\033]133;D;0\007\033]133;A\007")) == "DA");
        Panel p = run("\033Pxx\030visible\033_apc\032 too\033]0;t\030!");
        CHECK(p.text() == "visible too!");
        CHECK(run("a\033Pq#0;2\033\\b").text() == "ab");      // ST still ends it
        CHECK(run("a\033Pq\007b").text() == "ab");            // and BEL, as before
    }

    GROUP("stream:csi-limits");
    {
        // A long truecolor SGR (past the old 64-byte limit) applies.
        std::string sgr =
            "\033[38;2;255;128;1;48;2;250;251;252;58:2::255:255:255;4:3;1;3;9;22;1mz";
        Panel p = run(sgr);
        const TermStyle *z = styleOf(p, "z");
        CHECK(z && z->fg.kind == TermColor::RGB && z->fg.r == 255 && z->fg.g == 128 &&
              z->bg.b == 252 && z->bold && z->italic && z->strike);
        TerminalScreen s(10, 2);
        s.feed(sgr);
        CHECK(s.cell(0, 0).style.bg.kind == TermColor::RGB && s.cell(0, 0).style.bold);
        // More than 32 parameters, or 512 bytes: dropped whole, the final
        // byte included.
        std::string many = "\033[";
        for (int i = 0; i < 40; i++) many += "1;";
        Panel m = run(many + "31mz");
        CHECK(m.text() == "z" && *styleOf(m, "z") == TermStyle());
        Panel lc = run("\033[" + std::string(600, '1') + "mz");
        CHECK(lc.text() == "z" && *styleOf(lc, "z") == TermStyle());
    }

    GROUP("term:combining-cap");
    {
        // Combining marks stop at kTermMaxCellBytes a cell, in both models.
        std::string marks;
        for (int i = 0; i < 5000; i++) marks += "\xCC\x81";
        TerminalScreen s(10, 3);
        s.setScrollbackLimit(10);
        s.feed("a" + marks + "b");
        CHECK(s.cell(0, 0).ch.size() <= kTermMaxCellBytes && s.cell(0, 0).ch[0] == 'a');
        CHECK(s.cell(0, 1).ch == "b");
        Panel p = run("a" + marks + "b\n");
        CHECK(p.text().size() <= kTermMaxCellBytes + 2 && p.text().back() == '\n');
        // A flag's tag sequence and an accented letter keep every mark.
        std::string flag = "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5"
                           "\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF";
        TerminalScreen f(10, 2);
        f.feed(flag + "e\xCC\x81\xCC\xA3");
        CHECK(f.cell(0, 0).ch == flag && f.cell(0, 2).ch == "e\xCC\x81\xCC\xA3");
        // A line of many rows with marks reflows (its joined text past what
        // the old 24-bit offsets held), keeps its cells, and find reads it.
        TerminalScreen big(400, 5);
        big.setScrollbackLimit(2000);
        std::string cell = "a";
        for (int i = 0; i < 15; i++) cell += "\xCC\x81";
        std::string chunk;
        for (int i = 0; i < 400; i++) chunk += cell;
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 1500; r++) big.feed(chunk);
        big.feed("Z\r\n");
        big.resize(399, 5);
        std::printf("  term:combining-cap: a 18 MB line of marks fed and reflowed in %.0f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              t0).count());
        int last = -1;
        for (int l = -big.historySize(); l < big.rows(); l++)
            if (big.lineText(l).find('Z') != std::string::npos) last = l;
        CHECK(last != -1 && big.lineCell(last, 0).ch == cell);
        CHECK(big.lineCell(-big.historySize(), 398).ch == cell);
        CHECK(big.find("Z", true).size() == 1);
    }

    GROUP("term:insert-wide");
    {
        // ICH and insert mode with the cursor on either half of a wide
        // character: no half is left without the other.
        for (int col : {1, 2, 3, 4, 5}) {
            TerminalScreen s(8, 1);
            s.feed("a日本b");
            s.feed("\033[" + std::to_string(col) + "G\033[@");
            CHECK(screenProblem(s).empty());
            TerminalScreen i(8, 1);
            i.feed("a日本b");
            i.feed("\033[" + std::to_string(col) + "G\033[4hx");
            CHECK(screenProblem(i).empty());
        }
        // On the left half the character moves whole.
        TerminalScreen s(8, 1);
        s.feed("a日b\033[2G\033[@");
        CHECK(s.rowText(0) == "a 日b");
        TerminalScreen r(8, 1);
        r.feed("a日b\033[3G\033[@");      // on its right half it goes
        CHECK(r.rowText(0) == "a   b");
    }

    GROUP("term:soft-reset");
    {
        // DECSTR (which the host sends after a shell died) puts the
        // character sets back, sends the saved cursor home and ends a
        // synchronized frame.
        TerminalScreen s(10, 3);
        s.feed("\033[3;5H\0337\033(0\033[?2026h");
        CHECK(s.synchronizedOutput());
        int ended = s.synchronizedFramesEnded();
        s.feed("\033[!p");
        CHECK(!s.synchronizedOutput() && s.synchronizedFramesEnded() == ended + 1);
        s.feed("\033[1;1Hq");
        CHECK(s.cell(0, 0).ch == "q");
        s.feed("\033[2;3H\0338X");
        CHECK(s.cell(0, 0).ch == "X" && s.cell(2, 4).ch == " ");
    }

    GROUP("term:two-columns");
    {
        TerminalScreen s(1, 3);
        CHECK(s.cols() == 2);
        s.feed("日日\033[@日\033[4h日\033[4l");
        CHECK(screenProblem(s).empty());
        s.resize(0, 2);
        CHECK(s.cols() == 2 && screenProblem(s).empty());
    }

    GROUP("term:shrink-keeps-lines");
    {
        // A completion list under the prompt survives dragging the pane
        // shorter and back.
        TerminalScreen s(20, 10);
        s.setScrollbackLimit(100);
        s.feed("% ls\r\n");
        for (int i = 1; i <= 6; i++) s.feed("item" + std::to_string(i) + "\r\n");
        s.feed("\033[1;6H");          // the cursor back on the prompt line
        auto before = logicalCells(s);
        s.resize(20, 3);
        CHECK(s.cursorRow() == 0 && s.rowText(0) == "% ls");
        s.resize(20, 10);
        CHECK(logicalCells(s) == before && s.rowText(6) == "item6");
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 5);
        // Set-aside lines do not come back once the screen has scrolled.
        s.resize(20, 3);
        s.feed("\033[3;1H\r\nnew\r\n");
        s.resize(20, 10);
        bool stale = false;
        for (int r = 0; r < s.rows(); r++)
            if (s.rowText(r).rfind("item6", 0) == 0) stale = true;
        CHECK(!stale);
    }

    GROUP("term:title-clean");
    {
        // Bidirectional and format controls go from titles (U+202E reversed
        // a tab's label), with C0 and C1 controls and bytes that are not
        // UTF-8.
        std::string t = "ab\xE2\x80\xAE" "c\xE2\x81\xA6" "d\xE2\x80\x8B" "e\xC2\x9B" "f\xFF" "g";
        CHECK(termCleanTitle(t) == "abcdefg");
        CHECK(termCleanTitle("caf\xC3\xA9 \xE6\x97\xA5") == "caf\xC3\xA9 \xE6\x97\xA5");
        CHECK(termCleanTitle(std::string(300, 'x') + "\xC3\xA9").size() == 256);
        CHECK(termCleanTitle(std::string(255, 'x') + "\xC3\xA9") == std::string(255, 'x'));
        TerminalStream st;
        auto ev = st.feed("\033]2;l\xE2\x80\xAEr\007");
        CHECK(ev.size() == 1 && ev[0].text == "lr");
        TerminalScreen s(10, 2);
        s.feed("\033]0;x\xE2\x81\xA7y\007");
        CHECK(s.title() == "xy");
    }
}

} // namespace
