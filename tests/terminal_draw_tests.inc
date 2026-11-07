// terminal_draw_tests.inc — the `screen:dirty-rows`, `screen:line-cells` and
// `fuzz:dirty-rows` groups of run_tests.cpp: the rows TerminalScreen says
// changed (takeDirty), which the grid view redraws and nothing else, and
// the by-reference cells it draws from.

namespace {

bool sameCell(const TermCell &a, const TermCell &b) {
    return a.ch == b.ch && a.style == b.style && a.width == b.width;
}

bool sameRow(const std::vector<TermCell> &a, const std::vector<TermCell> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!sameCell(a[i], b[i])) return false;
    return true;
}

// The rows marked, as a string of 0 and 1 (row 0 first).
std::string dirtyRows(TerminalScreen &s, TerminalScreen::Dirty *out = nullptr) {
    TerminalScreen::Dirty d = s.takeDirty();
    std::string r;
    for (uint8_t b : d.rows) r += b ? '1' : '0';
    if (out) *out = d;
    return r;
}

}  // namespace

void testTerminalDraw() {
    GROUP("screen:dirty-rows");
    {
        TerminalScreen s(10, 5);
        s.setScrollbackLimit(100);
        TerminalScreen::Dirty d;
        CHECK(dirtyRows(s, &d) == "11111" && d.all);   // new: everything
        CHECK(!s.takeDirty().any());                    // taken: nothing left
        s.feed("ab");
        CHECK(dirtyRows(s, &d) == "10000" && !d.all && d.scrolled == 0 && !d.marks);
        s.feed("\x1b[3;1Hxy");                          // a move, then a write
        CHECK(dirtyRows(s) == "00100");
        s.feed("\x1b[5;5H\x1b[1;2H\x1b[?25l\x1b[31m");    // moves and a style: no cells
        CHECK(!s.takeDirty().any());
        s.feed("\x1b[2K");                              // EL on row 0
        CHECK(dirtyRows(s) == "10000");
        s.feed("\x1b[4;1H\x1b[J");                       // ED from row 3 down
        CHECK(dirtyRows(s) == "00011");
        s.feed("\x1b[2;3H\x1b[2@");                      // ICH
        CHECK(dirtyRows(s) == "01000");
        s.feed("\x1b[2;3H\x1b[P\x1b[3X");                 // DCH, ECH
        CHECK(dirtyRows(s) == "01000");
        // A combining mark joins the cell before: that row.
        s.feed("\x1b[3;1He\xCC\x81");
        CHECK(dirtyRows(s) == "00100");
        s.feed("\x1b[3;2H\xCC\x81");
        CHECK(dirtyRows(s) == "00100");
        // A line feed at the bottom: every row moved, one line went up.
        s.feed("\x1b[5;1H\n");
        CHECK(dirtyRows(s, &d) == "11111" && !d.all && d.scrolled == 1);
        // A scroll region: only its rows.
        s.feed("\x1b[2;3r\x1b[3;1H\n");
        CHECK(dirtyRows(s, &d) == "01100" && d.scrolled == 0);
        s.feed("\x1b[2;1H\x1b" "M");                     // RI at the region's top
        CHECK(dirtyRows(s) == "01100");
        s.feed("\x1b[2;1H\x1b[L");                       // IL inside it
        CHECK(dirtyRows(s) == "01100");
        s.feed("\x1b[r");
        (void)s.takeDirty();
        // Autowrap: the row written and the next.
        s.feed("\x1b[1;9Habcd");
        CHECK(dirtyRows(s) == "11000");
        // A wide character at the right edge goes whole to the next row.
        s.feed("\x1b[3;10H日");
        CHECK(dirtyRows(s) == "00110");
        // REP repeats on its row.
        s.feed("\x1b[5;1Hz\x1b[3b");
        CHECK(dirtyRows(s) == "00001");
        // Everything: the screens switching, a reset, DECALN, a resize,
        // Cmd+K, the scrollback cleared.
        s.feed("\x1b[?1049h");
        CHECK(s.takeDirty().all);
        s.feed("\x1b[Hq");
        CHECK(dirtyRows(s) == "10000");
        s.feed("\x1b[?1049l");
        CHECK(s.takeDirty().all);
        s.feed("\x1b#8");
        CHECK(s.takeDirty().all);
        s.resize(12, 6);
        d = s.takeDirty();
        CHECK(d.all && d.rows.size() == 6);
        s.feed("\x1b[Hx");
        CHECK(dirtyRows(s) == "100000");
        s.clearAll();
        CHECK(s.takeDirty().all);
        for (int i = 0; i < 8; i++) s.feed("line\r\n");
        CHECK(s.takeDirty().scrolled == 3);
        s.feed("\x1b[3J");
        CHECK(s.takeDirty().all);
        s.feed("\x1b" "c");
        CHECK(s.takeDirty().all);
        // An unchanged scrollback limit is no change; a lower one that drops
        // lines is.
        for (int i = 0; i < 10; i++) s.feed("more\r\n");
        (void)s.takeDirty();
        s.setScrollbackLimit(100);
        CHECK(!s.takeDirty().any());
        s.setScrollbackLimit(1);
        CHECK(s.takeDirty().all);
        // The shell integration's marks.
        TerminalScreen m(20, 4);
        (void)m.takeDirty();
        m.feed("\x1b]133;A\x07% ");
        d = m.takeDirty();
        CHECK(d.marks && d.rows[0] == 1);
        m.feed("ls");
        CHECK(!m.takeDirty().marks);
        m.feed("\r\n\x1b]133;C\x07out\r\n\x1b]133;D;1\x07");
        CHECK(m.takeDirty().marks);
        // On the alternate screen marks are not kept: no change.
        m.feed("\x1b[?1049h");
        (void)m.takeDirty();
        m.feed("\x1b]133;A\x07");
        CHECK(!m.takeDirty().marks);
    }

    GROUP("screen:line-cells");
    {
        TerminalScreen s(6, 2);
        s.setScrollbackLimit(10);
        s.feed("ab日\x1b[31mc\r\nsecond\r\nthird");
        std::vector<TermCell> scratch;
        // A screen row: the stored cells themselves.
        const std::vector<TermCell> &r1 = s.lineCells(1, scratch);
        CHECK(&r1 == &s.row(1) && r1.size() == 6 && r1[0].ch == "t");
        CHECK(scratch.empty());
        // A scrollback line: unpacked, cell for cell as lineCell reads it.
        const std::vector<TermCell> &h = s.lineCells(-1, scratch);
        CHECK(&h == &scratch && h.size() == 5);
        bool same = true;
        for (int c = 0; c < (int)h.size(); c++) same = same && sameCell(h[c], s.lineCell(-1, c));
        CHECK(same && h[2].ch == "日" && h[2].width == 2 && h[3].width == 0);
        CHECK(h[4].style.fg.kind != TermColor::Default);
        // Outside the lines: nothing.
        CHECK(s.lineCells(-5, scratch).empty() && s.lineCells(2, scratch).empty());
    }

    GROUP("fuzz:dirty-rows");
    {
        // A host that copies only the rows takeDirty names (all of them when
        // it says all) always holds what the screen holds: no change goes
        // unreported. Random output, resizes and clears, as fuzz:terminal.
        const char *pieces[] = {
            "a", "b", " ", "\t", "\r", "\n", "\r\n", "日", "\xF0\x9F\x98\x80", "e\xCC\x81",
            "\xCC\x81", "\xE2\x80\x8D", "\xF0\x9F\x91\xA9", "\xF0\x9F\x87\xAF",
            "\xF0\x9F\x87\xB5", "\xEF\xB8\x8F", "\xE2\x9D\xA4",
            "\x1b[H", "\x1b[5;3H", "\x1b[J", "\x1b[1J", "\x1b[2J", "\x1b[3J", "\x1b[K",
            "\x1b[1K", "\x1b[2K", "\x1b[2L", "\x1b[2M", "\x1b[3@", "\x1b[@", "\x1b[3P",
            "\x1b[2X", "\x1b[2;4r", "\x1b[r", "\x1b[S", "\x1b[2T", "\x1b" "7", "\x1b" "8",
            "\x1b[?1049h", "\x1b[?1049l", "\x1b[?1047h", "\x1b[?1047l", "\x1b[?47h",
            "\x1b[?47l", "\x1b[?6h", "\x1b[?6l", "\x1b[?7l", "\x1b[?7h", "\x1b[4h", "\x1b[4l",
            "\x1b" "M", "\x1b" "D", "\x1b" "E", "\x1b[5b", "\x1b[42m", "\x1b[m", "\x1b[20C",
            "\x1b[3D", "\x1b[99G", "\x1b[2G", "\x1b]133;A\x07", "\x1b]133;C\x07",
            "\x1b]133;D;1\x07", "\x1b[!p", "\x1b" "c", "\x1b(0q\x1b(B", "\x1b#8",
            "\x1b]8;;https://x\x07L\x1b]8;;\x07", "\x1b[7m", "\x1b[38;2;1;2;3m"};
        const int kPieces = (int)(sizeof pieces / sizeof *pieces);
        int missed = 0, rounds = 0, partial = 0;
        std::string first;
        for (unsigned seed = 1; seed <= 3; seed++) {
            std::mt19937 rng(seed + 100);
            for (int round = 0; round < 120; round++, rounds++) {
                TerminalScreen s(2 + rng() % 20, 1 + rng() % 8);
                s.setScrollbackLimit(rng() % 3 == 0 ? 0 : 1 + rng() % 12);
                std::vector<std::vector<TermCell>> host;
                for (int step = 0; step < 40; step++) {
                    int n = rng() % 12;
                    for (int i = 0; i < n; i++) s.feed(pieces[rng() % kPieces]);
                    if (rng() % 8 == 0) s.resize(rng() % 25, 1 + rng() % 9);
                    if (rng() % 20 == 0) s.clearToCursorLine();
                    if (rng() % 25 == 0) s.clearAll();
                    TerminalScreen::Dirty d = s.takeDirty();
                    if (d.all || host.size() != (size_t)s.rows()) {
                        host.assign(s.rows(), {});
                        for (int r = 0; r < s.rows(); r++) host[r] = s.row(r);
                    } else {
                        partial++;
                        for (int r = 0; r < s.rows(); r++)
                            if (d.rows[r]) host[r] = s.row(r);
                    }
                    for (int r = 0; r < s.rows(); r++) {
                        if (sameRow(host[r], s.row(r))) continue;
                        if (missed++ == 0)
                            first = "row " + std::to_string(r) + " (seed " +
                                    std::to_string(seed + 100) + ", round " +
                                    std::to_string(round) + ", step " + std::to_string(step) + ")";
                        host[r] = s.row(r);
                    }
                }
            }
        }
        if (missed) std::printf("  fuzz:dirty-rows: %d rows missed, first %s\n", missed, first.c_str());
        CHECK(missed == 0);
        CHECK(rounds == 360 && partial > 5000);
    }
}
