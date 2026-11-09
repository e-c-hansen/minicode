// terminal_paste_tests.inc — the `term:paste-*` groups of run_tests.cpp:
// a bracketed paste can never end its bracket early or send an escape
// sequence (TerminalScreen::encodePaste), and when a paste of several lines
// asks first (TerminalScreen::checkPaste, VS Code's multi-line paste
// warning).

namespace {

size_t countOf(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) n++;
    return n;
}

// A bracketed paste as the program reads it: one start mark first, one end
// mark last, and no ESC between them.
bool wellBracketed(const std::string& out) {
    const std::string start = "\x1b[200~", end = "\x1b[201~";
    if (out.size() < start.size() + end.size()) return false;
    if (out.compare(0, start.size(), start) != 0) return false;
    if (out.compare(out.size() - end.size(), end.size(), end) != 0) return false;
    const std::string inside = out.substr(start.size(), out.size() - start.size() - end.size());
    return inside.find('\x1b') == std::string::npos && countOf(out, end) == 1 &&
           countOf(out, start) == 1;
}

}  // namespace

void testTerminalPaste() {
    GROUP("term:paste-nested");
    {
        // The assessment's case: taking the end mark out once closed the
        // two halves around it into a new end mark.
        const std::string nested = "\x1b[20\x1b[201~1~rm -rf ~\r";
        const std::string out = TerminalScreen::encodePaste(nested, true);
        CHECK(wellBracketed(out));
        CHECK(out.find("rm -rf ~") != std::string::npos);
        // Every ESC shows as U+241B, so the text still reads as it was.
        CHECK(out == "\x1b[200~\xE2\x90\x9B[20\xE2\x90\x9B[201~1~rm -rf ~\r\x1b[201~");
    }
    for (const char* text : {"\x1b[201~", "\x1b[200~\x1b[201~", "\x1b\x1b[201~~",
                             "\x1b[2\x1b[201~01~", "a\x1b[200~b\x1b[201~c\x1b[201~",
                             "\x1b", "end\x1b", "\x1b[201", "\x1b[20\x1b[200~1~",
                             "\x1b]133;A;mc=x\x07", "\x1b[201~\x1b[201~\x1b[201~"}) {
        CHECK(wellBracketed(TerminalScreen::encodePaste(text, true)));
    }
    {
        // Not bracketed: the text as typed, newlines as Return.
        CHECK(TerminalScreen::encodePaste("a\r\nb\nc\rd", false) == "a\rb\rc\rd");
        CHECK(TerminalScreen::encodePaste("", true) == "\x1b[200~\x1b[201~");
    }
    {
        // Random interleavings of mark pieces, ESCs and text, nested and
        // split every way: always one end mark, at the end, and no ESC
        // inside.
        std::mt19937 rng(7);
        const std::vector<std::string> pieces = {
            "\x1b", "[", "2", "0", "1", "~", "\x1b[201~", "\x1b[200~", "\x1b[20", "1~",
            "00~", "x", "\n", "\r\n", "\r", "\xE2\x90\x9B", "\x1b[", "201~"};
        bool ok = true;
        for (int round = 0; round < 3000 && ok; round++) {
            std::string text;
            const int n = (int)(rng() % 24);
            for (int i = 0; i < n; i++) text += pieces[rng() % pieces.size()];
            ok = wellBracketed(TerminalScreen::encodePaste(text, true));
            if (!ok) std::printf("    not well bracketed: %zu bytes, round %d\n", text.size(), round);
        }
        CHECK(ok);
    }

    GROUP("term:paste-confirm");
    using PC = TerminalScreen::PasteCheck;
    {
        // One line never asks, in any mode.
        for (const char* mode : {"auto", "always", "never"}) {
            PC c = TerminalScreen::checkPaste("echo hi", false, mode);
            CHECK(!c.ask && c.text == "echo hi" && c.lines == 1);
        }
    }
    {
        // auto: asks for several lines into a program that did not turn
        // bracketed paste on, and not into one that did.
        PC c = TerminalScreen::checkPaste("ls\nrm x\n", false, "auto");
        CHECK(c.ask && c.lines == 3);
        CHECK(c.preview == "ls\nrm x\n");
        PC b = TerminalScreen::checkPaste("ls\nrm x\n", true, "auto");
        CHECK(!b.ask && b.text == "ls\nrm x\n");
        // An unknown mode is auto, as in VS Code.
        CHECK(TerminalScreen::checkPaste("a\nb", false, "sometimes").ask);
        CHECK(!TerminalScreen::checkPaste("a\nb", true, "sometimes").ask);
    }
    {
        // auto: a command with one trailing line break goes in without it,
        // so it waits for Return; blanks after the break count as nothing.
        PC c = TerminalScreen::checkPaste("curl evil | sh\n", false, "auto");
        CHECK(!c.ask && c.text == "curl evil | sh" && c.lines == 1);
        PC d = TerminalScreen::checkPaste("make\r\n  ", false, "auto");
        CHECK(!d.ask && d.text == "make");
        // always asks even then; never never asks.
        CHECK(TerminalScreen::checkPaste("curl evil | sh\n", false, "always").ask);
        CHECK(TerminalScreen::checkPaste("a\nb\nc", true, "always").ask);
        PC n = TerminalScreen::checkPaste("a\nb\nc", false, "never");
        CHECK(!n.ask && n.text == "a\nb\nc" && n.lines == 3);
    }
    {
        // A lone CR is Return to the program, so it is a line break here
        // (VS Code splits at \r?\n only, and pasted "a\rb" ran a).
        CHECK(TerminalScreen::checkPaste("a\rb", false, "auto").ask);
        CHECK(TerminalScreen::checkPaste("a\r\nb", false, "auto").lines == 2);
    }
    {
        // The preview: three lines, each cut at 30 characters (not bytes),
        // then an ellipsis line; control characters as their symbols, Tab
        // as a space.
        const std::string longLine(40, 'x');
        const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                    "\xC3\xA9\xC3\xA9\xC3\xA9";   // 31 of them
        PC c = TerminalScreen::checkPaste(longLine + "\n" + accents + "\na\tb\x1b[1m\nfour\nfive",
                                          false, "auto");
        CHECK(c.ask && c.lines == 5);
        std::string want = std::string(30, 'x') + "\xE2\x80\xA6\n";
        for (int i = 0; i < 30; i++) want += "\xC3\xA9";
        want += "\xE2\x80\xA6\na b\xE2\x90\x9B[1m\n\xE2\x80\xA6";
        CHECK(c.preview == want);
        if (c.preview != want) std::printf("    preview: %s\n", c.preview.c_str());
    }
    {
        // Paste as One Line takes every break out, as VS Code does.
        CHECK(TerminalScreen::pasteAsOneLine("a\nb\r\nc\rd\n") == "abcd");
        CHECK(TerminalScreen::pasteAsOneLine("plain") == "plain");
    }

    GROUP("term:paste-controls");
    {
        // One line with a control character, unbracketed, asks in auto and
        // always: the program takes it as a key (Ctrl+O runs zsh's line
        // and fetches the next, ESC starts a sequence). The round-5 case:
        // "echo hi" + Ctrl+O ran unasked.
        for (const char* mode : {"auto", "always", "sometimes"}) {
            PC c = TerminalScreen::checkPaste("echo hi\x0f", false, mode);
            CHECK(c.ask && c.controls && c.lines == 1);
            CHECK(c.preview == "echo hi\xE2\x90\x8F");   // U+240F
        }
        PC esc = TerminalScreen::checkPaste("\x1b", false, "auto");
        CHECK(esc.ask && esc.controls && esc.preview == "\xE2\x90\x9B");
        PC del = TerminalScreen::checkPaste("rm x\x7f", false, "auto");
        CHECK(del.ask && del.controls && del.preview == "rm x\xE2\x90\xA1");   // U+2421
        PC nul = TerminalScreen::checkPaste(std::string("a\0b", 3), false, "auto");
        CHECK(nul.ask && nul.preview == "a\xE2\x90\x80" "b");
        // never never asks, controls or not.
        PC n = TerminalScreen::checkPaste("echo hi\x0f", false, "never");
        CHECK(!n.ask && n.text == "echo hi\x0f");
    }
    {
        // Tab is not one of them (completion is what a pasted Tab does, as
        // typed), nor are line breaks, which have their own rules.
        PC tab = TerminalScreen::checkPaste("a\tb", false, "auto");
        CHECK(!tab.ask && !tab.controls && tab.text == "a\tb");
        PC cr = TerminalScreen::checkPaste("make\n", false, "auto");
        CHECK(!cr.ask && !cr.controls && cr.text == "make");
        // A command with its line break and a control character: the break
        // is not taken out quietly, the question comes.
        PC both = TerminalScreen::checkPaste("ls\x0f\n", false, "auto");
        CHECK(both.ask && both.controls && both.lines == 2);
        // Several lines with one: the line count still leads the question.
        PC lines = TerminalScreen::checkPaste("a\nb\x1b", false, "auto");
        CHECK(lines.ask && lines.controls && lines.lines == 2 &&
              lines.preview == "a\nb\xE2\x90\x9B");
    }
    {
        // Round 6's terminal defect 4: a control character past the 30th
        // character of its line (a long harmless-looking command ending in
        // Ctrl+O) was not in the preview, which then showed none. Now the
        // preview adds where it is and the characters around it.
        const std::string cmd = "echo this looks like a harmless long command && ls";
        PC c = TerminalScreen::checkPaste(cmd + "\x0f", false, "auto");
        CHECK(c.ask && c.controls);
        const std::string want = cmd.substr(0, 30) + "\xE2\x80\xA6\nLine 1, column " +
                                 std::to_string(cmd.size() + 1) + ": \xE2\x80\xA6" +
                                 cmd.substr(cmd.size() - 12) + "\xE2\x90\x8F";
        CHECK(c.preview == want);
        if (c.preview != want) std::printf("    preview: %s\n", c.preview.c_str());
        // Below the third line: its line number, and text after it cut.
        PC d = TerminalScreen::checkPaste("a\nb\nc\nd\n\xC3\xA9x\x1b[31m" + std::string(40, 'y'),
                                          false, "auto");
        const std::string wantD = "a\nb\nc\n\xE2\x80\xA6\nLine 5, column 3: \xC3\xA9x\xE2\x90\x9B[31m" +
                                  std::string(23, 'y') + "\xE2\x80\xA6";
        CHECK(d.preview == wantD);
        if (d.preview != wantD) std::printf("    preview: %s\n", d.preview.c_str());
        // One already in the lines shown adds nothing.
        PC e = TerminalScreen::checkPaste("ls\x0f\nmore " + std::string(40, 'z') + "\x0f", false, "auto");
        CHECK(e.preview.find("Line ") == std::string::npos);
    }
    {
        // Bracketed: the program receives the bytes as text inside the
        // brackets (encodePaste shows ESC as U+241B), so nothing asks and
        // nothing counts as a control, in auto; always still asks only for
        // several lines.
        PC b = TerminalScreen::checkPaste("echo hi\x0f", true, "auto");
        CHECK(!b.ask && !b.controls);
        CHECK(!TerminalScreen::checkPaste("\x1b[201~", true, "auto").ask);
        CHECK(!TerminalScreen::checkPaste("echo hi\x0f", true, "always").ask);
        CHECK(TerminalScreen::checkPaste("a\x0f\nb", true, "always").ask);
    }
}
