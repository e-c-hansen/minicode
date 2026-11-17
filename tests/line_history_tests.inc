// line_history_tests.inc — the `linehistory:*` groups of run_tests.cpp
// (LineHistory.h: blame that knows Claude Code's sessions), included there
// after agent_edits_tests.inc, whose transcript helpers it shares.
#include "LineHistory.h"

static void testLineHistory() {
    using namespace LineHistory;
    using AgentEdits::Change;
    using AgentEdits::Tool;
    using namespace agentidx;

    auto edit = [](const std::string &o, const std::string &n, double t = 1) {
        Change c;
        c.tool = Tool::Edit;
        c.oldText = o;
        c.newText = n;
        c.answered = true;
        c.time = t;
        return c;
    };
    auto made = [](const std::string &content, double t = 1) {
        Change c;
        c.tool = Tool::Write;
        c.newText = content;
        c.created = true;
        c.answered = true;
        c.time = t;
        return c;
    };

    GROUP("linehistory:lines");
    {
        CHECK(lineCount("") == 0 && lineCount("a") == 1 && lineCount("a\n") == 1);
        CHECK(lineCount("a\nb") == 2 && lineCount("a\n\n") == 2 && lineCount("\n") == 1);
        std::vector<size_t> s = lineStarts("ab\ncd\n\nx");
        CHECK((s == std::vector<size_t>{0, 3, 6, 7}));
    }

    const std::string h1(40, 'a'), h2 = std::string(39, 'b') + "c", z(40, '0');
    GROUP("linehistory:porcelain");
    {
        const std::string out =
            h1 + " 1 1 2\nauthor Ann Lee\nauthor-mail <ann@example.com>\nauthor-time 1700000000\n"
            "author-tz +0000\ncommitter Ann Lee\ncommitter-mail <ann@example.com>\n"
            "committer-time 1700000000\ncommitter-tz +0000\nsummary First version\nboundary\n"
            "filename hello.py\n\tdef hello():\n" +
            h1 + " 2 2\n\t    print('hi')\n" +
            z + " 3 3 1\nauthor External file (--contents)\nauthor-mail <external.file>\n"
            "author-time 1700000500\nauthor-tz +0000\ncommitter External file (--contents)\n"
            "committer-mail <external.file>\ncommitter-time 1700000500\ncommitter-tz +0000\n"
            "summary Version of hello.py from hello.py\nprevious " + h1 + " hello.py\n"
            "filename hello.py\n\t    print('more')\n" +
            h2 + " 3 4 1\nauthor Bo\nauthor-mail <bo@example.com>\nauthor-time 1700000300\n"
            "author-tz +0100\ncommitter Bo\ncommitter-mail <bo@example.com>\n"
            "committer-time 1700000300\ncommitter-tz +0100\nsummary Return at the end\n"
            "previous " + h1 + " hello.py\nfilename hello.py\n\treturn\n" +
            h1 + " 3 5\n\t\t# a tab after the marker is the line's own\n";
        Blame b;
        CHECK(parsePorcelain(out, &b));
        CHECK(b.commits.size() == 3 && b.lineCommit.size() == 5);
        if (b.commits.size() == 3 && b.lineCommit.size() == 5) {
            CHECK((b.lineCommit == std::vector<int>{0, 0, 1, 2, 0}));
            CHECK(b.commits[0].author == "Ann Lee" && b.commits[0].time == 1700000000 &&
                  b.commits[0].summary == "First version" && b.commits[0].boundary &&
                  b.commits[0].authorMail == "<ann@example.com>" && !b.commits[0].uncommitted());
            CHECK(b.commits[1].uncommitted() && b.commits[2].author == "Bo" && !b.commits[2].boundary);
        }
        Blame empty;
        CHECK(parsePorcelain("", &empty) && empty.lineCommit.empty());
        Blame bad;
        CHECK(!parsePorcelain("fatal: no such path 'x' in HEAD\n", &bad));
        CHECK(!parsePorcelain(h1 + " 1 1 1\nauthor A\n", &bad));                  // no text line
        CHECK(!parsePorcelain(h1 + " 1 2 1\nauthor A\n\tline two only\n", &bad));   // line 1 missing
        CHECK(!parsePorcelain(h1 + " x y\n\tx\n", &bad));
    }

    GROUP("linehistory:written");
    {
        // A file the session made, then edited: every line is the session's,
        // each to the change that last wrote it.
        const std::string first = "def hello():\n    print('hi')\n\n\ndef main():\n    hello()\n";
        const std::string now =
            "def hello():\n    print('hello, world')\n\n\ndef main():\n    hello()\n    hello()\n";
        std::vector<int> w = linesWritten(
            {made(first), edit("print('hi')", "print('hello, world')"),
             edit("    hello()\n", "    hello()\n    hello()\n")},
            now);
        CHECK((w == std::vector<int>{0, 1, 0, 0, 0, 0, 2}) ||
              (w == std::vector<int>{0, 1, 0, 0, 0, 2, 0}));
        // Whichever copy of "    hello()" the diff calls new, exactly one is
        // edit 2's.
        CHECK(std::count(w.begin(), w.end(), 2) == 1 && w[1] == 1);

        // The lines an Edit quotes around its change are not its.
        const std::string file = "import os\ndef f():\n    return 2\n\nx = 1\n";
        w = linesWritten({edit("def f():\n    return 1\n", "def f():\n    return 2\n")}, file);
        CHECK((w == std::vector<int>{-1, -1, 0, -1, -1}));

        // A change in the middle of a line makes the line its.
        w = linesWritten({edit("x = 1", "x = 2")}, "a = 0; x = 2\nb\n");
        CHECK((w == std::vector<int>{0, -1}));

        // Lines it added.
        w = linesWritten({edit("a\n", "a\nb\nc\n")}, "top\na\nb\nc\nend\n");
        CHECK((w == std::vector<int>{-1, -1, 0, 0, -1}));

        // Changed by hand since: gone, so not the session's.
        w = linesWritten({edit("x = 1", "x = 2")}, "x = 3\n");
        CHECK((w == std::vector<int>{-1}));

        // Its text found twice: which one is its cannot be told.
        w = linesWritten({edit("x = 1", "x = 2")}, "x = 2\nx = 2\n");
        CHECK((w == std::vector<int>{-1, -1}));

        // A later edit since overwritten by hand ends the walk; the earlier
        // one, still standing on its own, is found from the file now.
        w = linesWritten({edit("a = 1", "a = 2"), edit("b = 1", "b = 3")}, "a = 2\nb = 4\n");
        CHECK((w == std::vector<int>{0, -1}));

        // A rewrite of a file the session did not make holds no old text.
        Change rewrite = made("one\ntwo\n");
        rewrite.created = false;
        w = linesWritten({rewrite}, "one\ntwo\n");
        CHECK((w == std::vector<int>{-1, -1}));
        // ... but an edit after it still counts.
        w = linesWritten({rewrite, edit("two", "deux")}, "one\ndeux\n");
        CHECK((w == std::vector<int>{-1, 1}));

        // A made file later edited by hand: the lines still as it wrote them
        // stay its, through the edit's undo.
        w = linesWritten({made("a\nb\nc\n"), edit("b\n", "B\n")}, "a\nB\nc\nmine\n");
        // B is the edit's; a and c cannot be told from text typed since,
        // since the file is no longer as made.
        CHECK((w == std::vector<int>{-1, 1, -1, -1}));
        CHECK(linesWritten({made("a\nb\nc\n"), edit("b\n", "B\n")}, "a\nB\nc\n") ==
              (std::vector<int>{0, 1, 0}));

        // Text put back that would join the next line takes it into the
        // region: "xb" became "y", "z" and "b", all three the edit's.
        w = linesWritten({edit("x", "y\nz\n")}, "a\ny\nz\nb\n");
        CHECK((w == std::vector<int>{-1, 0, 0, 0}));

        // Refused calls do nothing; an edit that only removed text cannot be
        // found, and the walk goes on past it.
        Change refused = edit("q", "never");
        refused.refused = true;
        w = linesWritten({edit("k = 0", "k = 1"), edit("junk\n", ""), refused}, "k = 1\nnever\n");
        CHECK((w == std::vector<int>{0, -1}));

        // Two edits on neighbouring lines, both kept, in order.
        w = linesWritten({edit("a = 1\n", "a = 10\n"), edit("b = 1\n", "b = 20\n")},
                         "a = 10\nb = 20\nc = 1\n");
        CHECK((w == std::vector<int>{0, 1, -1}));
        // A later edit rewriting an earlier one's line takes it.
        w = linesWritten({edit("a = 1\n", "a = 10\n"), edit("a = 10\n", "a = 100\n")}, "a = 100\n");
        CHECK((w == std::vector<int>{1}));

        // UTF-8 text and a last line with no newline.
        w = linesWritten({edit("caf\xc3\xa9", "caf\xc3\xa9 \xe2\x9c\x93")}, "x\ncaf\xc3\xa9 \xe2\x9c\x93");
        CHECK((w == std::vector<int>{-1, 0}));
        CHECK(linesWritten({}, "a\n") == std::vector<int>{-1});
        CHECK(linesWritten({edit("a", "b")}, "").empty());
    }

    GROUP("linehistory:combine");
    {
        Blame b;
        b.commits.resize(2);
        b.commits[0].hash = h1;
        b.commits[1].hash = z;
        b.lineCommit = {0, 1, 1, 1, 0};
        std::vector<Change> s1 = {edit("p", "q", 100), edit("r", "s", 300)};
        std::vector<Change> s2 = {edit("t", "u", 200)};
        SessionLines a{&s1, {0, 0, 1, -1, -1}};   // claims line 0, which is committed
        SessionLines c{&s2, {-1, 0, 0, -1, -1}};
        std::vector<Line> l = combine(5, &b, {a, c});
        CHECK(l.size() == 5);
        if (l.size() == 5) {
            CHECK(l[0].kind == Kind::Commit && l[0].commit == 0);
            CHECK(l[1].kind == Kind::Session && l[1].session == 1 && l[1].change == 0);
            // Line 2: session 0's change at 300 is later than session 1's at 200.
            CHECK(l[2].kind == Kind::Session && l[2].session == 0 && l[2].change == 1);
            CHECK(l[3].kind == Kind::NotCommitted && l[4].kind == Kind::Commit);
            CHECK(l[0] != l[1] && l[0] == l[4]);
        }
        // No repository: nothing committed.
        std::vector<Line> none = combine(2, nullptr, {SessionLines{&s2, {0, -1}}});
        CHECK(none.size() == 2 && none[0].kind == Kind::Session && none[1].kind == Kind::NotCommitted);
        // A blame for another text says nothing.
        std::vector<Line> other = combine(3, &b, {});
        CHECK(other.size() == 3 && other[0].kind == Kind::Unknown && other[2].kind == Kind::Unknown);
    }

    GROUP("linehistory:words");
    {
        Commit cm;
        cm.hash = h1;
        cm.author = "Ann Lee";
        cm.time = 1000000;
        cm.summary = "Fix the parser";
        const long long now = 1000000 + 3 * 86400;
        CHECK(commitLabel(cm, now) == "aaaaaaa Ann Lee, 3 days ago \xC2\xB7 Fix the parser");
        auto date = [](long long) { return std::string("Sep 23, 2026 at 14:02"); };
        CHECK(commitDetail(cm, now, date) ==
              "aaaaaaa \xC2\xB7 Ann Lee \xC2\xB7 3 days ago (Sep 23, 2026 at 14:02)\nFix the parser\n"
              "Click to open the commit.");
        Change c = edit("a", "b", (double)(now - 7200));
        c.prompt = "<command-name>/x</command-name>\n\n  Add a   perimeter method\nwith tests";
        SessionInfo s{"Circle perimeter", "/t.jsonl"};
        CHECK(sessionLabel(s, c, now) == "Claude Code: Circle perimeter, 2 hours ago");
        CHECK(sessionDetail(s, c, now, nullptr) ==
              "Written by a Claude Code session, not committed\nCircle perimeter\n"
              "Prompt: /x\nEdited 2 hours ago\nClick to see what the session changed in this file.");
        CHECK(promptLine("\n\n  Add a   perimeter method\nwith tests") == "Add a perimeter method");
        CHECK(promptLine("") == "");
        CHECK(sessionLabel(SessionInfo{}, c, now) == "Claude Code: a Claude Code session, 2 hours ago");
        CHECK(notCommittedLabel() == "Not committed");
        CHECK(notCommittedDetail().find("Not committed") == 0);
    }

    GROUP("linehistory:prompt");
    {
        // collect keeps the prompt each call answered.
        fsys::path root = scratch("linehistory");
        const std::string sid = "s-lh", cwd = root.string();
        const std::string file = (root / "a.py").string();
        put(root / "a.py", "x = 2\ny = 3\n");
        const std::string t = (root / "t.jsonl").string();
        auto editUse = [&](const std::string &ts, const std::string &id, const std::string &o,
                           const std::string &n) {
            return use(sid, cwd, ts, id, "Edit",
                       "{\"file_path\":" + q(file) + ",\"old_string\":" + q(o) + ",\"new_string\":" +
                           q(n) + ",\"replace_all\":false}");
        };
        put(t, prompt(sid, cwd, "2026-11-05T10:00:00Z", "Make x two\nand nothing else") + "\n" +
                   editUse("2026-11-05T10:00:01Z", "e1", "x = 1", "x = 2") + "\n" +
                   result(sid, cwd, "2026-11-05T10:00:02Z", "e1", "The file has been updated successfully.") +
                   "\n" + prompt(sid, cwd, "2026-11-05T10:01:00Z", "Now y") + "\n" +
                   editUse("2026-11-05T10:01:01Z", "e2", "y = 1", "y = 3") + "\n" +
                   result(sid, cwd, "2026-11-05T10:01:02Z", "e2", "The file has been updated successfully.") +
                   "\n");
        AgentEdits::FileHistory h = AgentEdits::collect(AgentTranscript::posixFs(), t, file);
        CHECK(h.changes.size() == 2);
        if (h.changes.size() == 2) {
            CHECK(h.changes[0].prompt == "Make x two\nand nothing else");
            CHECK(promptLine(h.changes[0].prompt) == "Make x two");
            CHECK(h.changes[1].prompt == "Now y");
            std::vector<int> w = linesWritten(h.changes, "x = 2\ny = 3\n");
            CHECK((w == std::vector<int>{0, 1}));
        }
        fsys::remove_all(root);
    }

    GROUP("linehistory:bounded");
    {
        // A large file and a session of many edits: one pass stays quick.
        std::string text;
        for (int i = 0; i < 100000; i++) text += "line " + std::to_string(i) + "\n";
        std::vector<Change> many;
        for (int i = 0; i < 200; i++) {
            const std::string was = "line " + std::to_string(i * 400) + "\n";
            const std::string is = "LINE " + std::to_string(i * 400) + " changed\n";
            many.push_back(edit(was, is));
            text.replace(text.find(was), was.size(), is);
        }
        const double t0 = cpuMs();
        std::vector<int> w = linesWritten(many, text);
        CHECK_TIME(cpuMs() - t0, 2000);
        CHECK(w.size() == 100000 && w[0] == 0 && w[400] == 1 && w[399] == -1 && w[79600] == 199);
    }
}
