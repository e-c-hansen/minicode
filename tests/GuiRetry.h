// GuiRetry.h — what the two GUI harnesses (mac/tests/gui/Harness.mm,
// linux/tests/gui/Harness.cpp and Wrapper.cpp) share for running their
// suites in a seeded order and for retrying a failed suite once. Plain
// C++17 and POSIX, header only, tested by the core's `gui-retry:*` groups.
//
// A seeded order (MC_GUI_SHUFFLE=<seed>): the suites a run selected are
// played in an order drawn from the seed, the same on every machine and
// both harnesses (splitmix64 and Fisher-Yates here, never std::shuffle,
// whose draws differ between libc++ and libstdc++). The soak workflow
// (.github/workflows/soak.yml) gives each round its own seed and prints it,
// so a round's order is replayed with the same seed and the same suites.
//
// One retry (MC_GUI_RETRIES=<at most this many suites>, set by CI): the
// process playing the suites writes what failed to MC_GUI_RESULTS as it
// ends (formatResults). The process that started it (the Mac's first
// stage, the GTK port's wrapper) reads that, and when every failure was
// inside a suite and no more than that many suites failed, runs each of
// them again, alone, as a fresh run of the whole program in a new scratch
// world (and on GTK a new display), its output marked "    retry| ". A
// suite that passes then is a "  FLAKY [suite] <its first failure>" line
// and the run passes; one that fails again is a "  RETRY [suite]" line and
// the run fails, as it would have. A crash, the run's own time limit, a
// failure outside any suite, or more failed suites than allowed, retries
// nothing. docs/ci.md says why and what a FLAKY line asks for.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace GuiRetry {

// ------------------------------------------------------------ seeded order
inline uint64_t nextRandom(uint64_t &state) {   // splitmix64
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// A seed from MC_GUI_SHUFFLE's text: decimal digits only, so a typo is
// said rather than read as some other number.
inline bool parseSeed(const char *text, uint64_t *seed) {
    if (!text || !*text) return false;
    uint64_t v = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *seed = v;
    return true;
}

// The positions 0..n-1 in the order the seed gives.
inline std::vector<size_t> shuffledOrder(size_t n, uint64_t seed) {
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    uint64_t state = seed;
    for (size_t i = n; i > 1; i--) {
        const size_t j = (size_t)(nextRandom(state) % i);
        std::swap(order[i - 1], order[j]);
    }
    return order;
}

// ------------------------------------------------------------ the results
struct SuiteFailure {
    std::string suite;
    int failed = 0;
    std::string first;   // the first failure, without "  FAIL [suite] "
};

struct Results {
    bool complete = false;   // the run reached its end and wrote everything
    int passed = 0, failed = 0;
    int outside = 0;         // failures in no suite (a name no suite has, the
                             // end's checks, the run's own time limit)
    std::vector<SuiteFailure> suites;   // in the order they first failed
};

// Tabs and line breaks made spaces, so a line is one field of one line.
inline std::string oneLine(const std::string &s) {
    std::string out;
    for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') start++;
    return out.substr(start);
}

// Adds one failure: to its suite when it has one, else as outside.
inline void noteFailure(Results &r, const std::string &suite, const std::string &line) {
    r.failed++;
    if (suite.empty()) {
        r.outside++;
        return;
    }
    for (SuiteFailure &s : r.suites)
        if (s.suite == suite) {
            s.failed++;
            return;
        }
    r.suites.push_back({suite, 1, oneLine(line)});
}

inline std::string formatResults(const Results &r) {
    std::ostringstream out;
    out << "passed\t" << r.passed << "\nfailed\t" << r.failed << "\noutside\t" << r.outside << "\n";
    for (const SuiteFailure &s : r.suites)
        out << "suite\t" << oneLine(s.suite) << "\t" << s.failed << "\t" << oneLine(s.first) << "\n";
    out << "complete\n";
    return out.str();
}

inline Results parseResults(const std::string &text) {
    Results r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        size_t at = 0;
        for (;;) {
            const size_t tab = line.find('\t', at);
            f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        if (f[0] == "complete" && f.size() == 1) r.complete = true;
        else if (f.size() == 2 && f[0] == "passed") r.passed = atoi(f[1].c_str());
        else if (f.size() == 2 && f[0] == "failed") r.failed = atoi(f[1].c_str());
        else if (f.size() == 2 && f[0] == "outside") r.outside = atoi(f[1].c_str());
        else if (f.size() == 4 && f[0] == "suite" && !f[1].empty())
            r.suites.push_back({f[1], atoi(f[2].c_str()), f[3]});
    }
    return r;
}

inline bool writeResults(const std::string &path, const Results &r) {
    if (path.empty()) return false;
    std::ofstream out(path, std::ios::trunc);
    out << formatResults(r);
    return (bool)out;
}

inline Results readResults(const std::string &path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parseResults(ss.str());
}

// ------------------------------------------------------------ the plan
struct Plan {
    std::vector<SuiteFailure> retry;   // the suites to run again
    std::string refusal;               // why none is, when some failed
};

inline Plan planRetries(const Results &r, int maxSuites) {
    Plan p;
    if (!r.complete) {
        p.refusal = "the run did not reach its end (a crash, or stopped from outside), so nothing is retried";
        return p;
    }
    if (r.failed == 0) return p;
    if (r.outside > 0) {
        p.refusal = std::to_string(r.outside) +
                    " check(s) failed outside any suite (the run's time limit, a suite name, the end's "
                    "checks), so nothing is retried";
        return p;
    }
    if (r.suites.empty()) {
        p.refusal = "checks failed but no suite was named, so nothing is retried";
        return p;
    }
    if ((int)r.suites.size() > maxSuites) {
        p.refusal = std::to_string(r.suites.size()) + " suites failed, more than the " +
                    std::to_string(maxSuites) + " a run may retry, so none is retried";
        return p;
    }
    p.retry = r.suites;
    return p;
}

inline std::string flakyLine(const SuiteFailure &s) { return "  FLAKY [" + s.suite + "] " + s.first; }

// ------------------------------------------------------------ the retry
// `env` (NAME=value strings) with every `drop` name taken out.
inline std::vector<std::string> withoutNames(const std::vector<std::string> &env,
                                             const std::vector<std::string> &drop) {
    std::vector<std::string> out;
    for (const std::string &e : env) {
        const std::string name = e.substr(0, e.find('='));
        bool dropped = false;
        for (const std::string &d : drop) dropped = dropped || d == name;
        if (!dropped) out.push_back(e);
    }
    return out;
}

inline std::string envValue(const std::vector<std::string> &env, const std::string &name) {
    for (const std::string &e : env)
        if (e.compare(0, name.size() + 1, name + "=") == 0) return e.substr(name.size() + 1);
    return "";
}

// Runs `argv` with exactly `env`, its stdout and stderr together written to
// `out` a line at a time, each line after `prefix`. The exit status, or 128
// plus the signal that ended it; -1 when it could not start.
inline int runPrefixed(const std::vector<std::string> &argv, const std::vector<std::string> &env,
                       const std::string &prefix, FILE *out) {
    if (argv.empty()) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(out);
    fflush(stdout);
    fflush(stderr);
    std::vector<char *> args, envs;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    for (const std::string &e : env) envs.push_back(const_cast<char *>(e.c_str()));
    envs.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        if (fds[1] > 2) close(fds[1]);
        execve(args[0], args.data(), envs.data());
        _exit(127);
    }
    close(fds[1]);
    std::string pending;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(buf, (size_t)n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            fprintf(out, "%s%s\n", prefix.c_str(), pending.substr(0, nl).c_str());
            pending.erase(0, nl + 1);
        }
        fflush(out);
    }
    if (!pending.empty()) fprintf(out, "%s%s\n", prefix.c_str(), pending.c_str());
    fflush(out);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

