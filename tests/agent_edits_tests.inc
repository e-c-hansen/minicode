// agent_edits_tests.inc — the `agentedits:*` groups of run_tests.cpp
// (AgentEdits.h: what one session changed in one file), kept in their own
// file and included there after agent_index_tests.inc, whose helpers build
// transcripts in a scratch tree.
#include "AgentEdits.h"

static void testAgentEdits() {
    using namespace AgentEdits;
    using namespace agentidx;

    GROUP("agentedits:input");
    {
        std::vector<Change> e = changesFromInput(
            Tool::Edit, R"({"file_path":"/a","old_string":"x = 1","new_string":"x = 2","replace_all":true})");
        CHECK(e.size() == 1 && e[0].oldText == "x = 1" && e[0].newText == "x = 2" && e[0].replaceAll);
        std::vector<Change> m = changesFromInput(
            Tool::MultiEdit, R"({"file_path":"/a","edits":[{"old_string":"a","new_string":"b"},)"
                             R"({"old_string":"c","new_string":"d"}]})");
        CHECK(m.size() == 2 && m[1].part == 1 && m[1].parts == 2 && m[1].newText == "d");
        std::vector<Change> w = changesFromInput(Tool::Write, R"({"file_path":"/a","content":"all\n"})");
        CHECK(w.size() == 1 && w[0].newText == "all\n" && w[0].oldText.empty());
        std::vector<Change> nb = changesFromInput(Tool::NotebookEdit,
                                                  R"j({"notebook_path":"/n.ipynb","new_source":"print(1)"})j");
        CHECK(nb.size() == 1 && nb[0].newText == "print(1)");
        CHECK(changesFromInput(Tool::Edit, "{not json").empty());
        CHECK(changesFromInput(Tool::Edit, R"({"file_path":"/a"})").empty());
        CHECK(isEditTool("MultiEdit") && !isEditTool("Bash") && !isEditTool("Read"));
        CHECK(occurrences("abab ab", "ab", 10) == 3 && occurrences("x", "", 5) == 0 &&
              occurrences("aaaa", "aa", 10) == 2);
    }

    auto edit = [](const std::string &o, const std::string &n, bool all = false) {
        Change c;
        c.tool = Tool::Edit;
        c.oldText = o;
        c.newText = n;
        c.replaceAll = all;
        c.answered = true;
        return c;
    };
    auto writeOf = [](const std::string &content, bool created) {
        Change c;
        c.tool = Tool::Write;
        c.newText = content;
        c.created = created;
        c.answered = true;
        return c;
    };

    GROUP("agentedits:analyze");
    {
        // A file the session made, then edited twice: undone in turn, it was
        // not there before.
        const std::string made = "def hello():\n    print('hi')\n\n\ndef main():\n    hello()\n";
        const std::string now =
            "def hello():\n    print('hello, world')\n\n\ndef main():\n    hello()\n    hello()\n";
        Result r = analyze({writeOf(made, true), edit("print('hi')", "print('hello, world')"),
                            edit("    hello()\n", "    hello()\n    hello()\n")},
                           now, true);
        CHECK(r.rebuilt && !r.existedBefore && r.before.empty() && r.why.empty());
        CHECK(r.standing == 3 && r.gone == 0);
        for (const Change &c : r.changes) CHECK(c.standing == Standing::Stands);
        // An existing file edited: the text before comes back.
        Result e = analyze({edit("a = 1", "a = 2"), edit("b = 1", "b = 3")}, "a = 2\nb = 3\n", true);
        CHECK(e.rebuilt && e.existedBefore && e.before == "a = 1\nb = 1\n");
        // An edit overwritten by hand since: it is gone, and nothing is
        // rebuilt; the earlier one still stands.
        Result g = analyze({edit("a = 1", "a = 2"), edit("b = 1", "b = 3")}, "a = 2\nb = 4\n", true);
        CHECK(!g.rebuilt && g.changes[1].standing == Standing::Gone &&
              g.changes[0].standing == Standing::Stands && g.gone == 1 && g.standing == 1);
        CHECK(g.why.find("edit 2") != std::string::npos);
        // replace_all of several places: stands, but cannot be undone exactly.
        Result ra = analyze({edit("foo", "bar", true)}, "bar(bar)\n", true);
        CHECK(!ra.rebuilt && ra.changes[0].standing == Standing::Stands &&
              ra.why.find("more than once") != std::string::npos);
        // replace_all that touched one place is undone.
        Result r1 = analyze({edit("foo", "bar", true)}, "x = bar\n", true);
        CHECK(r1.rebuilt && r1.before == "x = foo\n");
        // A text found twice.
        Result twice = analyze({edit("x", "y")}, "y y\n", true);
        CHECK(!twice.rebuilt && twice.changes[0].standing == Standing::Stands);
        // An edit that only removed text.
        Result del = analyze({edit("junk\n", "")}, "clean\n", true);
        CHECK(!del.rebuilt && del.changes[0].standing == Standing::Unknown &&
              del.why.find("only removed") != std::string::npos);
        // A rewrite of a file that was there: what was there is not known.
        Result rw = analyze({writeOf("new\n", false)}, "new\n", true);
        CHECK(!rw.rebuilt && rw.changes[0].standing == Standing::Stands);
        Result rwGone = analyze({writeOf("new\n", true)}, "newer\n", true);
        CHECK(!rwGone.rebuilt && rwGone.changes[0].standing == Standing::Gone);
        // A refused call is left out of the reckoning.
        Change refused = edit("a", "zzz");
        refused.refused = true;
        Result rf = analyze({edit("a = 1", "a = 2"), refused}, "a = 2\n", true);
        CHECK(rf.rebuilt && rf.before == "a = 1\n" && rf.changes[1].standing == Standing::Unknown);
        // No file now.
        Result none = analyze({edit("a", "b")}, "", false);
        CHECK(!none.rebuilt && none.changes[0].standing == Standing::Gone);
        // A multi-part edit is undone last part first.
        Change p0 = edit("one", "two"), p1 = edit("two", "three");
        p0.tool = p1.tool = Tool::MultiEdit;
        Result mp = analyze({p0, p1}, "three\n", true);
        CHECK(mp.rebuilt && mp.before == "one\n");
    }

    GROUP("agentedits:collect");
    {
        fsys::path root = scratch("agentedits");
        const std::string sid = "s-edits", cwd = root.string();
        const std::string file = (root / "hello.py").string();
        const std::string other = (root / "other.py").string();
        put(root / "hello.py", "def hello():\n    print('hello, world')\n");
        const std::string t = (root / "t.jsonl").string();
        auto editUse = [&](const std::string &ts, const std::string &id, const std::string &path,
                           const std::string &o, const std::string &n) {
            return use(sid, cwd, ts, id, "Edit",
                       "{\"file_path\":" + q(path) + ",\"old_string\":" + q(o) + ",\"new_string\":" +
                           q(n) + ",\"replace_all\":false}");
        };
        std::string big(300000, 'z');
        put(t,
            prompt(sid, cwd, "2026-11-05T10:00:00Z", "Say hello") + "\n" +
            use(sid, cwd, "2026-11-05T10:00:01Z", "w1", "Write",
                "{\"file_path\":" + q(file) + ",\"content\":" + q("def hello():\n    print('hi')\n") + "}") +
            "\n" + result(sid, cwd, "2026-11-05T10:00:02Z", "w1", "File created successfully at: " + file) +
            "\n" + editUse("2026-11-05T10:00:03Z", "e1", other, "a", "b") + "\n" +
            editUse("2026-11-05T10:00:04Z", "e2", file, "print('hi')", "print('hello, world')") + "\n" +
            result(sid, cwd, "2026-11-05T10:00:05Z", "e2", "The file has been updated successfully.") +
            "\n" + editUse("2026-11-05T10:00:06Z", "e3", file, "nothing", big) + "\n" +
            result(sid, cwd, "2026-11-05T10:00:07Z", "e3", "String to replace not found", true) + "\n");
        CountingFs cfs;
        FileHistory h = collect(cfs.fs, t, file);
        CHECK(h.changes.size() == 3 && h.unreadable == 0);
        if (h.changes.size() == 3) {
            CHECK(h.changes[0].tool == Tool::Write && h.changes[0].created && h.changes[0].answered);
            CHECK(h.changes[1].oldText == "print('hi')" && !h.changes[1].refused && h.changes[1].time > 0);
            CHECK(h.changes[2].refused && h.changes[2].newText.size() == big.size());
            Result r = analyze(h.changes, "def hello():\n    print('hello, world')\n", true);
            CHECK(r.rebuilt && !r.existedBefore && r.standing == 2);
        }
        // The other file has its own one change; the input too large is counted.
        CHECK(collect(cfs.fs, t, other).changes.size() == 1);
        FileHistory small = collect(cfs.fs, t, file, 1000);
        CHECK(small.changes.size() == 2 && small.unreadable == 1);
        // A path spelled through /private/tmp is the same file as /tmp's.
        AgentTranscript::Fs plain = cfs.fs;
        plain.realpath = nullptr;
        CHECK(samePath(plain, "/private/tmp/x/a.py", "/tmp/x/a.py"));
        CHECK(!samePath(plain, "/private/tmpx/a.py", "/tmpx/a.py"));
        CHECK(!samePath(plain, "/tmp/x/a.py", "/tmp/x/b.py"));
        fsys::remove_all(root);
    }

    GROUP("agentedits:fixtures");
    {
        const std::string dir = "tests/fixtures/claude/projects/-Users-user-Code-public-minicode/";
        const std::string mainT = dir + "1e1f578c-45c6-4cb5-92e5-84e11a306454.jsonl";
        const std::string subT =
            dir + "1e1f578c-45c6-4cb5-92e5-84e11a306454/subagents/agent-a1ad4e3e0a7ec7fd2.jsonl";
        AgentTranscript::Fs fs = AgentTranscript::posixFs();
        if (fsys::exists(mainT)) {
            // A Write over a file that was there, then an Edit.
            FileHistory h =
                collect(fs, mainT, "/Users/user/Code/public/minicode/scripts/mutible-launcher");
            CHECK(h.changes.size() == 2);
            if (h.changes.size() == 2) {
                CHECK(h.changes[0].tool == Tool::Write && !h.changes[0].created);
                CHECK(h.changes[1].tool == Tool::Edit && h.changes[1].oldText == "old" &&
                      h.changes[1].newText == "new");
                // The file as the fixture's world has it: the edit is gone.
                Result r = analyze(h.changes, "#!/bin/sh\n", true);
                CHECK(!r.rebuilt && r.changes[1].standing == Standing::Gone);
            }
            // The subagent made LspPicker.h, and wrote into its scratchpad
            // through /private/tmp.
            FileHistory p = collect(fs, subT,
                "/Users/user/Code/public/minicode/.claude/worktrees/agent-a1ad4e3e0a7ec7fd2/src/LspPicker.h");
            CHECK(p.changes.size() == 1 && p.changes[0].created);
            Result made = analyze(p.changes, "placeholder\n", true);
            CHECK(made.rebuilt && !made.existedBefore);
            FileHistory pad = collect(fs, subT,
                "/tmp/claude-501/-Users-user-Code-public-minicode/1e1f578c-45c6-4cb5-92e5-84e11a306454/"
                "scratchpad/edit1.py");
            CHECK(pad.changes.size() == 1);
            // Evidence lists the three edits, in order, beside the commands.
            AgentEvidence::Evidence ev = AgentEvidence::read(fs, subT);
            CHECK(ev.edits.size() == 3 && ev.commands.size() == 3);
            if (ev.edits.size() == 3) {
                CHECK(ev.edits[0].tool == "Edit" && ev.edits[1].tool == "Write" &&
                      ev.edits[2].path.find("LspPicker.h") != std::string::npos);
                CHECK(ev.edits[0].answered && !ev.edits[0].isError && ev.edits[0].time > 0);
                CHECK(ev.edits[0].time <= ev.edits[1].time && ev.edits[1].time <= ev.edits[2].time);
            }
        }
    }
}
