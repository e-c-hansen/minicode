// agent_task_tests.inc — the `agenttask:*` groups of run_tests.cpp
// (AgentTask.h): New Agent Task's name from the prompt, the name made fit
// for a branch and a folder, a free one, the worktree's place, and the line
// typed into the terminal, run through a real shell to show the prompt
// arrives as one argument, exactly as typed.

// What `line` prints when /bin/sh runs it; "<no shell>" when it cannot.
static std::string agentTaskShellOutput(const std::string &line) {
    FILE *p = popen(line.c_str(), "r");
    if (!p) return "<no shell>";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

static void testAgentTask() {
    using namespace AgentTask;

    GROUP("agenttask:name");
    CHECK(nameFromPrompt("Fix the login bug when the password is empty") == "fix-login-bug-when");
    CHECK(nameFromPrompt("Please add a dark theme") == "add-dark-theme");
    CHECK(nameFromPrompt("Don't crash on empty files") == "dont-crash-empty-files");
    CHECK(nameFromPrompt("don\xE2\x80\x99t panic") == "dont-panic");
    CHECK(nameFromPrompt("Rename `parseArgs()` to parseOptions") == "rename-parseargs-parseoptions");
    CHECK(nameFromPrompt("") == "task");
    CHECK(nameFromPrompt("   \n\t ") == "task");
    CHECK(nameFromPrompt("the a an to") == "task");
    CHECK(nameFromPrompt("¿Qué pasa? ñandú") == "qu-pasa");   // "and" is left out
    CHECK(nameFromPrompt("$(rm -rf ~) `whoami`; echo hi") == "rm-rf-whoami-echo");
    // A long word is cut; later words that do not fit are left out.
    CHECK(nameFromPrompt(std::string(50, 'x')) == std::string(32, 'x'));
    CHECK(nameFromPrompt("refactor supercalifragilistic expialidocious") == "refactor-supercalifragilistic");
    for (const char *p : {"Fix it", "a", "Make the tests pass!", "x/y/z ../../etc"}) {
        const std::string n = nameFromPrompt(p);
        CHECK(!n.empty() && n.size() <= 32);
        CHECK(sanitizeName(n) == n);   // already fit to be a branch
    }

    GROUP("agenttask:sanitize");
    CHECK(sanitizeName("fix-login") == "fix-login");
    CHECK(sanitizeName("Fix Login Bug") == "Fix-Login-Bug");
    CHECK(sanitizeName("feature/x") == "feature-x");
    CHECK(sanitizeName("../../etc/passwd") == "etc-passwd");
    CHECK(sanitizeName("a..b") == "a.b");
    CHECK(sanitizeName("--x--") == "x");
    CHECK(sanitizeName(".hidden") == "hidden");
    CHECK(sanitizeName("name.lock") == "name");
    CHECK(sanitizeName("name.lock.lock") == "name");
    CHECK(sanitizeName("a b\tc\nd") == "a-b-c-d");
    CHECK(sanitizeName("x'y\"z$(w)`v`") == "x-y-z-w-v");
    CHECK(sanitizeName("@{HEAD}~1^:?*[") == "HEAD-1");
    CHECK(sanitizeName("café") == "caf");
    CHECK(sanitizeName("") == "");
    CHECK(sanitizeName("///") == "");
    CHECK(sanitizeName("-") == "");
    CHECK(sanitizeName(std::string(80, 'a')).size() == 60);
    CHECK(sanitizeName(std::string(59, 'a') + "-b") == std::string(59, 'a'));

    GROUP("agenttask:unique");
    {
        std::set<std::string> taken = {"fix", "fix-2", "other"};
        auto isTaken = [&](const std::string &n) { return taken.count(n) > 0; };
        CHECK(uniqueName("fix", isTaken) == "fix-3");
        CHECK(uniqueName("other", isTaken) == "other-2");
        CHECK(uniqueName("free", isTaken) == "free");
    }

    GROUP("agenttask:place");
    CHECK(worktreePath("/p/repo", "fix") == "/p/repo/.claude/worktrees/fix");
    CHECK(worktreePath("/p/repo/", "fix") == "/p/repo/.claude/worktrees/fix");
    CHECK(insideClaudeHome("/Users/me/.claude/worktrees/fix", "/Users/me"));
    CHECK(insideClaudeHome("/Users/me/.claude", "/Users/me/"));
    CHECK(!insideClaudeHome("/Users/me/code/.claude/worktrees/fix", "/Users/me"));
    CHECK(!insideClaudeHome("/Users/me/.claudette/x", "/Users/me"));
    CHECK(!insideClaudeHome("/x", ""));
    CHECK(worktreeAddArgs("fix", "/p/.claude/worktrees/fix") ==
          std::vector<std::string>({"worktree", "add", "-b", "fix", "/p/.claude/worktrees/fix", "HEAD"}));

    GROUP("agenttask:command");
    CHECK(promptFitsOnLine("Fix the login bug"));
    CHECK(!promptFitsOnLine(""));
    CHECK(!promptFitsOnLine("two\nlines"));
    CHECK(!promptFitsOnLine("a\ttab"));
    CHECK(!promptFitsOnLine(std::string(161, 'x')));
    CHECK(promptFitsOnLine(std::string(160, 'x')));
    CHECK(startCommand("claude", "/p/.claude/worktrees/fix", "Fix the bug", "/t/f.txt") ==
          "cd /p/.claude/worktrees/fix && claude 'Fix the bug'");
    CHECK(startCommand("claude", "/p q/wt", "it's broken", "/t/f.txt") ==
          "cd '/p q/wt' && claude 'it'\\''s broken'");
    CHECK(startCommand("  claude --model opus ", "/w", "two\nlines", "/t/a b.txt") ==
          "cd /w && claude --model opus \"$(cat '/t/a b.txt')\"");
    CHECK(trimPrompt("\n  Fix it  \n\n") == "Fix it");
    CHECK(trimPrompt("a\n\nb") == "a\n\nb");

    GROUP("agenttask:shell");
    // The line through /bin/sh with printf standing in for claude: what it
    // prints is the one argument the prompt became.
    if (access("/bin/sh", X_OK) == 0) {
        char tmpl[] = "/tmp/mc-agenttask-XXXXXX";
        const char *dir = mkdtemp(tmpl);
        CHECK(dir != nullptr);
        const std::string d = dir ? dir : "/tmp";
        const std::string file = d + "/prompt it's.txt";
        const std::vector<std::string> prompts = {
            "Fix the bug",
            "it's \"quoted\" and $(touch " + d + "/pwned) `touch " + d + "/pwned2` $HOME !! \\n ; | & > x",
            "two lines\nwith $(touch " + d + "/pwned3) and 'quotes'\n\tand a tab",
            std::string(200, 'y') + " a long one; rm -rf /nonexistent",
            "ünïcödé — ✓ 'mixed' \"q\"",
        };
        for (const std::string &p : prompts) {
            std::ofstream(file) << p;
            const std::string line = startCommand("printf '[%s]'", d, p, file);
            {
                const std::string got = agentTaskShellOutput(line);
                if (got != "[" + p + "]") std::printf("  line: %s\n  printed: %s\n", line.c_str(), got.c_str());
                CHECK(got == "[" + p + "]");
            }
        }
        // Nothing the prompts held was run.
        struct stat st;
        CHECK(stat((d + "/pwned").c_str(), &st) != 0);
        CHECK(stat((d + "/pwned2").c_str(), &st) != 0);
        CHECK(stat((d + "/pwned3").c_str(), &st) != 0);
        // The folder is quoted too: the shell ends up in it.
        const std::string odd = d + "/a b'c $(x)";
        mkdir(odd.c_str(), 0700);
        CHECK(agentTaskShellOutput(startCommand("pwd -P; printf %s", odd, "ok", file)) ==
                 [&] {
                     char buf[PATH_MAX];
                     return std::string(realpath(odd.c_str(), buf) ? buf : odd.c_str()) + "\nok";
                 }());
        rmdir(odd.c_str());
        unlink(file.c_str());
        rmdir(d.c_str());
    }

    GROUP("agenttask:words");
    CHECK(startsFromText("main", 0) == "From the last commit on main");
    CHECK(startsFromText("", 0) == "From the last commit (HEAD)");
    CHECK(startsFromText("main", 1) ==
          "From the last commit on main; your 1 uncommitted change stays here and is not in the "
          "task's worktree");
    CHECK(startsFromText("dev", 3) ==
          "From the last commit on dev; your 3 uncommitted changes stay here and are not in the "
          "task's worktree");
}
