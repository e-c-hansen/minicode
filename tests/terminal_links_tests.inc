// terminal_links_tests.inc — the `hyperlinks:` groups of run_tests.cpp:
// OSC 8 hyperlinks in TerminalScreen (ids on cells, the table, its limits),
// what TermLinks makes of a hyperlink's address, and the 0.0.0.0 and [::]
// rewrite.

namespace {

std::string osc8(const std::string& params, const std::string& uri, const char* end = "\x1b\\") {
    return "\x1b]8;" + params + ";" + uri + end;
}

uint32_t linkAt(const TerminalScreen& s, int line, int col) { return s.lineCell(line, col).style.link; }

}  // namespace

void testTerminalHyperlinks() {
    GROUP("hyperlinks:osc8");
    {
        TerminalScreen s(40, 5);
        s.feed(osc8("", "http://localhost:5173/") + "open" + osc8("", "") + " plain");
        uint32_t id = linkAt(s, 0, 0);
        CHECK(id != 0);
        CHECK(s.linkURI(id) == "http://localhost:5173/");
        for (int c = 0; c < 4; c++) CHECK(linkAt(s, 0, c) == id);
        for (int c = 4; c < 10; c++) CHECK(linkAt(s, 0, c) == 0);
        CHECK(s.rowText(0) == "open plain");
        CHECK(s.linkURI(0).empty() && s.linkURI(999).empty());
    }
    {
        // BEL ends it as well as ST; colors change and reset inside it.
        TerminalScreen s(40, 5);
        s.feed(osc8("", "https://example.com/a", "\x07") + "a\x1b[1mb\x1b[0mc" + osc8("", "", "\x07") + "d");
        uint32_t id = linkAt(s, 0, 0);
        CHECK(id != 0 && linkAt(s, 0, 1) == id && linkAt(s, 0, 2) == id);
        CHECK(s.cell(0, 1).style.bold && !s.cell(0, 2).style.bold);
        CHECK(linkAt(s, 0, 3) == 0);
    }
    {
        // The same address shares an id; an id= parameter tells two apart,
        // and the same id= and address printed in pieces is one link.
        TerminalScreen s(60, 5);
        s.feed(osc8("", "http://localhost/x") + "a" + osc8("", "") + " " +
               osc8("", "http://localhost/x") + "b" + osc8("", "") + " " +
               osc8("id=1", "http://localhost/x") + "c" + osc8("", "") + " " +
               osc8("id=2", "http://localhost/x") + "d" + osc8("", "") + " " +
               osc8("foo=bar:id=1", "http://localhost/x") + "e" + osc8("", ""));
        CHECK(linkAt(s, 0, 0) == linkAt(s, 0, 2));
        CHECK(linkAt(s, 0, 4) != linkAt(s, 0, 0));
        CHECK(linkAt(s, 0, 6) != linkAt(s, 0, 4));
        CHECK(linkAt(s, 0, 8) == linkAt(s, 0, 4));
        CHECK(s.linkCount() == 3);
    }
    {
        // A wide character carries the link on both halves.
        TerminalScreen s(20, 3);
        s.feed(osc8("", "file:///tmp/x") + "\xe4\xb8\xad" + osc8("", ""));
        CHECK(linkAt(s, 0, 0) != 0 && linkAt(s, 0, 1) == linkAt(s, 0, 0));
        CHECK(s.cell(0, 1).width == 0);
    }
    {
        // A new link replaces an open one; a full reset and erasing end it.
        TerminalScreen s(40, 5);
        s.feed(osc8("", "http://localhost/1") + "a" + osc8("", "http://localhost/2") + "b");
        CHECK(s.linkURI(linkAt(s, 0, 0)) == "http://localhost/1");
        CHECK(s.linkURI(linkAt(s, 0, 1)) == "http://localhost/2");
        s.feed("\x1b[2J\x1b[H");
        CHECK(linkAt(s, 0, 0) == 0 && linkAt(s, 0, 1) == 0);
        s.feed("c");
        CHECK(s.linkURI(linkAt(s, 0, 0)) == "http://localhost/2");   // still open
        s.feed("\x1b" "c" "d");
        CHECK(s.rowText(0) == "d" && linkAt(s, 0, 0) == 0);
    }

    GROUP("hyperlinks:refused");
    {
        // Only http, https and file; nothing over the limit, nothing with a
        // space or a control character; such an address prints plain text
        // and closes a link that was open.
        const std::string bad[] = {
            "javascript:alert(1)", "ftp://example.com/", "data:text/html,x", "http://",
            "mailto:a@b.c", "http://exa mple.com/", std::string("http://a\x01") + "b/",
            "http://localhost/" + std::string(TerminalScreen::kMaxLinkBytes, 'a'),
        };
        for (const std::string& uri : bad) {
            TerminalScreen s(40, 5);
            s.feed(osc8("", "http://localhost/ok") + "a" + osc8("", uri) + "b" + osc8("", "") + "c");
            CHECK(linkAt(s, 0, 0) != 0);
            CHECK(linkAt(s, 0, 1) == 0);
            CHECK(linkAt(s, 0, 2) == 0);
            CHECK(s.rowText(0) == "abc");
        }
        TerminalScreen s(40, 5);
        std::string longest = "https://h/" + std::string(TerminalScreen::kMaxLinkBytes - 10, 'p');
        s.feed(osc8("", longest) + "x" + osc8("", ""));
        CHECK(s.linkURI(linkAt(s, 0, 0)) == longest);
        // Upper-case schemes count; "8;" with no second ';' closes.
        s.feed(osc8("", "HTTPS://localhost/") + "y\x1b]8;\x1b\\z");
        CHECK(linkAt(s, 0, 1) != 0 && linkAt(s, 0, 2) == 0);
    }

    GROUP("hyperlinks:scrollback-reflow");
    {
        TerminalScreen s(20, 3);
        s.setScrollbackLimit(100);
        s.feed(osc8("", "http://localhost:8000/") + "served here" + osc8("", "") + "\r\n");
        for (int i = 0; i < 5; i++) s.feed("line\r\n");
        int line = 1000;
        for (int l = -s.historySize(); l < s.rows(); l++)
            if (s.lineText(l) == "served here") line = l;
        CHECK(line < 0);
        uint32_t id = line < 0 ? linkAt(s, line, 0) : 0;
        CHECK(id != 0 && linkAt(s, line, 10) == id && linkAt(s, line, 11) == 0);
        CHECK(s.linkURI(id) == "http://localhost:8000/");
        // A copy keeps the table.
        TerminalScreen copy = s;
        CHECK(copy.linkURI(id) == "http://localhost:8000/");
    }
    {
        // A wrapped link reflows with its cells.
        TerminalScreen s(10, 4);
        s.setScrollbackLimit(100);
        s.feed("ab " + osc8("", "http://localhost/long") + "0123456789xyz" + osc8("", "") + " end");
        s.resize(30, 4);
        int row = 1000;
        for (int l = -s.historySize(); l < s.rows(); l++)
            if (s.lineText(l).find("0123456789xyz") != std::string::npos) row = l;
        CHECK(row != 1000);
        if (row == 1000) return;
        size_t at = s.lineText(row).find('0');
        uint32_t id = linkAt(s, row, (int)at);
        CHECK(id != 0 && linkAt(s, row, (int)at + 12) == id);
        CHECK(linkAt(s, row, (int)at - 1) == 0 && linkAt(s, row, (int)at + 13) == 0);
        s.resize(7, 4);
        int found = 0;
        for (int l = -s.historySize(); l < s.rows(); l++)
            for (int c = 0; c < s.lineWidth(l); c++)
                if (linkAt(s, l, c) == id) found++;
        CHECK(found == 13);
    }

    GROUP("hyperlinks:table-limit");
    {
        // ls --hyperlink in a big folder, many times: ids no cell uses any
        // more are given out again; while every id is in use a new link
        // prints as plain text.
        TerminalScreen s(30, 5);
        s.setScrollbackLimit(100000);
        for (size_t i = 0; i < TerminalScreen::kMaxLinks; i++)
            s.feed(osc8("", "file:///f/" + std::to_string(i)) + "f" + osc8("", "") + "\r\n");
        CHECK(s.linkCount() == TerminalScreen::kMaxLinks);
        s.feed(osc8("", "file:///one-too-many") + "x" + osc8("", ""));
        CHECK(linkAt(s, s.cursorRow(), 0) == 0);
        CHECK(s.linkCount() == TerminalScreen::kMaxLinks);
        s.clearAll();
        s.feed(osc8("", "file:///after") + "y" + osc8("", ""));
        uint32_t id = linkAt(s, 0, 0);
        CHECK(id != 0 && s.linkURI(id) == "file:///after");
        CHECK(s.linkCount() == 1);
        // A link still on screen keeps its id through a collection.
        for (size_t i = 0; i < TerminalScreen::kMaxLinks + 10; i++)
            s.feed(osc8("", "file:///g/" + std::to_string(i)) + "g" + osc8("", "") + "\r\n");
        CHECK(s.linkURI(linkAt(s, -s.historySize(), 0)) == "file:///after");
    }

    GROUP("hyperlinks:targets");
    {
        using TermLinks::Link;
        Link l;
        CHECK(TermLinks::hyperlink("http://localhost:5173/", "mac", l) && l.kind == Link::Url &&
              l.target == "http://localhost:5173/");
        CHECK(TermLinks::hyperlink("HTTPS://example.com/a?b#c", "mac", l) && l.kind == Link::Url);
        CHECK(TermLinks::hyperlink("file:///tmp/a%20b.txt", "mac", l) && l.kind == Link::File &&
              l.target == "/tmp/a b.txt");
        CHECK(TermLinks::hyperlink("file://localhost/etc/hosts", "mac", l) && l.target == "/etc/hosts");
        CHECK(TermLinks::hyperlink("file://Mac.local/x/y?q=1#f", "mac", l) && l.target == "/x/y");
        CHECK(TermLinks::hyperlink("file://mac/x", "Mac.local", l) && l.target == "/x");
        CHECK(!TermLinks::hyperlink("file://other-host/x", "mac", l));
        CHECK(!TermLinks::hyperlink("file://other-host/x", "", l));
        CHECK(!TermLinks::hyperlink("file:///a%00b", "mac", l));
        CHECK(!TermLinks::hyperlink("file:///a%zz", "mac", l));
        CHECK(!TermLinks::hyperlink("file:", "mac", l));
        CHECK(!TermLinks::hyperlink("ftp://x/", "mac", l));
        CHECK(!TermLinks::hyperlink("http://", "mac", l));
        CHECK(!TermLinks::hyperlink("javascript:alert(1)", "mac", l));
    }

    GROUP("hyperlinks:browser-address");
    {
        using TermLinks::browserAddress;
        CHECK(browserAddress("http://0.0.0.0:8000/") == "http://localhost:8000/");
        CHECK(browserAddress("http://[::]:8000/") == "http://localhost:8000/");
        CHECK(browserAddress("https://0.0.0.0") == "https://localhost");
        CHECK(browserAddress("http://0.0.0.0/x?y") == "http://localhost/x?y");
        CHECK(browserAddress("http://0.0.0.0.example.com/") == "http://0.0.0.0.example.com/");
        CHECK(browserAddress("http://127.0.0.1:3000/") == "http://127.0.0.1:3000/");
        CHECK(browserAddress("http://[::1]:3000/") == "http://[::1]:3000/");
        CHECK(browserAddress("file:///0.0.0.0") == "file:///0.0.0.0");
        CHECK(browserAddress("localhost:8000") == "localhost:8000");
    }

}
