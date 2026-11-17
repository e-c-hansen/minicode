// process_env_tests.inc — the `env:*` groups of run_tests.cpp: the
// environment snapshot every thread that starts a program reads
// (src/ProcessEnv.h), a stress of spawning from worker threads while the
// main thread sets variables, and a scan that keeps the Mac's code and GUI
// checks off raw setenv and environ. Included there.
#include "ProcessEnv.h"
#include <cctype>
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>

static void testProcessEnv() {
    GROUP("env:snapshot");
    {
        ProcessEnv::set("MC_ENV_TEST_A", "one=two");
        auto s = ProcessEnv::current();
        CHECK(s->get("MC_ENV_TEST_A") == std::optional<std::string>("one=two"));
        CHECK(!s->get("MC_ENV_TEST_"));        // a prefix is not the name
        CHECK(!s->get("MC_ENV_TEST_AB"));
        ProcessEnv::set("MC_ENV_TEST_A", nullptr);
        CHECK(!ProcessEnv::current()->has("MC_ENV_TEST_A"));
        CHECK(s->get("MC_ENV_TEST_A").has_value());   // a snapshot taken stays as it was
        CHECK(ProcessEnv::current()->generation > s->generation);

        ProcessEnv::Snapshot base;
        base.entries = {"PATH=/bin", "KEEP=1", "DROP=x"};
        ProcessEnv::Block b(base, {{"PATH", std::string("/usr/bin")},
                                   {"DROP", std::nullopt},
                                   {"NEW", std::string("a")},
                                   {"NEW", std::string("b")}});
        CHECK(b.get("PATH") == std::optional<std::string>("/usr/bin"));
        CHECK(b.get("KEEP") == std::optional<std::string>("1"));
        CHECK(!b.get("DROP"));
        CHECK(b.get("NEW") == std::optional<std::string>("b"));   // the last change wins
        CHECK(b.entries().size() == 3);
        char* const* envp = b.envp();
        size_t n = 0;
        while (envp[n]) n++;
        CHECK(n == 3);
    }

    // The race the GUI checks hit on Linux (2026-11-17): a worker copied
    // environ for a child while the main thread's setenv reallocated it.
    // Here workers start programs from the snapshot while the main thread
    // sets and unsets enough variables to make libc grow and free environ's
    // array many times over. With workers walking environ, ASan reports a
    // use after free and values come back torn; from the snapshot every
    // child gets its variable and every snapshot is whole.
    GROUP("env:spawn-stress");
    {
        std::atomic<bool> done{false};
        std::atomic<int> spawned{0}, failed{0}, torn{0};
        auto worker = [&] {
            int mine = 0;
            while (!done.load() || mine < 8) {
                std::shared_ptr<const ProcessEnv::Snapshot> s = ProcessEnv::current();
                for (const std::string& e : s->entries) {
                    if (e.find('=') == std::string::npos) torn++;
                    // MC_ENV_STRESS_<n> always holds "v<n>v<n>...": a value
                    // read while it was being rewritten would not.
                    if (e.compare(0, 14, "MC_ENV_STRESS_") == 0) {
                        const size_t eq = e.find('=');
                        const std::string n = e.substr(14, eq - 14), v = e.substr(eq + 1);
                        const std::string unit = "v" + n;
                        bool whole = !v.empty() && v.size() % unit.size() == 0;
                        for (size_t i = 0; whole && i < v.size(); i += unit.size())
                            whole = v.compare(i, unit.size(), unit) == 0;
                        if (!whole) torn++;
                    }
                }
                ProcessEnv::Block env(*s, {{"MC_ENV_PROBE", std::string("yes")}});
                const char* argv[] = {"/bin/sh", "-c", "[ \"$MC_ENV_PROBE\" = yes ]", nullptr};
                pid_t pid = 0;
                if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, (char* const*)argv,
                                env.envp()) != 0) {
                    failed++;
                } else {
                    int status = 0;
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed++;
                }
                spawned++;
                mine++;
            }
        };
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; i++) workers.emplace_back(worker);
        for (int round = 0; round < 6; round++) {
            for (int i = 0; i < 120; i++) {
                const std::string n = std::to_string(i);
                std::string v;
                for (int k = 0; k <= (i + round) % 9; k++) v += "v" + n;
                ProcessEnv::set("MC_ENV_STRESS_" + n, v.c_str());
            }
            for (int i = 0; i < 120; i++)
                ProcessEnv::set("MC_ENV_STRESS_" + std::to_string(i), nullptr);
        }
        done = true;
        for (std::thread& t : workers) t.join();
        if (failed || torn)
            std::printf("  %d of %d children failed, %d torn entries\n", failed.load(),
                        spawned.load(), torn.load());
        CHECK(spawned.load() >= 32);
        CHECK(failed.load() == 0);
        CHECK(torn.load() == 0);
        CHECK(!ProcessEnv::current()->has("MC_ENV_STRESS_0"));
    }

    // The Mac's code reads the environment only through the snapshot
    // (mac/src/Environment.h), and the GUI checks change it only with
    // MCSetEnv: a raw setenv in a suite raced with git copying environ on
    // the panel's queue. Comments are left out of the scan.
    GROUP("env:raw-access");
    {
        struct Rule {
            const char* folder;
            std::vector<const char*> words;
        };
        const std::vector<Rule> rules = {
            {"mac/src", {"setenv(", "unsetenv(", "putenv(", "environ", "processInfo.environment",
                         "processInfo] environment"}},
            {"mac/tests/gui", {"setenv(", "unsetenv(", "putenv("}},
        };
        auto isWordChar = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        int scanned = 0;
        for (const Rule& rule : rules) {
            DIR* dir = opendir(rule.folder);
            if (!dir) {
                std::printf("  (%s not found here; env:raw-access skipped)\n", rule.folder);
                continue;
            }
            while (dirent* e = readdir(dir)) {
                const std::string f = e->d_name;
                const bool source = (f.size() > 3 && f.compare(f.size() - 3, 3, ".mm") == 0) ||
                                    (f.size() > 2 && f.compare(f.size() - 2, 2, ".h") == 0) ||
                                    (f.size() > 2 && f.compare(f.size() - 2, 2, ".m") == 0);
                if (!source) continue;
                std::ifstream in(std::string(rule.folder) + "/" + f);
                std::string line;
                int number = 0;
                scanned++;
                while (std::getline(in, line)) {
                    number++;
                    const size_t comment = line.find("//");
                    if (comment != std::string::npos) line.resize(comment);
                    for (const char* w : rule.words) {
                        const std::string word = w;
                        for (size_t at = line.find(word); at != std::string::npos;
                             at = line.find(word, at + 1)) {
                            const bool startOk = at == 0 || !isWordChar(line[at - 1]);
                            const size_t after = at + word.size();
                            const bool endOk = word.back() == '(' || after >= line.size() ||
                                               !isWordChar(line[after]);
                            if (!startOk || !endOk) continue;
                            std::printf("  %s/%s:%d uses %s\n", rule.folder, f.c_str(), number,
                                        w);
                            CHECK(false);
                        }
                    }
                }
            }
            closedir(dir);
        }
        if (scanned) CHECK(scanned > 50);
    }
}
