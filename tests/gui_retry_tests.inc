// The GUI harnesses' seeded order and one retry of a failed suite
// (tests/GuiRetry.h), as both harnesses use them.
#include "GuiRetry.h"
#include <filesystem>
#include <random>

static void testGuiRetry() {
    GROUP("gui-retry:order");
    {
        uint64_t seed = 0;
        CHECK(GuiRetry::parseSeed("12345", &seed) && seed == 12345);
        CHECK(GuiRetry::parseSeed("18446744073709551615", &seed) && seed == UINT64_MAX);
        CHECK(!GuiRetry::parseSeed("18446744073709551616", &seed));
        CHECK(!GuiRetry::parseSeed("", &seed));
        CHECK(!GuiRetry::parseSeed("12a", &seed));
        CHECK(!GuiRetry::parseSeed("-1", &seed));
        // Every position once, the same for the same seed on every machine
        // (the first draws are pinned, so a change to the generator, which
        // would stop old seeds replaying, fails here).
        const std::vector<size_t> a = GuiRetry::shuffledOrder(10, 42), b = GuiRetry::shuffledOrder(10, 42);
        CHECK(a == b);
        std::vector<size_t> sorted = a;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); i++) CHECK(sorted[i] == i);
        uint64_t state = 0;
        CHECK(GuiRetry::nextRandom(state) == 0xE220A8397B1DCDAFull);
        CHECK(GuiRetry::nextRandom(state) == 0x6E789E6AA1B965F4ull);
        // Different seeds give different orders (for 20 suites, two seeds
        // agreeing would be a broken generator).
        CHECK(GuiRetry::shuffledOrder(20, 1) != GuiRetry::shuffledOrder(20, 2));
        CHECK(GuiRetry::shuffledOrder(0, 7).empty());
        CHECK(GuiRetry::shuffledOrder(1, 7) == std::vector<size_t>{0});
    }

    GROUP("gui-retry:results");
    {
        GuiRetry::Results r;
        r.passed = 40;
        GuiRetry::noteFailure(r, "tree", "TreeSuite.mm:12  the row shows\t(in: a step)\n");
        GuiRetry::noteFailure(r, "tree", "TreeSuite.mm:30  later");
        GuiRetry::noteFailure(r, "git_panel", "LEAK left a window");
        CHECK(r.failed == 3 && r.outside == 0 && r.suites.size() == 2);
        CHECK(r.suites[0].suite == "tree" && r.suites[0].failed == 2);
        CHECK(r.suites[0].first == "TreeSuite.mm:12  the row shows (in: a step)");
        GuiRetry::noteFailure(r, "", "the whole run took over 1500 s");
        CHECK(r.outside == 1 && r.failed == 4);
        r.complete = true;
        const GuiRetry::Results back = GuiRetry::parseResults(GuiRetry::formatResults(r));
        CHECK(back.complete && back.passed == 40 && back.failed == 4 && back.outside == 1);
        CHECK(back.suites.size() == 2 && back.suites[1].suite == "git_panel" &&
              back.suites[1].failed == 1 && back.suites[1].first == "LEAK left a window");
        // A file cut short (the run killed while writing) is not complete.
        std::string text = GuiRetry::formatResults(r);
        CHECK(!GuiRetry::parseResults(text.substr(0, text.size() - 9)).complete);
        CHECK(!GuiRetry::parseResults("").complete);
        CHECK(GuiRetry::flakyLine(back.suites[0]) ==
              "  FLAKY [tree] TreeSuite.mm:12  the row shows (in: a step)");
    }

    GROUP("gui-retry:plan");
    {
        GuiRetry::Results r;
        r.complete = true;
        CHECK(GuiRetry::planRetries(r, 3).retry.empty() && GuiRetry::planRetries(r, 3).refusal.empty());
        for (const char *s : {"a", "b", "c"}) GuiRetry::noteFailure(r, s, "x");
        CHECK(GuiRetry::planRetries(r, 3).retry.size() == 3);
        GuiRetry::Results four = r;
        GuiRetry::noteFailure(four, "d", "x");
        CHECK(GuiRetry::planRetries(four, 3).retry.empty());
        CHECK(GuiRetry::planRetries(four, 3).refusal.find("4 suites failed, more than the 3") == 0);
        GuiRetry::Results outside = r;
        GuiRetry::noteFailure(outside, "", "no suite is named x");
        CHECK(GuiRetry::planRetries(outside, 3).retry.empty() && !GuiRetry::planRetries(outside, 3).refusal.empty());
        GuiRetry::Results crashed = r;
        crashed.complete = false;
        CHECK(GuiRetry::planRetries(crashed, 3).retry.empty() && !GuiRetry::planRetries(crashed, 3).refusal.empty());
        GuiRetry::Results counted;   // failures with no suite list (a corrupt file)
        counted.complete = true;
        counted.failed = 2;
        CHECK(GuiRetry::planRetries(counted, 3).retry.empty() && !GuiRetry::planRetries(counted, 3).refusal.empty());
    }

    GROUP("gui-retry:run");
    {
        namespace fs = std::filesystem;
        std::random_device rd;
        const fs::path root = fs::temp_directory_path() / ("minicode-gui-retry-" + std::to_string(rd()));
        fs::create_directories(root);
        // A stand-in for the harness's program: "flaky" passes when run as a
        // retry, "broken" never does; each says what it was given.
        const fs::path program = root / "guitest";
        {
            std::ofstream f(program);
            f << "#!/bin/sh\n"
                 "echo \"suite $1, retry of $MC_GUI_RETRY_OF, pictures $MC_GUI_SNAPSHOTS\"\n"
                 "echo \"skip=$MC_GUI_SKIP_SUITES shuffle=$MC_GUI_SHUFFLE keep=$KEEP_ME\" >&2\n"
                 "printf 'no newline at the end'\n"
                 "[ \"$1\" = flaky ] && [ \"$MC_GUI_RETRY_OF\" = flaky ]\n";
        }
        fs::permissions(program, fs::perms::owner_all);
        const std::vector<std::string> env = {"PATH=/usr/bin:/bin", "KEEP_ME=yes", "MC_GUI_SKIP_SUITES=a b",
                                              "MC_GUI_SHUFFLE=9", "MC_GUI_SNAPSHOTS=/pics", "MC_GUI_RETRIES=3"};
        const std::string results = (root / "results").string();
        auto runWith = [&](const std::vector<std::string> &suites, int status, int max, std::string *out) {
            GuiRetry::Results r;
            r.complete = true;
            for (const std::string &s : suites) GuiRetry::noteFailure(r, s, s + "Suite.mm:1  it failed");
            GuiRetry::writeResults(results, r);
            const fs::path log = root / "out.txt";
            FILE *f = fopen(log.c_str(), "w");
            const int st = GuiRetry::retryFailedSuites(status, results, max, program.string(), env, f);
            fclose(f);
            std::ifstream in(log);
            std::stringstream ss;
            ss << in.rdbuf();
            *out = ss.str();
            return st;
        };
        std::string out;
        CHECK(runWith({"flaky"}, 1, 3, &out) == 0);
        CHECK(out.find("    retry| suite flaky, retry of flaky, pictures /pics/retry-flaky\n") != std::string::npos);
        // The run's own choices are not passed on; the rest of the environment is.
        CHECK(out.find("    retry| skip= shuffle= keep=yes\n") != std::string::npos);
        CHECK(out.find("    retry| no newline at the end\n") != std::string::npos);
        CHECK(out.find("\n  FLAKY [flaky] flakySuite.mm:1  it failed\n") != std::string::npos);
        CHECK(out.find("Also: 1 suite(s) failed and then passed when retried") != std::string::npos);
        // One that fails again fails the run, and is no FLAKY line.
        CHECK(runWith({"flaky", "broken"}, 1, 3, &out) == 1);
        CHECK(out.find("  FLAKY [flaky]") != std::string::npos);
        CHECK(out.find("  FLAKY [broken]") == std::string::npos);
        CHECK(out.find("  RETRY [broken] failed again in a fresh process (status 1)") != std::string::npos);
        // Past the limit, or with retries off, nothing runs.
        CHECK(runWith({"flaky", "flaky2", "flaky3", "flaky4"}, 1, 3, &out) == 1);
        CHECK(out.find("retry|") == std::string::npos && out.find("No suite retried: 4 suites failed") != std::string::npos);
        CHECK(runWith({"flaky"}, 1, 0, &out) == 1 && out.empty());
        CHECK(runWith({}, 0, 3, &out) == 0 && out.empty());
        // A run that wrote nothing (it crashed) retries nothing.
        fs::remove(results);
        const fs::path log = root / "none.txt";
        FILE *f = fopen(log.c_str(), "w");
        CHECK(GuiRetry::retryFailedSuites(1, results, 3, program.string(), env, f) == 1);
        fclose(f);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
}
