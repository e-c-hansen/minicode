// agent_view_tests.inc — the `agentview:*` groups of run_tests.cpp
// (AgentView.h): the words and groupings the Agents panel, the worktrees in
// the file tree and the Evidence view show.

static void testAgentView() {
    using namespace AgentView;
    using AgentIndex::Place;

    GROUP("agentview:groups");
    {
        std::vector<AgentIndex::FileTouch> files(5);
        files[0].path = "/p/a.txt"; files[0].place = Place::Main; files[0].writes = 1;
        files[1].path = "/tmp/x/scratchpad/r.md"; files[1].place = Place::Outside; files[1].edits = 2;
        files[2].path = "/p/.claude/worktrees/agent-1/b.h"; files[2].place = Place::Worktree;
        files[2].writes = 1;
        files[3].path = "/p/refused.txt"; files[3].place = Place::Main; files[3].failed = 1;
        files[4].path = "/p/c.txt"; files[4].place = Place::Main; files[4].edits = 1;
        std::vector<FileGroup> g = groupFiles(files);
        CHECK(g.size() == 3);
        CHECK(g[0].place == Place::Main && g[0].files == std::vector<size_t>({0, 4}));
        CHECK(g[1].place == Place::Worktree && g[1].files == std::vector<size_t>({2}));
        CHECK(g[2].place == Place::Outside && g[2].files == std::vector<size_t>({1}));
        CHECK(groupFiles({}).empty());
        std::vector<AgentIndex::FileTouch> onlyFailed(1);
        onlyFailed[0].failed = 2;
        CHECK(groupFiles(onlyFailed).empty());
    }

    GROUP("agentview:folder-text");
    CHECK(folderText("/p/src/a.cpp", Place::Main, "/p", "", "", "/h") == "src");
    CHECK(folderText("/p/a.cpp", Place::Main, "/p", "", "", "/h") == "");
    CHECK(folderText("/p/.claude/worktrees/agent-1/src/x.h", Place::Worktree, "/p",
                     "/p/.claude/worktrees/agent-1", "", "/h") == "src");
    // Another session's worktree, found by its folder name.
    CHECK(folderText("/p/.claude/worktrees/agent-2/lib/y.h", Place::Worktree, "/p",
                     "/p/.claude/worktrees/agent-1", "", "/h") == "lib");
    CHECK(folderText("/p/.claude/worktrees/agent-2/y.h", Place::Worktree, "/p", "", "", "/h") == "");
    CHECK(folderText("/t/c/-p/s1/scratchpad/r.md", Place::Outside, "/p", "", "/t/c/-p/s1/scratchpad",
                     "/h") == "scratchpad");
    CHECK(folderText("/t/c/-p/s1/scratchpad/out/r.md", Place::Outside, "/p", "",
                     "/t/c/-p/s1/scratchpad", "/h") == "scratchpad/out");
    CHECK(folderText("/h/notes/x.md", Place::Outside, "/p", "", "", "/h") == "~/notes");
    CHECK(folderText("/h/x.md", Place::Outside, "/p", "", "", "/h") == "~");
    CHECK(folderText("/hx/x.md", Place::Outside, "/p", "", "", "/h") == "/hx");
    CHECK(folderText("/tmp/../tmp/x.md", Place::Outside, "/p", "", "", "/h") == "/tmp");

    GROUP("agentview:row-text");
    CHECK(countsText(3, 12) == "3 files, 12 commands");
    CHECK(countsText(1, 1) == "1 file, 1 command");
    CHECK(countsText(0, 2) == "no files, 2 commands");
    CHECK(countsText(2, 0) == "2 files, no commands");
    CHECK(countsText(0, 0) == "no files or commands");
    CHECK(detailLine(true, "5 min", "worktree-agent-a1", 3, 12) ==
          "running \xC2\xB7 5 min \xC2\xB7 worktree-agent-a1 \xC2\xB7 3 files, 12 commands");
    CHECK(detailLine(false, "", "", 0, 0) == "finished \xC2\xB7 no files or commands");
    CHECK(emptyText(false, false, "/h/.claude/projects").find("Reading") == 0);
    CHECK(emptyText(true, true, "/h/.claude/projects").find("/h/.claude/projects does not exist") !=
          std::string::npos);
    CHECK(emptyText(false, true, "/h/.claude/projects") ==
          "No agent sessions have worked in this folder.");

    GROUP("agentview:worktrees");
    CHECK(worktreeBadge(3, true) == "3 ahead \xC2\xB7 modified");
    CHECK(worktreeBadge(1, false) == "1 ahead");
    CHECK(worktreeBadge(0, false) == "up to date");
    CHECK(worktreeBadge(0, true) == "modified");
    CHECK(worktreeBadge(-1, false) == "");
    CHECK(worktreeBadge(-1, true) == "modified");
    {
        std::vector<AgentIndex::Worktree> all = AgentIndex::parseWorktreeList(
            "worktree /p\nHEAD 1111\nbranch refs/heads/main\n\n"
            "worktree /p/.claude/worktrees/agent-a1\nHEAD 2222\n"
            "branch refs/heads/worktree-agent-a1\n\n");
        CHECK(mainRef(all) == "main");
        std::vector<AgentIndex::Worktree> detached = AgentIndex::parseWorktreeList(
            "worktree /p\nHEAD abcd\ndetached\n\n");
        CHECK(mainRef(detached) == "abcd");
        CHECK(mainRef({}) == "");
        // Compared with main where the branch left it, when that was found.
        CompareBase base = compareBase("main", "0123456789abcdef0123456789abcdef01234567\n");
        CHECK(base.ref == "0123456789abcdef0123456789abcdef01234567");
        CHECK(base.label == "main at the branch point (0123456)");
        CHECK(compareBase("main", "").ref == "main" && compareBase("main", "").label == "main");
        CHECK(compareBase("main", "fatal: no merge base\n").ref == "main");
        // No sessions for / or the home folder.
        CHECK(!indexesFolder("/", "/Users/me") && !indexesFolder("/Users/me", "/Users/me") &&
              !indexesFolder("/Users/me/", "/Users/me") && !indexesFolder("", "/Users/me"));
        CHECK(indexesFolder("/Users/me/code", "/Users/me") && indexesFolder("/Users", "/Users/me"));
    }
    {
        static const char nsz[] =
            "M\0src/a.cpp\0A\0src/new.h\0R087\0old/name.txt\0new/name.txt\0D\0gone.md\0";
        static const char utz[] = "notes/todo.md\0src/a.cpp\0";
        std::string ns(nsz, sizeof nsz - 1), untracked(utz, sizeof utz - 1);
        std::vector<Change> c = parseChanges(ns, untracked);
        CHECK(c.size() == 5);
        CHECK(c[0].path == "gone.md" && c[0].status == 'D');
        CHECK(c[1].path == "new/name.txt" && c[1].status == 'R' && c[1].from == "old/name.txt");
        CHECK(c[2].path == "notes/todo.md" && c[2].status == '?');
        CHECK(c[3].path == "src/a.cpp" && c[3].status == 'M');   // not repeated as untracked
        CHECK(c[4].path == "src/new.h" && c[4].status == 'A');
        CHECK(parseChanges("", "").empty());
        CHECK(parseChanges(std::string("M\0", 2), "").empty());   // cut short: nothing made up
    }

    GROUP("agentview:evidence-text");
    {
        AgentEvidence::Summary s;
        CHECK(summaryText(s) == "");
        s.found = true; s.format = "count"; s.passed = 3630; s.failed = 1;
        CHECK(summaryText(s) == "1 failed, 3,630 passed");
        s.failed = 0;
        CHECK(summaryText(s) == "3,630 passed, 0 failed");
        s.format = "pytest"; s.passed = 12; s.skipped = 1;
        CHECK(summaryText(s) == "12 passed, 1 skipped");
        s.errors = 2; s.passed = 5; s.skipped = 0;
        CHECK(summaryText(s) == "2 errors, 5 passed");
        s.errors = 1;
        CHECK(summaryText(s) == "1 error, 5 passed");
        AgentEvidence::Command c;
        CHECK(std::string(resultWord(c)) == "running");
        c.answered = true;
        CHECK(std::string(resultWord(c)) == "ok");
        c.isError = true;
        CHECK(std::string(resultWord(c)) == "error");
        CHECK(moreLinesText(1234) == "1,234 more lines");
        CHECK(moreLinesText(1) == "1 more line");
        CHECK(moreLinesText(-1) == "more lines");
        CHECK(clipLine("short") == "short");
        CHECK(clipLine(std::string(500, 'x'), 10) == std::string(10, 'x') + "\xE2\x80\xA6");
        // Never inside a character: "é" is two bytes.
        std::string accents;
        for (int i = 0; i < 10; i++) accents += "\xC3\xA9";
        std::string cut = clipLine(accents, 5);
        CHECK(cut == "\xC3\xA9\xC3\xA9\xE2\x80\xA6");
    }

    GROUP("agentview:not-found");
    CHECK(notFoundText("src/a.py", {"/Users/u/p", "/Users/u/p/.claude/worktrees/agent-1",
                                    "/private/tmp/claude-501/x/s1/scratchpad"},
                       "/Users/u") ==
          "Not found: src/a.py (looked in ~/p, ~/p/.claude/worktrees/agent-1, "
          "/private/tmp/claude-501/x/s1/scratchpad)");
    CHECK(notFoundText("x", {}, "/h") == "Not found: x");
    CHECK(notFoundText("x", {"/hx/y"}, "/h") == "Not found: x (looked in /hx/y)");
}
