// terminal_vt_tests.inc — the `term:vt-conformance`, `term:sgr-underline`,
// `term:kitty-keys` and `term:mode-2027` groups of run_tests.cpp: what
// TerminalScreen does with each control sequence a program sends, case by
// case, as an esctest-style group: the screen's text, attributes, cursor
// and replies after each. The cases are written from xterm's control
// sequence document and the kitty keyboard protocol's specification (no
// code or cases copied from esctest or vttest).

namespace {

// A prompt mark with the nonce the screen was given, as zsh prints it.
std::string vtMark(const std::string &body) { return "\x1b]133;" + body + ";mc=vt\x07"; }

// The screen after `bytes`, with the shell integration's nonce set.
TerminalScreen vtScreen(const std::string &bytes, int cols = 10, int rows = 5) {
    TerminalScreen s(cols, rows);
    s.setMarkNonce("vt");
    s.feed(bytes);
    return s;
}

// Columns the characters in `text` take when printed at column 0 of an
// empty screen (with `before` fed first: marks, modes).
int vtColumns(const std::string &before, const std::string &text) {
    TerminalScreen s = vtScreen(before, 40, 3);
    s.feed("\r" + text);
    return s.cursorCol();
}

bool vtIndexed(const TermColor &c, int index) {
    return c.kind == TermColor::Indexed && c.index == index;
}

}  // namespace

