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

    GROUP("evidence:unittest");
    CHECK(testCommand("python -m unittest") == TestTool::Unittest);
    CHECK(testCommand("python3 -m unittest discover -s tests -v") == TestTool::Unittest);
    CHECK(testCommand(".venv/bin/python -m unittest tests.test_a") == TestTool::Unittest);
    CHECK(testCommand("uv run python -m unittest") == TestTool::Unittest);
    CHECK(testCommand("cd pkg && python3 -m unittest 2>&1 | tail -5") == TestTool::Unittest);
    CHECK(testCommand("python -m unittestish") == TestTool::None);
    CHECK(std::string(testToolName(TestTool::Unittest)) == "unittest");
    s = parseSummary("..s.\n----------------------------------------------------------------------\n"
                     "Ran 4 tests in 0.004s\n\nOK (skipped=1)\n");
    CHECK(s.found && s.format == "unittest" && s.passed == 3 && s.skipped == 1 && s.failed == 0);
    CHECK(s.line == "Ran 4 tests in 0.004s; OK (skipped=1)");
    s = parseSummary("F.E\n======\nFAIL: test_x (t.T)\n...\nRan 12 tests in 0.020s\n\n"
                     "FAILED (failures=2, errors=1, expected failures=1)\n");
    CHECK(s.found && s.passed == 8 && s.failed == 2 && s.errors == 1);
    s = parseSummary("Ran 1 test in 0.000s\n\nOK\n");
    CHECK(s.found && s.passed == 1);
    s = parseSummary("\nRan 0 tests in 0.000s\n\nNO TESTS RAN\n");
    CHECK(s.found && s.passed == 0 && s.failed == 0);
    CHECK(!parseSummary("Ran 4 tests in 0.004s\n").found);         // no verdict yet
    CHECK(!parseSummary("Ran 4 tests in 0.004s\n\nmaybe\n").found);
    CHECK(!parseSummary("Ran 4 errands in town\n\nOK\n").found);

    GROUP("evidence:jest");
    CHECK(testCommand("jest") == TestTool::Jest);
    CHECK(testCommand("npx jest --runInBand src/") == TestTool::Jest);
    CHECK(testCommand("yarn jest") == TestTool::Jest);
    CHECK(testCommand("pnpm exec jest -t parse") == TestTool::Jest);
    CHECK(testCommand("node_modules/.bin/jest") == TestTool::Jest);
    CHECK(testCommand("npx eslint .") == TestTool::None);
    s = parseSummary("PASS src/a.test.js\nFAIL src/b.test.js\n\n"
                     "Test Suites: 1 failed, 1 passed, 2 total\n"
                     "Tests:       1 failed, 2 skipped, 10 passed, 13 total\n"
                     "Snapshots:   0 total\nTime:        1.2 s\nRan all test suites.\n");
    CHECK(s.found && s.format == "jest" && s.failed == 1 && s.skipped == 2 && s.passed == 10);
    CHECK(s.line == "Tests:       1 failed, 2 skipped, 10 passed, 13 total");
    s = parseSummary("Tests:       4 passed, 4 total\n", TestTool::Npm);   // npm test running jest
    CHECK(s.found && s.passed == 4 && s.failed == 0);
    s = parseSummary("Tests:       1 todo, 3 passed, 4 total\n");
    CHECK(s.found && s.skipped == 1 && s.passed == 3);
    CHECK(!parseSummary("Tests: see the docs\n").found);

    GROUP("evidence:vitest");
    CHECK(testCommand("vitest run") == TestTool::Vitest);
    CHECK(testCommand("npx vitest run --reporter=dot") == TestTool::Vitest);
    CHECK(testCommand("pnpm vitest") == TestTool::Vitest);
    CHECK(testCommand("bunx vitest") == TestTool::Vitest);
    s = parseSummary(" \x1b[32m\u2713\x1b[39m src/a.test.ts (3)\n\n"
                     " Test Files  \x1b[1m\x1b[31m1 failed\x1b[39m\x1b[22m | \x1b[32m3 passed\x1b[39m (4)\n"
                     "      Tests  \x1b[1m\x1b[31m1 failed\x1b[39m\x1b[22m | \x1b[32m10 passed\x1b[39m | 1 skipped (12)\n"
                     "   Start at  10:00:00\n   Duration  1.20s\n");
    CHECK(s.found && s.format == "vitest" && s.failed == 1 && s.passed == 10 && s.skipped == 1);
    s = parseSummary("      Tests  7 passed (7)\n");
    CHECK(s.found && s.passed == 7);
    CHECK(!parseSummary(" Test Files  3 passed (3)\n").found);   // files, not tests

    GROUP("evidence:tox");
    CHECK(testCommand("tox") == TestTool::Tox);
    CHECK(testCommand("tox -e py312 -- -x") == TestTool::Tox);
    CHECK(testCommand("uvx tox -p") == TestTool::Tox);
    CHECK(testCommand("python -m tox") == TestTool::Tox);
    CHECK(testCommand("pipx run tox") == TestTool::Tox);
    CHECK(testCommand("nox -s tests") == TestTool::Nox);
    CHECK(testCommand("uvx nox") == TestTool::Nox);
    CHECK(std::string(testToolName(TestTool::Nox)) == "nox");
    // tox 4: environments counted, not the last environment's pytest line.
    const std::string tox4 =
        "py311: commands[0]> pytest\n===== 5 passed in 0.10s =====\n"
        "py312: commands[0]> pytest\n===== 1 failed, 4 passed in 0.12s =====\n"
        "  py311: OK (1.23=setup[0.50]+cmd[0.73] seconds)\n"
        "  py312: FAIL code 1 (1.10=setup[0.40]+cmd[0.70] seconds)\n"
        "  evaluation failed :( (2.40 seconds)\n";
    s = parseSummary(tox4, TestTool::Tox);
    CHECK(s.found && s.format == "tox" && s.passed == 1 && s.failed == 1);
    CHECK(s.line == "py312: FAIL code 1 (1.10=setup[0.40]+cmd[0.70] seconds)");
    // Without the hint the last runner's own line is what is found.
    s = parseSummary(tox4);
    CHECK(s.found && s.format == "pytest" && s.failed == 1);
    // tox 3.
    s = parseSummary("___ summary ___\n  py39: commands succeeded\nERROR:   py310: commands failed\n",
                     TestTool::Tox);
    CHECK(s.found && s.passed == 1 && s.failed == 1);
    s = parseSummary("___ summary ___\n  py39: commands succeeded\n  congratulations :)\n",
                     TestTool::Tox);
    CHECK(s.found && s.passed == 1 && s.failed == 0);
    // tox that printed no environment lines (cut off): what ran inside.
    s = parseSummary("Ran 3 tests in 0.001s\n\nOK\n", TestTool::Tox);
    CHECK(s.found && s.format == "unittest" && s.passed == 3);
    // nox.
    s = parseSummary("nox > Running session tests-3.11\nnox > Session tests-3.11 was successful in 2 seconds.\n"
                     "nox > Session tests-3.12 failed.\nnox > Session lint skipped: no interpreter.\n"
                     "nox > Ran multiple sessions:\nnox > * tests-3.11: success\nnox > * tests-3.12: failed\n",
                     TestTool::Nox);
    CHECK(s.found && s.format == "nox" && s.passed == 1 && s.failed == 1 && s.skipped == 1);

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
