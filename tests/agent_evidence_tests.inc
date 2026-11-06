// agent_evidence_tests.inc — the `evidence:*` groups of run_tests.cpp
// (AgentEvidence.h), included after agent_index_tests.inc, whose helpers
// (namespace agentidx) build the transcripts.

static void testAgentEvidence() {
    using namespace AgentEvidence;
    using namespace agentidx;

    GROUP("evidence:commands");
    CHECK(testCommand("pytest") == TestTool::Pytest);
    CHECK(testCommand("pytest -x tests/test_a.py") == TestTool::Pytest);
    CHECK(testCommand(".venv/bin/python -m pytest -q") == TestTool::Pytest);
    CHECK(testCommand("python3 -m pytest") == TestTool::Pytest);
    CHECK(testCommand("python -m unittest") == TestTool::None);
    CHECK(testCommand("python script.py pytest") == TestTool::None);
    CHECK(testCommand("uv run pytest -k foo") == TestTool::Pytest);
    CHECK(testCommand("uv run --with rich python -m pytest") == TestTool::Pytest);
    CHECK(testCommand("make test") == TestTool::Make);
    CHECK(testCommand("make check") == TestTool::Make);
    CHECK(testCommand("make -C build -j8 test") == TestTool::Make);
    CHECK(testCommand("make JOBS=1 test 2>&1 | tail -3") == TestTool::Make);
    CHECK(testCommand("cd /w/p && make 2>&1 | grep -E 'warning|error'; make test 2>&1 | tail -3") ==
          TestTool::Make);
    CHECK(testCommand("make") == TestTool::None);
    CHECK(testCommand("make install") == TestTool::None);
    CHECK(testCommand("ctest --output-on-failure") == TestTool::Ctest);
    CHECK(testCommand("npm test") == TestTool::Npm);
    CHECK(testCommand("npm run test") == TestTool::Npm);
    CHECK(testCommand("npm run test:unit") == TestTool::Npm);
    CHECK(testCommand("npm run build") == TestTool::None);
    CHECK(testCommand("pnpm test") == TestTool::Npm);
    CHECK(testCommand("cargo test --release") == TestTool::Cargo);
    CHECK(testCommand("cargo +nightly test") == TestTool::Cargo);
    CHECK(testCommand("cargo build") == TestTool::None);
    CHECK(testCommand("go test ./...") == TestTool::Go);
    CHECK(testCommand("go build ./...") == TestTool::None);
    CHECK(testCommand("FOO=1 timeout 600 make test") == TestTool::Make);
    CHECK(testCommand("env CI=1 time pytest") == TestTool::Pytest);
    CHECK(testCommand("echo 'make test'") == TestTool::None);
    CHECK(testCommand("grep -n 'pytest' setup.cfg") == TestTool::None);
    CHECK(testCommand("(cd build && ctest)") == TestTool::Ctest);
    CHECK(testCommand("") == TestTool::None);
    CHECK(std::string(testToolName(TestTool::Cargo)) == "cargo");

    GROUP("evidence:summary");
    Summary s = parseSummary("collected 14 items\n...\n=========== 3 failed, 10 passed, 1 skipped, 2 warnings in 1.23s ===========\n");
    CHECK(s.found && s.format == "pytest" && s.failed == 3 && s.passed == 10 && s.skipped == 1 && s.errors == 0);
    CHECK(s.line == "=========== 3 failed, 10 passed, 1 skipped, 2 warnings in 1.23s ===========");
    s = parseSummary("5 passed in 0.03s");
    CHECK(s.found && s.passed == 5 && s.failed == 0);
    s = parseSummary("==== 1 failed, 2 errors in 0.51s ====");
    CHECK(s.failed == 1 && s.errors == 2);
    s = parseSummary("=== 1 error in 0.10s ===");
    CHECK(s.found && s.errors == 1);
    s = parseSummary("no tests ran in 0.01s");
    CHECK(s.found && s.format == "pytest" && s.passed == 0);
    s = parseSummary("\x1b[32m==== \x1b[1m7 passed\x1b[0m\x1b[32m in 2.0s ====\x1b[0m");
    CHECK(s.found && s.passed == 7);
    s = parseSummary("83% tests passed, 2 tests failed out of 12\n\nTotal Test time (real) = 1.2 sec");
    CHECK(s.found && s.format == "ctest" && s.passed == 10 && s.failed == 2);
    s = parseSummary("100% tests passed, 0 tests failed out of 4");
    CHECK(s.passed == 4 && s.failed == 0);
    s = parseSummary(
        "running 3 tests\ntest result: ok. 3 passed; 0 failed; 1 ignored; 0 measured; 0 filtered out; finished in 0.00s\n"
        "running 2 tests\ntest result: FAILED. 1 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out\n",
        TestTool::Cargo);
    CHECK(s.found && s.format == "cargo" && s.passed == 4 && s.failed == 1 && s.skipped == 1);
    CHECK(s.line.compare(0, 19, "test result: FAILED") == 0);
    s = parseSummary("ok  \texample.com/a\t0.012s\n--- FAIL: TestX (0.00s)\nFAIL\nFAIL\texample.com/b\t0.2s\n"
                     "?   \texample.com/c\t[no test files]\nok  \texample.com/d\t(cached)\n",
                     TestTool::Go);
    CHECK(s.found && s.format == "go" && s.passed == 2 && s.failed == 1);
    CHECK(!parseSummary("ok  \tx\t0.1s\n", TestTool::Make).found);   // go lines only for go test
    s = parseSummary("Running MiniCode core tests...\n\n4466 passed, 0 failed\n");
    CHECK(s.found && s.format == "count" && s.passed == 4466 && s.failed == 0);
    s = parseSummary("  FAIL [x] tests/run_tests.cpp:10  cond\n\n3630 passed, 1 failed");
    CHECK(s.failed == 1 && s.passed == 3630);
    CHECK(!parseSummary("Everything passed, nothing failed").found);
    CHECK(!parseSummary("built in 3s").found);
    CHECK(!parseSummary("").found);
    // The last summary wins (a rerun in the same output).
    s = parseSummary("2 passed, 1 failed\n...\n3 passed, 0 failed\n");
    CHECK(s.passed == 3 && s.failed == 0);
    CHECK(stripAnsi("\x1b[1;31mred\x1b[0m \x1b]0;title\x07x") == "red x");

    std::ifstream probe("tests/fixtures/claude/projects/-Users-user-Code-public-minicode/"
                        "1e1f578c-45c6-4cb5-92e5-84e11a306454/subagents/agent-a1ad4e3e0a7ec7fd2.jsonl");
    if (!probe) return;
    probe.close();

    GROUP("evidence:fixtures");
    fsys::path root = scratch("evidence");
    fsys::path t = root / "agent.jsonl";
    fsys::copy_file("tests/fixtures/claude/projects/-Users-user-Code-public-minicode/"
                    "1e1f578c-45c6-4cb5-92e5-84e11a306454/subagents/agent-a1ad4e3e0a7ec7fd2.jsonl",
                    t);
    CountingFs cfs;
    Reader r(t.string());
    CHECK(r.update(cfs.fs));
    const Evidence &ev = r.evidence();
    CHECK(ev.commands.size() == 3 && ev.badLines == 0);
    CHECK(ev.versions == std::vector<std::string>{"2.1.318"});
    CHECK(ev.cwd == "/Users/user/Code/public/minicode/.claude/worktrees/agent-a1ad4e3e0a7ec7fd2");
    if (ev.commands.size() == 3) {
        const Command &a = ev.commands[0], &b = ev.commands[1], &c = ev.commands[2];
        CHECK(a.command == "ls src" && a.isError && a.answered && a.test == TestTool::None);
        CHECK(a.description == "Implement core LSP editing features");
        CHECK(b.test == TestTool::Make && !b.isError && b.summary.found &&
              b.summary.passed == 3630 && b.summary.failed == 1);
        CHECK(c.summary.passed == 3631 && c.summary.failed == 0 && c.description == "Fix the right line and rerun");
        AgentTranscript::Output o = AgentTranscript::readOutput(cfs.fs, t.string(), c.output, 20, 20);
        CHECK(o.whole && o.head.size() == 3 && o.head[2] == "3631 passed, 0 failed");
        CHECK(c.cwd == ev.cwd && c.time > 0);
    }
    CHECK(ev.lastTest == 2 && ev.lastTestCommand() && ev.lastTestCommand()->summary.failed == 0);
    CHECK(ev.finalMessage == "Placeholder reply.");
    CHECK(!r.update(cfs.fs));
    // Then the session runs make test once more, it fails, and the final
    // message says the tests pass: the failure shows beside the claim.
    const std::string sid = "1e1f578c-45c6-4cb5-92e5-84e11a306454";
    const std::string cwd = ev.cwd;
    std::string big = "FAIL [x]\n" + std::string(1500000, '.') + "\n3631 passed, 2 failed\n";
    put(t,
        bash(sid, cwd, "2026-11-05T14:00:00Z", "tb1", "make test 2>&1 | tail -3", "Run the tests") + "\n" +
        result(sid, cwd, "2026-11-05T14:00:30Z", "tb1", big) + "\n" +
        text(sid, cwd, "2026-11-05T14:01:00Z", "All tests pass.", "mfinal") + "\n" +
        text(sid, cwd, "2026-11-05T14:01:00Z", "Committed.", "mfinal") + "\n" +
        bash(sid, cwd, "2026-11-05T14:02:00Z", "tb2", "pytest", "Python tests") + "\n",
        true);
    CHECK(r.update(cfs.fs));
    CHECK(ev.commands.size() == 5 && ev.lastTest == 4);
    if (ev.commands.size() == 5) {
        const Command &d = ev.commands[3];
        CHECK(d.answered && d.summary.found && d.summary.failed == 2 && d.summary.passed == 3631);
        CHECK(d.output.length > 1000000);
        *cfs.bytes = 0;
        AgentTranscript::Output o = AgentTranscript::readOutput(cfs.fs, t.string(), d.output, 20, 20);
        CHECK(*cfs.bytes <= 2 * 64 * 1024 && o.omitted == -1 && o.head[0] == "FAIL [x]" &&
              o.tail.back() == "3631 passed, 2 failed");
        CHECK(!ev.commands[4].answered && ev.commands[4].test == TestTool::Pytest);
    }
    CHECK(ev.finalMessage == "All tests pass.\n\nCommitted.");
    CHECK(ev.finalTime == AgentTranscript::parseTime("2026-11-05T14:01:00Z"));
    // The last test command has no result yet; the one before it failed.
    CHECK(ev.lastTestCommand() && !ev.lastTestCommand()->answered);
    // A test command that went on in the background: its summary comes from
    // the file it writes.
    put(root / "bg.output", "...\n12 passed, 0 failed\n");
    put(t, result(sid, cwd, "2026-11-05T14:03:00Z", "tb2",
                  "Command was moved to the background (ID: b1) after running for 180s. "
                  "Output is being written to: " + (root / "bg.output").string() + ". You will be notified.") +
               "\n",
        true);
    CHECK(r.update(cfs.fs));
    if (ev.commands.size() == 5) {
        const Command &e = ev.commands[4];
        CHECK(e.inBackground && e.persistedPath == (root / "bg.output").string());
        CHECK(e.summary.found && e.summary.passed == 12);
    }
    // One-shot read agrees.
    Evidence again = read(cfs.fs, t.string());
    CHECK(again.commands.size() == 5 && again.finalMessage == ev.finalMessage);
    fsys::remove_all(root);
}