void testTerminalVt() {
    GROUP("term:vt-conformance");
    {
        // Device attributes: DA1 a VT220 with ANSI color; DA2 type 1, the
        // version as a number, ROM 0; XTVERSION the name and version in a
        // DCS. Only parameter 0 asks.
        TerminalScreen s(10, 4);
        s.setVersion("1.5.8");
        s.feed("\x1b[c\x1b[0c\x1b[1c");
        CHECK(s.takeReplies() == "\x1b[?62;22c\x1b[?62;22c");
        s.feed("\x1b[>c\x1b[>0c\x1b[>1c");
        CHECK(s.takeReplies() == "\x1b[>1;10508;0c\x1b[>1;10508;0c");
        s.feed("\x1b[>q\x1b[>0q\x1b[>1q");
        CHECK(s.takeReplies() == "\x1bP>|minicode 1.5.8\x1b\\\x1bP>|minicode 1.5.8\x1b\\");
        s.setVersion("2.10");
        s.feed("\x1b[>c");
        CHECK(s.takeReplies() == "\x1b[>1;21000;0c");
        s.setVersion("");
        s.feed("\x1b[>q");
        CHECK(s.takeReplies() == "\x1bP>|minicode\x1b\\");
        CHECK(s.text().empty());                    // nothing printed
    }
    {
        // OSC 10, 11 and 12 answer the host's colors, ended as asked (BEL
        // or ST); "10;?;?" asks for 10 and 11; OSC 4 for palette entries.
        TerminalScreen s(10, 4);
        s.feed("\x1b]11;?\x07");
        CHECK(s.takeReplies() == "\x1b]11;rgb:1818/1818/1818\x07");   // the dark default
        s.setThemeColors(0x333333, 0xFFFFFF, 0x0000FF);
        s.feed("\x1b]10;?\x1b\\\x1b]11;?\x07\x1b]12;?\x07");
        CHECK(s.takeReplies() == "\x1b]10;rgb:3333/3333/3333\x1b\\"
                                 "\x1b]11;rgb:ffff/ffff/ffff\x07"
                                 "\x1b]12;rgb:0000/0000/ffff\x07");
        s.feed("\x1b]10;?;?\x07");
        CHECK(s.takeReplies() == "\x1b]10;rgb:3333/3333/3333\x07\x1b]11;rgb:ffff/ffff/ffff\x07");
        s.feed("\x1b]4;1;?;196;?\x07");
        CHECK(s.takeReplies() == "\x1b]4;1;rgb:cdcd/3131/3131\x07\x1b]4;196;rgb:ffff/0000/0000\x07");
        s.feed("\x1b]11;#000000\x07\x1b]4;300;?\x07\x1b]4;x;?\x07\x1b]12;red\x07");
        CHECK(s.takeReplies().empty());             // setting, and nonsense: no answer
        CHECK(s.text().empty());
    }
    {
        // DECRQM answers every mode the screen keeps: 1 set, 2 reset,
        // 0 not known.
        TerminalScreen s(10, 4);
        auto ask = [&](const std::string &mode) {
            s.takeReplies();
            s.feed("\x1b[" + mode + "$p");
            return s.takeReplies();
        };
        CHECK(ask("?1") == "\x1b[?1;2$y");
        CHECK(ask("?7") == "\x1b[?7;1$y");
        CHECK(ask("?25") == "\x1b[?25;1$y");
        CHECK(ask("?12") == "\x1b[?12;2$y");
        CHECK(ask("?66") == "\x1b[?66;2$y");
        CHECK(ask("?2027") == "\x1b[?2027;1$y");
        CHECK(ask("?9999") == "\x1b[?9999;0$y");
        CHECK(ask("4") == "\x1b[4;2$y");
        s.feed("\x1b[?1;12;66;1004;2004;2026h\x1b[4h\x1b[?7l");
        CHECK(ask("?1") == "\x1b[?1;1$y");
        CHECK(ask("?12") == "\x1b[?12;1$y");
        CHECK(ask("?66") == "\x1b[?66;1$y" && s.appKeypad());
        CHECK(ask("?1004") == "\x1b[?1004;1$y");
        CHECK(ask("?2004") == "\x1b[?2004;1$y");
        CHECK(ask("?2026") == "\x1b[?2026;1$y");
        CHECK(ask("?7") == "\x1b[?7;2$y");
        CHECK(ask("4") == "\x1b[4;1$y");
        s.feed("\x1b[?66l");
        CHECK(!s.appKeypad());
    }
    {
        // DECSCUSR: 0 the host's shape, 1 and 2 block, 3 and 4 underline,
        // 5 and 6 bar, odd ones blinking; 7 and up ignored; RIS and
        // DECSTR put it back.
        TerminalScreen s(10, 4);
        CHECK(s.cursorShape() == TermCursorShape::Default && !s.cursorBlinks());
        s.feed("\x1b[6 q");
        CHECK(s.cursorShape() == TermCursorShape::Bar && !s.cursorBlinks());
        s.feed("\x1b[5 q");
        CHECK(s.cursorShape() == TermCursorShape::Bar && s.cursorBlinks());
        s.feed("\x1b[4 q");
        CHECK(s.cursorShape() == TermCursorShape::Underline && !s.cursorBlinks());
        s.feed("\x1b[3 q");
        CHECK(s.cursorShape() == TermCursorShape::Underline && s.cursorBlinks());
        s.feed("\x1b[2 q");
        CHECK(s.cursorShape() == TermCursorShape::Block && !s.cursorBlinks());
        s.feed("\x1b[1 q");
        CHECK(s.cursorShape() == TermCursorShape::Block && s.cursorBlinks());
        s.feed("\x1b[7 q");
        CHECK(s.cursorShape() == TermCursorShape::Block && s.cursorBlinks());
        s.feed("\x1b[ q");
        CHECK(s.cursorShape() == TermCursorShape::Default && !s.cursorBlinks());
        s.feed("\x1b[6 q\x1b[!p");
        CHECK(s.cursorShape() == TermCursorShape::Default);
        s.feed("\x1b[3 q\x1b" "c");
        CHECK(s.cursorShape() == TermCursorShape::Default && !s.cursorBlinks());
        CHECK(s.text().empty());                    // " q" printed nothing
    }
    {
        // Cursor movement: CUP is 1-based and clamps; CUU and CUD stop at
        // the scroll region's margins from inside it; CUF and CUB stop at
        // the edges; CHA, HPA, VPA, CNL, CPL, HPR and VPR.
        TerminalScreen s(10, 6);
        s.feed("\x1b[3;4H");
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 3);
        s.feed("\x1b[99;99H");
        CHECK(s.cursorRow() == 5 && s.cursorCol() == 9);
        s.feed("\x1b[H");
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
        s.feed("\x1b[20C");
        CHECK(s.cursorCol() == 9);
        s.feed("\x1b[3D");
        CHECK(s.cursorCol() == 6);
        s.feed("\x1b[2G");
        CHECK(s.cursorCol() == 1);
        s.feed("\x1b[5`");
        CHECK(s.cursorCol() == 4);
        s.feed("\x1b[4d");
        CHECK(s.cursorRow() == 3 && s.cursorCol() == 4);
        s.feed("\x1b[2E");
        CHECK(s.cursorRow() == 5 && s.cursorCol() == 0);
        s.feed("\x1b[3F");
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 0);
        s.feed("\x1b[2a\x1b[e");
        CHECK(s.cursorRow() == 3 && s.cursorCol() == 2);
        s.feed("\x1b[2;4r\x1b[3;1H\x1b[9A");     // inside the region: stops at its top
        CHECK(s.cursorRow() == 1);
        s.feed("\x1b[9B");
        CHECK(s.cursorRow() == 3);
        s.feed("\x1b[6;1H\x1b[9A");               // below it: stops at the top margin too
        CHECK(s.cursorRow() == 1);
        s.feed("\x1b[1;1H\x1b[9B");               // above it: stops at the bottom margin
        CHECK(s.cursorRow() == 3);
    }
    {
        // Scroll regions: a line feed at the bottom margin scrolls only the
        // region; RI at the top margin scrolls it down; SU and SD.
        TerminalScreen s(4, 5);
        s.feed("a\r\nb\r\nc\r\nd\r\ne");
        s.feed("\x1b[2;4r\x1b[4;1H\n");
        CHECK(s.text() == "a\nc\nd\n\ne");
        s.feed("\x1b[2;1H\x1bM");
        CHECK(s.text() == "a\n\nc\nd\ne");
        s.feed("\x1b[S");
        CHECK(s.text() == "a\nc\nd\n\ne");
        s.feed("\x1b[2T");
        CHECK(s.text() == "a\n\n\nc\ne");
        s.feed("\x1b[r");                          // reset: the whole screen, cursor home
        CHECK(s.scrollTop() == 0 && s.scrollBottom() == 4 && s.cursorRow() == 0);
    }
    {
        // Insert and delete: ICH, DCH, ECH on the line; IL and DL from the
        // cursor's line within the region, the cursor to column 0.
        TerminalScreen s(6, 4);
        s.feed("abcdef\x1b[1;3H\x1b[2@");
        CHECK(s.rowText(0) == "ab  cd" && s.cursorCol() == 2);
        s.feed("\x1b[3P");
        CHECK(s.rowText(0) == "abd");
        s.feed("\x1b[1;1H\x1b[2X");
        CHECK(s.rowText(0) == "  d" && s.cursorCol() == 0);
        s.feed("\x1b[2;1H1\r\n2\r\n3\x1b[2;2H\x1b[L");
        CHECK(s.text() == "  d\n\n1\n2" && s.cursorCol() == 0);
        s.feed("\x1b[2M");
        CHECK(s.text() == "  d\n2");
        s.feed("\x1b[1;1H\x1b[4h" "XY\x1b[4l");      // insert mode
        CHECK(s.rowText(0) == "XY  d");
    }
    {
        // Tab stops: every 8 by default; HTS sets one, TBC 0 clears the
        // one at the cursor, TBC 3 all; CHT and CBT move by them.
        TerminalScreen s(20, 2);
        s.feed("\t");
        CHECK(s.cursorCol() == 8);
        s.feed("\t\t");
        CHECK(s.cursorCol() == 19);               // no stop left: the last column
        s.feed("\r\x1b[3G\x1bH\r\t");
        CHECK(s.cursorCol() == 2);
        s.feed("\x1b[g\r\t");
        CHECK(s.cursorCol() == 8);
        s.feed("\x1b[2I");
        CHECK(s.cursorCol() == 19);
        s.feed("\x1b[Z");
        CHECK(s.cursorCol() == 16);
        s.feed("\x1b[3g\r\t");
        CHECK(s.cursorCol() == 19);
    }
    {
        // DECSC and DECRC keep the cursor, the style, origin mode and the
        // character set; SCOSC and SCORC the same slot.
        TerminalScreen s(10, 4);
        s.feed("\x1b[2;3H\x1b[1;31m\x1b(0\x1b" "7\x1b[H\x1b[m\x1b(B" "x\x1b" "8q");
        CHECK(s.cursorRow() == 1 && s.cursorCol() == 3);
        CHECK(s.cell(1, 2).ch == "─");       // q in line drawing
        CHECK(s.cell(1, 2).style.bold && vtIndexed(s.cell(1, 2).style.fg, 1));
        CHECK(s.cell(0, 0).ch == "x" && !s.cell(0, 0).style.bold);
        s.feed("\x1b[4;5H\x1b[s\x1b[H\x1b[u");
        CHECK(s.cursorRow() == 3 && s.cursorCol() == 4);
    }
    {
        // Origin mode: CUP and VPA count from the region's top and stay in
        // it; DSR 6 answers relative to it; reset sends the cursor home.
        TerminalScreen s(10, 6);
        s.feed("\x1b[2;4r\x1b[?6h");
        CHECK(s.cursorRow() == 1 && s.cursorCol() == 0);
        s.feed("\x1b[2;3H");
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        s.feed("\x1b[9;1H");
        CHECK(s.cursorRow() == 3);                // clamped to the bottom margin
        s.feed("\x1b[6n");
        CHECK(s.takeReplies() == "\x1b[3;1R");
        s.feed("\x1b[?6l");
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
    }
    {
        // DECSTR: style, insert, origin and autowrap back, margins the
        // whole screen, the cursor shown; the text and cursor stay.
        TerminalScreen s(6, 4);
        s.feed("ab\x1b[2;3r\x1b[?6h\x1b[4h\x1b[?7l\x1b[?25l\x1b[1;4m\x1b[3;2H");
        s.feed("\x1b[!p");
        CHECK(s.rowText(0) == "ab");
        CHECK(s.scrollTop() == 0 && s.scrollBottom() == 3);
        CHECK(s.autowrap() && s.cursorVisible());
        s.feed("Z");
        CHECK(!s.cell(s.cursorRow(), s.cursorCol() - 1).style.bold);
        s.feed("\x1b[1;1H" "c");                  // not inserting: overwrites
        CHECK(s.rowText(0) == "cb");
    }
    {
        // Autowrap: the last column waits (pending wrap) and the next
        // character goes on the next row; without it, it overwrites.
        TerminalScreen s(3, 3);
        s.feed("abc");
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 2);
        s.feed("d");
        CHECK(s.text() == "abc\nd");
        s.feed("\x1b[?7l\x1b[3;1Hxyzw");
        CHECK(s.rowText(2) == "xyw" && s.cursorRow() == 2);
        // DECALN fills with E and homes the cursor.
        s.feed("\x1b#8");
        CHECK(s.text() == "EEE\nEEE\nEEE" && s.cursorRow() == 0 && s.cursorCol() == 0);
    }

    GROUP("term:sgr-underline");
    {
        // The style stays 16 bytes: one per style run in the scrollback.
        CHECK(sizeof(TermStyle) == 16);
        TermStyle st;
        applySgr(st, "4");
        CHECK(st.underline == TermUnderlineSingle);
        applySgr(st, "4:3");
        CHECK(st.underline == TermUnderlineCurly && st.underline);
        applySgr(st, "4:2");
        CHECK(st.underline == TermUnderlineDouble);
        applySgr(st, "4:4");
        CHECK(st.underline == TermUnderlineDotted);
        applySgr(st, "4:5");
        CHECK(st.underline == TermUnderlineDashed);
        applySgr(st, "4:9");
        CHECK(st.underline == TermUnderlineSingle);
        applySgr(st, "4:0");
        CHECK(st.underline == TermUnderlineNone && !st.underline);
        applySgr(st, "21");
        CHECK(st.underline == TermUnderlineDouble);
        applySgr(st, "24");
        CHECK(!st.underline);
        applySgr(st, "8;53");
        CHECK(st.hidden && st.overline);
        applySgr(st, "28;55");
        CHECK(!st.hidden && !st.overline);
        applySgr(st, "8;53;4:3;9");
        applySgr(st, "0");
        CHECK(st == TermStyle());
        // Without a table, underline colors are ignored; the rest of the
        // sequence still applies.
        applySgr(st, "58;5;196;1");
        CHECK(st.underlineColor == 0 && st.bold);
    }
    {
        // Underline colors: indexed and RGB, in the colon and semicolon
        // forms; 59 goes back to the text's color; equal colors share an
        // id, and the color follows its cells into the scrollback.
        TerminalScreen s(10, 3);
        s.setScrollbackLimit(10);
        s.feed("\x1b[4:3;58:2::10:20:30ma\x1b[58;2;10;20;30mb\x1b[58:5:196mc\x1b[58;5;9md\x1b[59me");
        const TermStyle &a = s.cell(0, 0).style, &b = s.cell(0, 1).style;
        CHECK(a.underline == TermUnderlineCurly && a.underlineColor != 0);
        CHECK(a.underlineColor == b.underlineColor);
        TermColor c = s.underlineColor(a.underlineColor);
        CHECK(c.kind == TermColor::RGB && c.r == 10 && c.g == 20 && c.b == 30);
        CHECK(vtIndexed(s.underlineColor(s.cell(0, 2).style.underlineColor), 196));
        CHECK(vtIndexed(s.underlineColor(s.cell(0, 3).style.underlineColor), 9));
        CHECK(s.cell(0, 4).style.underlineColor == 0 && s.cell(0, 4).style.underline);
        CHECK(s.underlineColorCount() == 3);
        s.feed("\x1b[m\r\n\n\n");                   // scrolled into the scrollback
        TermCell up = s.lineCell(-1, 0);
        CHECK(up.ch == "a" && up.style.underline == TermUnderlineCurly);
        CHECK(s.underlineColor(up.style.underlineColor).r == 10);
        // Hidden and overlined text keeps its characters (copied as text).
        s.feed("\x1b[8;53mpw\x1b[m");
        CHECK(s.cell(s.cursorRow(), 0).style.hidden && s.cell(s.cursorRow(), 0).style.overline);
        CHECK(s.rowText(s.cursorRow()) == "pw");
    }
    {
        // A full table: ids no cell uses are given out again; with every
        // id in use a new color is the text's (0).
        TerminalScreen s(80, 60);
        auto color = [](int i) {
            return "\x1b[58;2;" + std::to_string(i / 65536) + ";" + std::to_string(i / 256 % 256) +
                   ";" + std::to_string(i % 256) + "m";
        };
        std::string all;
        for (int i = 0; i < (int)TermColorTable::kMax; i++) all += color(i) + "x";
        s.feed(all);
        CHECK(s.underlineColorCount() == TermColorTable::kMax);
        s.feed(color(99999) + "y");
        CHECK(s.cell(s.cursorRow(), s.cursorCol() - 1).style.underlineColor == 0);
        s.feed("\x1b[m\x1b[2J\x1b[H" + color(99999) + "z");   // the cells are gone
        const uint16_t id = s.cell(0, 0).style.underlineColor;
        CHECK(id != 0 && s.underlineColor(id).b == 99999 % 256);
        CHECK(s.underlineColorCount() == 1);
    }
    {
        // Round 6's terminal defect 3: with every id in use by 9,000 lines
        // of scrollback, each SGR whose text held "58" (38;5;58, a channel
        // of 158) scanned every cell and run again, about 0.1 ms each. Now
        // only a new underline color that finds the table full may scan,
        // and only once the output since the last scan is a quarter of
        // what it read.
        TerminalScreen s(80, 24);
        s.setScrollbackLimit(9000);
        auto color = [](int i) {
            return "\x1b[58;2;" + std::to_string(i / 65536) + ";" + std::to_string(i / 256 % 256) +
                   ";" + std::to_string(i % 256) + "m";
        };
        std::string fill;
        for (int i = 0; i < 9000; i++)
            fill += color(i % (int)TermColorTable::kMax) + "u\x1b[59m line " + std::to_string(i) + "\r\n";
        s.feed(fill);
        CHECK(s.underlineColorCount() == TermColorTable::kMax);
        const int scans0 = s.underlineColorScans();
        std::string other, fresh;
        for (int i = 0; i < 5000; i++)
            other += i % 2 ? "\x1b[38;5;58m\x1b[1;1Ha" : "\x1b[48;2;158;58;1m\x1b[1;1Hb";
        for (int i = 0; i < 5000; i++) fresh += "\x1b[1;1H" + color(100000 + i) + "c";
        double t0 = cpuMs();
        s.feed(other);
        const double otherMs = cpuMs() - t0;
        const int scans1 = s.underlineColorScans();
        t0 = cpuMs();
        s.feed(fresh);
        const double freshMs = cpuMs() - t0;
        const int scans2 = s.underlineColorScans();
        std::printf("    5,000 SGRs with 58 in their text against a full table: %.1f ms, %d scans; "
                    "5,000 new underline colors: %.1f ms, %d scans\n",
                    otherMs, scans1 - scans0, freshMs, scans2 - scans1);
        CHECK(scans1 == scans0);                         // no new underline color: no scan
        // 5,000 misses, at about 30 units of work each against about
        // 30,000 entries a scan reads: a scan every thousand or so (one
        // per SGR before).
        CHECK(scans2 - scans1 >= 1 && scans2 - scans1 <= 40);
        CHECK_TIME(otherMs, 400);
        CHECK_TIME(freshMs, 400);
        // A new color with every id in use is the text's; the scrollback
        // keeps its colors.
        CHECK(s.cell(0, 0).style.underlineColor == 0);
        CHECK(s.underlineColor(s.lineCell(-1, 0).style.underlineColor).kind == TermColor::RGB);
    }

    GROUP("term:kitty-keys");
    {
        // The flags: query, push, pop, set; only flag 1 is taken.
        TerminalScreen s(10, 4);
        auto flags = [&] {
            s.takeReplies();
            s.feed("\x1b[?u");
            return s.takeReplies();
        };
        CHECK(flags() == "\x1b[?0u" && s.keyboardFlags() == 0);
        s.feed("\x1b[>1u");
        CHECK(flags() == "\x1b[?1u" && s.keyboardFlags() == 1);
        s.feed("\x1b[>31u");                       // more than it takes
        CHECK(flags() == "\x1b[?1u");
        s.feed("\x1b[<u");
        CHECK(s.keyboardFlags() == 1);
        s.feed("\x1b[<u");
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[<5u");                        // past the bottom: all off
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[>1u\x1b[>0u\x1b[<2u");        // pop 2 at once
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[=1;1u");
        CHECK(s.keyboardFlags() == 1);
        s.feed("\x1b[=1;3u");
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[=1;2u");
        CHECK(s.keyboardFlags() == 1);
        s.feed("\x1b[=0u");                        // mode 1 by default
        CHECK(s.keyboardFlags() == 0);
        // The stack is bounded: the oldest entries go.
        for (int i = 0; i < 100; i++) s.feed("\x1b[>1u");
        for (int i = 0; i < (int)TerminalScreen::kKeyboardStackMax; i++) s.feed("\x1b[<u");
        CHECK(s.keyboardFlags() == 1);
        s.feed("\x1b[<u");
        CHECK(s.keyboardFlags() == 0);
        CHECK(s.text().empty());
    }
    {
        // A stack per screen; a fresh alternate screen starts with none;
        // RIS and resetKeyboardModes turn everything off.
        TerminalScreen s(10, 4);
        s.feed("\x1b[>1u\x1b[?1049h");
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[>1u");
        CHECK(s.keyboardFlags() == 1);
        s.feed("\x1b[<u\x1b[?1049l");
        CHECK(s.keyboardFlags() == 1);              // the main screen's
        s.feed("\x1b[?1049h");
        s.feed("\x1b[>1u\x1b[?1049l\x1b[?1049h");
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[?1049l");
        s.feed("\x1b" "c");
        CHECK(s.keyboardFlags() == 0);
        s.feed("\x1b[>1u");
        s.resetKeyboardModes();
        CHECK(s.keyboardFlags() == 0);
    }
    {
        // Encodings with flag 1 (disambiguate escape codes).
        using K = TermKey;
        const int D = TermKeyboardDisambiguate;
        auto key = [&](K k, int mods, bool appCursor = false) {
            return TerminalScreen::encodeKey(k, mods, appCursor, false, D);
        };
        CHECK(key(K::Escape, 0) == "\x1b[27u");
        CHECK(key(K::Escape, TermModShift) == "\x1b[27;2u");
        CHECK(key(K::Enter, 0) == "\r");
        CHECK(key(K::Enter, TermModShift) == "\x1b[13;2u");
        CHECK(key(K::Enter, TermModAlt) == "\x1b[13;3u");
        CHECK(key(K::Tab, 0) == "\t");
        CHECK(key(K::Tab, TermModShift) == "\x1b[9;2u");
        CHECK(key(K::Backspace, 0) == "\x7f");
        CHECK(key(K::Backspace, TermModCtrl) == "\x1b[127;5u");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, true, D) == "\x1b[57414u");
        CHECK(key(K::Up, 0, true) == "\x1b[A");   // DECCKM does not apply
        CHECK(key(K::Up, TermModCtrl) == "\x1b[1;5A");
        CHECK(key(K::Home, 0) == "\x1b[H" && key(K::End, TermModShift) == "\x1b[1;2F");
        CHECK(key(K::F1, 0) == "\x1b[P" && key(K::F2, TermModAlt) == "\x1b[1;3Q");
        CHECK(key(K::F3, 0) == "\x1b[13~" && key(K::F3, TermModShift) == "\x1b[13;2~");
        CHECK(key(K::F4, 0) == "\x1b[S");
        CHECK(key(K::F5, 0) == "\x1b[15~" && key(K::F12, TermModCtrl) == "\x1b[24;5~");
        CHECK(key(K::Delete, 0) == "\x1b[3~" && key(K::PageUp, TermModShift) == "\x1b[5;2~");
        auto ch = [&](uint32_t cp, int mods) { return TerminalScreen::encodeChar(cp, mods, D); };
        CHECK(ch('c', TermModCtrl) == "\x1b[99;5u");
        CHECK(ch('A', TermModCtrl | TermModShift) == "\x1b[97;6u");   // the unshifted key
        CHECK(ch('x', TermModAlt) == "\x1b[120;3u");
        CHECK(ch('[', TermModAlt) == "\x1b[91;3u");
        CHECK(ch(' ', TermModCtrl) == "\x1b[32;5u");
        CHECK(ch('a', 0) == "a" && ch('A', TermModShift) == "A");
        // Without the flag, the legacy bytes.
        CHECK(TerminalScreen::encodeKey(K::Escape, 0, false) == "\x1b");
        CHECK(TerminalScreen::encodeKey(K::F1, 0, false) == "\x1bOP");
        CHECK(TerminalScreen::encodeChar('c', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('x', TermModAlt) == "\x1bx");
    }

    GROUP("term:mode-2027");
    {
        // zsh's widths, one code point at a time (its Unicode 9 table).
        CHECK(termCodepointWidth('a') == 1);
        CHECK(termCodepointWidth(0x1F1FA) == 2);     // a regional indicator
        CHECK(termCodepointWidth(0x2764) == 1);      // heavy black heart
        CHECK(termCodepointWidth(0xFE0F) == 0);      // VS16
        CHECK(termCodepointWidth(0x093F) == 0);      // a Devanagari spacing vowel sign
        CHECK(termCodepointWidth(0x0903) == 0);
        CHECK(termCodepointWidth(0x0301) == 0);
        CHECK(termCodepointWidth(0x1F468) == 2);
        CHECK(termCodepointWidth(0x4E00) == 2 && termCodepointWidth(0xAC00) == 2);
        CHECK(termCodepointWidth(0xE000) == 1);      // private use
        CHECK(termCodepointWidth(0x00E9) == 1);
        // Where zsh has none (non-printing, unassigned in Unicode 9), the
        // Unicode table's.
        CHECK(termCodepointWidth(0x200D) == termCharWidth(0x200D));
        CHECK(termCodepointWidth(0x00AD) == 1);
        CHECK(termCodepointWidth(0x1F972) == termCharWidth(0x1F972));   // Unicode 13
    }
    {
        // Clusters by default; code points from zsh's prompt mark to its
        // command mark; clusters again after it, and on the alternate
        // screen.
        const std::string flag = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";       // US
        const std::string heart = "\xE2\x9D\xA4\xEF\xB8\x8F";               // ❤️
        const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                                   "\xE2\x80\x8D\xF0\x9F\x91\xA7";
        const std::string ki = "\xE0\xA4\x95\xE0\xA4\xBF";                  // कि
        CHECK(vtColumns("", flag) == 2 && vtColumns("", heart) == 2);
        CHECK(vtColumns("", family) == 2 && vtColumns("", ki) == 2);
        const std::string prompt = vtMark("A");
        CHECK(vtColumns(prompt, flag) == 4);
        CHECK(vtColumns(prompt, heart) == 1);
        CHECK(vtColumns(prompt, family) == 6);
        CHECK(vtColumns(prompt, ki) == 1);
        CHECK(vtColumns(prompt + vtMark("B"), flag) == 4);
        CHECK(vtColumns(prompt + vtMark("C"), flag) == 2);
        CHECK(vtColumns(prompt + vtMark("C") + vtMark("D;0") + vtMark("A"), flag) == 4);
        CHECK(vtColumns(prompt + "\x1b[?1049h", flag) == 2);
        // Someone else's mark changes nothing.
        CHECK(vtColumns("\x1b]133;A\x07", flag) == 2);
        // The cells: four of them, each a whole wide character; the heart
        // with its VS16 in one narrow cell.
        TerminalScreen s = vtScreen(prompt + flag + heart, 10, 2);
        CHECK(s.cell(0, 0).width == 2 && s.cell(0, 2).width == 2);
        CHECK(s.cell(0, 4).ch == heart && s.cell(0, 4).width == 1);
        CHECK(s.cursorCol() == 5);
    }
    {
        // The program chooses: DECRST 2027 code points everywhere, DECSET
        // clusters even at the prompt; DECRQM answers what is in effect;
        // RIS goes back to the default.
        const std::string flag = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
        CHECK(vtColumns("\x1b[?2027l", flag) == 4);
        CHECK(vtColumns(vtMark("A") + "\x1b[?2027h", flag) == 2);
        CHECK(vtColumns("\x1b[?2027l\x1b" "c", flag) == 2);
        TerminalScreen s = vtScreen("");
        auto ask = [&] {
            s.takeReplies();
            s.feed("\x1b[?2027$p");
            return s.takeReplies();
        };
        CHECK(ask() == "\x1b[?2027;1$y" && s.clusterWidths());
        s.feed(vtMark("A"));
        CHECK(ask() == "\x1b[?2027;2$y" && !s.clusterWidths());
        s.feed("\x1b[?2027h");
        CHECK(ask() == "\x1b[?2027;1$y");
        s.feed(vtMark("C") + "\x1b[?2027l");
        CHECK(ask() == "\x1b[?2027;2$y");
    }
}