// After a run that ended with `status`: reads its results, and retries what
// planRetries allows, each failed suite once as a fresh run of `program`
// with the suite as its argument and `env` (the environment the run itself
// was started with) less the run's own choices. Returns the status the
// whole run ends with: 0 when every failed suite passed its retry.
inline int retryFailedSuites(int status, const std::string &resultsPath, int maxSuites,
                             const std::string &program, const std::vector<std::string> &env,
                             FILE *out) {
    if (status == 0 || maxSuites <= 0) return status;
    const Results results = readResults(resultsPath);
    const Plan plan = planRetries(results, maxSuites);
    if (plan.retry.empty()) {
        if (!plan.refusal.empty()) fprintf(out, "\nNo suite retried: %s.\n", plan.refusal.c_str());
        fflush(out);
        return status;
    }
    const std::string snapshots = envValue(env, "MC_GUI_SNAPSHOTS");
    std::vector<std::string> base =
        withoutNames(env, {"MC_GUI_SUITES", "SUITES", "MC_GUI_SKIP_SUITES", "MC_GUI_SHUFFLE",
                           "MC_GUI_RETRIES", "MC_GUI_RESULTS", "MC_GUI_RETRY_OF", "MC_GUI_SNAPSHOTS",
                           "MC_GUI_CHILD"});
    int passedAgain = 0;
    for (const SuiteFailure &s : plan.retry) {
        fprintf(out, "\nRetrying [%s] once, alone, in a fresh process (it failed %d check(s), first: %s)\n",
                s.suite.c_str(), s.failed, s.first.c_str());
        std::vector<std::string> e = base;
        e.push_back("MC_GUI_RETRY_OF=" + s.suite);
        if (!snapshots.empty()) e.push_back("MC_GUI_SNAPSHOTS=" + snapshots + "/retry-" + s.suite);
        const int st = runPrefixed({program, s.suite}, e, "    retry| ", out);
        if (st == 0) {
            passedAgain++;
            fprintf(out, "%s\n", flakyLine(s).c_str());
        } else {
            fprintf(out, "  RETRY [%s] failed again in a fresh process (status %d)\n", s.suite.c_str(), st);
        }
        fflush(out);
    }
    if (passedAgain == (int)plan.retry.size()) {
        fprintf(out, "Also: %d suite(s) failed and then passed when retried (FLAKY lines above)\n",
                passedAgain);
        fflush(out);
        return 0;
    }
    return status;
}

}  // namespace GuiRetry
